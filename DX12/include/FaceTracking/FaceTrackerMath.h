#pragma once
// The built-in face tracker's math, aligned with MediaPipe's Face Landmarker graph and held to it by
// tools/face_golden.py (the same frames through Python MediaPipe): the detector's SSD anchors and decoding,
// MediaPipe's weighted non-max suppression, the face's region of interest (from a detection, or from the
// last landmarks while tracking), MediaPipe's ImageToTensor mapping (a rotated square of the image -> the
// model's input), NV12 sampling, projecting the landmarks back, MediaPipe's landmark smoothing (its VIDEO
// mode's One Euro filter) and the head's rotation (a weighted fit of the canonical face; Horn's quaternion
// method). Header-only, DirectXMath only: tests/unit/face_tracker_test.cpp pins it without the engine or
// ONNX Runtime.
//
// Coordinates: image pixels, x right, y down, pixel centres on the integers (OpenCV's, which MediaPipe's
// CPU path uses). An Roi's rotation is MediaPipe's: radians, the rect turned clockwise on screen.

#include "FaceTracking/FaceMath.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace FaceTracking::Tracker {

inline constexpr int kDetectorSize = 128;     // BlazeFace short range: 128 x 128 RGB in [-1, 1]
inline constexpr int kAnchors = 896;
inline constexpr int kDetectorCoords = 16;    // box (x, y, w, h) + 6 keypoints (x, y)
inline constexpr int kLandmarkSize = 256;     // Face Mesh V2: 256 x 256 RGB in [0, 1]
inline constexpr int kLandmarks = 478;        // 468 mesh points + 10 iris points
inline constexpr int kBlendshapes = 52;       // "_neutral" + 51 ARKit names (no tongueOut)
inline constexpr int kBlendshapeInputs = 146;
inline constexpr float kRoiScale = 1.5f;      // the face rect grown 25% each side, squared on its long side
inline constexpr float kNmsIou = 0.3f;

// The landmarks the blendshape model reads, x and y in image pixels (MediaPipe's face_blendshapes_graph.cc
// kLandmarksSubsetIdxs; checked against MediaPipe's own scores by tools/face_golden.py).
inline constexpr std::array<uint16_t, kBlendshapeInputs> kBlendshapeLandmarks = {
    0, 1, 4, 5, 6, 7, 8, 10, 13, 14, 17, 21, 33, 37, 39, 40, 46, 52, 53, 54, 55, 58, 61, 63, 65, 66, 67, 70, 78, 80,
    81, 82, 84, 87, 88, 91, 93, 95, 103, 105, 107, 109, 127, 132, 133, 136, 144, 145, 146, 148, 149, 150, 152, 153, 154,
    155, 157, 158, 159, 160, 161, 162, 163, 168, 172, 173, 176, 178, 181, 185, 191, 195, 197, 234, 246, 249, 251, 263,
    267, 269, 270, 276, 282, 283, 284, 285, 288, 291, 293, 295, 296, 297, 300, 308, 310, 311, 312, 314, 317, 318, 321,
    323, 324, 332, 334, 336, 338, 356, 361, 362, 365, 373, 374, 375, 377, 378, 379, 380, 381, 382, 384, 385, 386, 387,
    388, 389, 390, 397, 398, 400, 402, 405, 409, 415, 454, 466, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477};

// The blendshape model's outputs, in order.
inline constexpr std::array<const char*, kBlendshapes> kBlendshapeNames = {
    "_neutral", "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff",
    "cheekSquintLeft", "cheekSquintRight", "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight",
    "eyeSquintLeft", "eyeSquintRight", "eyeWideLeft", "eyeWideRight", "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight", "mouthFunnel", "mouthLeft",
    "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft", "mouthPressRight", "mouthPucker", "mouthRight",
    "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper", "mouthSmileLeft", "mouthSmileRight",
    "mouthStretchLeft", "mouthStretchRight", "mouthUpperUpLeft", "mouthUpperUpRight", "noseSneerLeft", "noseSneerRight"};

// ==== Detection ====

struct Anchor
{
    float x = 0.0f, y = 0.0f; // the cell's centre, normalised to the detector input
};

// MediaPipe's SsdAnchorsCalculator for face_detection_short_range: 4 layers, strides 8 / 16 / 16 / 16,
// aspect ratio 1 plus the interpolated scale (two anchors per layer and cell), fixed anchor size (1),
// offset 0.5. Layers of one stride share a grid: 16x16x2 + 8x8x6 = 896.
inline std::vector<Anchor> MakeAnchors()
{
    constexpr int strides[4] = {8, 16, 16, 16};
    std::vector<Anchor> out;
    out.reserve(kAnchors);
    int layer = 0;
    while (layer < 4)
    {
        int last = layer;
        while (last + 1 < 4 && strides[last + 1] == strides[layer]) ++last;
        const int perCell = 2 * (last - layer + 1);
        const int grid = (kDetectorSize + strides[layer] - 1) / strides[layer];
        for (int y = 0; y < grid; ++y)
            for (int x = 0; x < grid; ++x)
                for (int k = 0; k < perCell; ++k)
                    out.push_back({(static_cast<float>(x) + 0.5f) / static_cast<float>(grid), (static_cast<float>(y) + 0.5f) / static_cast<float>(grid)});
        layer = last + 1;
    }
    return out;
}

struct Detection
{
    float score = 0.0f;
    float xmin = 0.0f, ymin = 0.0f, xmax = 0.0f, ymax = 0.0f; // normalised to the detector input
    float kp[6][2] = {};                                      // 0, 1: the eyes (the observer's right, left), ...
};

// The detector's raw outputs (regressors [kAnchors x 16], logits [kAnchors]) -> the detections whose score
// (sigmoid of the logit clipped to +-100) is at least minScore. MediaPipe's TensorsToDetections with
// reverse_output_order (x before y) and every scale 128.
inline void DecodeDetections(const float* regressors, const float* logits, const std::vector<Anchor>& anchors, float minScore,
                             std::vector<Detection>& out)
{
    out.clear();
    const int n = static_cast<int>(std::min<size_t>(anchors.size(), static_cast<size_t>(kAnchors)));
    for (int i = 0; i < n; ++i)
    {
        const float logit = std::clamp(logits[i], -100.0f, 100.0f);
        const float score = 1.0f / (1.0f + std::exp(-logit));
        if (!(score >= minScore)) continue;
        const float* r = regressors + static_cast<size_t>(i) * kDetectorCoords;
        const float s = 1.0f / static_cast<float>(kDetectorSize);
        const float cx = r[0] * s + anchors[static_cast<size_t>(i)].x, cy = r[1] * s + anchors[static_cast<size_t>(i)].y;
        const float w = r[2] * s, h = r[3] * s;
        Detection d;
        d.score = score;
        d.xmin = cx - 0.5f * w;
        d.ymin = cy - 0.5f * h;
        d.xmax = cx + 0.5f * w;
        d.ymax = cy + 0.5f * h;
        for (int k = 0; k < 6; ++k)
        {
            d.kp[k][0] = r[4 + 2 * k] * s + anchors[static_cast<size_t>(i)].x;
            d.kp[k][1] = r[5 + 2 * k] * s + anchors[static_cast<size_t>(i)].y;
        }
        out.push_back(d);
    }
}

inline float Iou(const Detection& a, const Detection& b)
{
    const float ix = std::max(0.0f, std::min(a.xmax, b.xmax) - std::max(a.xmin, b.xmin));
    const float iy = std::max(0.0f, std::min(a.ymax, b.ymax) - std::max(a.ymin, b.ymin));
    const float inter = ix * iy;
    const float uni = (a.xmax - a.xmin) * (a.ymax - a.ymin) + (b.xmax - b.xmin) * (b.ymax - b.ymin) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// MediaPipe's WEIGHTED non-max suppression, kept to the best group (one face): the top detection's score,
// with the score-weighted mean box and keypoints of every detection overlapping it by more than iou.
inline bool BestWeighted(std::vector<Detection>& dets, float iou, Detection& out)
{
    if (dets.empty()) return false;
    std::stable_sort(dets.begin(), dets.end(), [](const Detection& a, const Detection& b) { return a.score > b.score; });
    const Detection& top = dets.front();
    Detection sum;
    float wsum = 0.0f;
    for (const Detection& d : dets)
    {
        if (&d != &top && !(Iou(top, d) > iou)) continue;
        const float w = d.score;
        wsum += w;
        sum.xmin += w * d.xmin;
        sum.ymin += w * d.ymin;
        sum.xmax += w * d.xmax;
        sum.ymax += w * d.ymax;
        for (int k = 0; k < 6; ++k)
        {
            sum.kp[k][0] += w * d.kp[k][0];
            sum.kp[k][1] += w * d.kp[k][1];
        }
    }
    out = sum;
    out.score = top.score;
    out.xmin /= wsum;
    out.ymin /= wsum;
    out.xmax /= wsum;
    out.ymax /= wsum;
    for (int k = 0; k < 6; ++k)
    {
        out.kp[k][0] /= wsum;
        out.kp[k][1] /= wsum;
    }
    return true;
}

// ==== The region of interest ====

struct Roi
{
    float cx = 0.0f, cy = 0.0f; // image pixels
    float side = 0.0f;          // a square, pixels
    float rotation = 0.0f;      // radians, MediaPipe's sign (clockwise on screen)
};

inline float NormalizeRadians(float a)
{
    return a - 2.0f * FaceMath::kPi * std::floor((a + FaceMath::kPi) / (2.0f * FaceMath::kPi));
}

// MediaPipe's DetectionsToRects rotation: the vector from one keypoint to another, target angle 0.
inline float RotationFromPoints(float x0, float y0, float x1, float y1)
{
    return NormalizeRadians(-std::atan2(-(y1 - y0), x1 - x0));
}

// The detector's input: the whole image, centred in a square of side max(w, h), unrotated.
inline Roi LetterboxRoi(int imageW, int imageH)
{
    Roi r;
    r.cx = 0.5f * static_cast<float>(imageW);
    r.cy = 0.5f * static_cast<float>(imageH);
    r.side = static_cast<float>(std::max(imageW, imageH));
    return r;
}

// A detection (normalised to the letterboxed input) -> the landmark model's Roi: the box's centre, its
// long side x 1.5, turned by the line from keypoint 0 to keypoint 1.
inline Roi RoiFromDetection(const Detection& d, int imageW, int imageH)
{
    const Roi lb = LetterboxRoi(imageW, imageH);
    auto px = [&](float tx, float ty, float& x, float& y) {
        x = lb.cx + (tx - 0.5f) * lb.side;
        y = lb.cy + (ty - 0.5f) * lb.side;
    };
    float x0, y0, x1, y1, k0x, k0y, k1x, k1y;
    px(d.xmin, d.ymin, x0, y0);
    px(d.xmax, d.ymax, x1, y1);
    px(d.kp[0][0], d.kp[0][1], k0x, k0y);
    px(d.kp[1][0], d.kp[1][1], k1x, k1y);
    Roi r;
    r.cx = 0.5f * (x0 + x1);
    r.cy = 0.5f * (y0 + y1);
    r.side = std::max(x1 - x0, y1 - y0) * kRoiScale;
    r.rotation = RotationFromPoints(k0x, k0y, k1x, k1y);
    return r;
}

// Tracking: the last landmarks (x, y, z in pixels, kLandmarks of them) -> the next frame's Roi: their box's
// centre, its long side x 1.5, turned by the line from landmark 33 to landmark 263 (the eyes' outer corners).
inline Roi RoiFromLandmarks(const float* xyz)
{
    float x0 = xyz[0], x1 = xyz[0], y0 = xyz[1], y1 = xyz[1];
    for (int i = 1; i < kLandmarks; ++i)
    {
        x0 = std::min(x0, xyz[i * 3]);
        x1 = std::max(x1, xyz[i * 3]);
        y0 = std::min(y0, xyz[i * 3 + 1]);
        y1 = std::max(y1, xyz[i * 3 + 1]);
    }
    Roi r;
    r.cx = 0.5f * (x0 + x1);
    r.cy = 0.5f * (y0 + y1);
    r.side = std::max(x1 - x0, y1 - y0) * kRoiScale;
    r.rotation = RotationFromPoints(xyz[33 * 3], xyz[33 * 3 + 1], xyz[263 * 3], xyz[263 * 3 + 1]);
    return r;
}

// MediaPipe's ImageToTensor (OpenCV path: the rect's corners onto the output's corners, warpPerspective):
// output pixel (u, v) of a size x size tensor -> the image point it samples.
inline void TensorToImage(const Roi& roi, int size, float u, float v, float& x, float& y)
{
    const float c = std::cos(roi.rotation), s = std::sin(roi.rotation);
    const float dx = (u / static_cast<float>(size) - 0.5f) * roi.side, dy = (v / static_cast<float>(size) - 0.5f) * roi.side;
    x = roi.cx + c * dx - s * dy;
    y = roi.cy + s * dx + c * dy;
}

// What a sample outside the image takes (OpenCV's border modes, as MediaPipe's ImageToTensor sets them).
enum class Border
{
    Zero,      // black (BORDER_CONSTANT 0): the detector's letterbox
    Replicate, // the nearest edge pixel (BORDER_REPLICATE): the landmark model's crop
};

// An NV12 image (BT.601 limited range, what decoders and webcams give) sampled through the Roi into an RGB
// float tensor (NHWC, size x size x 3): value = rgb / 255 * scale + bias. Bilinear over the four nearest
// pixels, each with its 2x2 block's chroma, as OpenCV's warp of the decoded RGB image does; a pixel outside
// the image is black or the nearest edge pixel's (`border`).
inline void SampleNv12(const uint8_t* yPlane, const uint8_t* uvPlane, int width, int height, const Roi& roi, int size, float scale,
                       float bias, float* out, Border border = Border::Zero)
{
    const float c = std::cos(roi.rotation), s = std::sin(roi.rotation);
    const float step = roi.side / static_cast<float>(size);
    const float k = scale / 255.0f;
    for (int v = 0; v < size; ++v)
    {
        const float dy = (static_cast<float>(v) / static_cast<float>(size) - 0.5f) * roi.side;
        float x = roi.cx + c * (-0.5f * roi.side) - s * dy;
        float y = roi.cy + s * (-0.5f * roi.side) + c * dy;
        for (int u = 0; u < size; ++u, x += c * step, y += s * step)
        {
            const float fx = std::floor(x), fy = std::floor(y);
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const float ax = x - fx, ay = y - fy;
            float Y = 0.0f, U = 0.0f, V = 0.0f;
            for (int tap = 0; tap < 4; ++tap)
            {
                int tx = x0 + (tap & 1), ty = y0 + (tap >> 1);
                const float w = ((tap & 1) ? ax : 1.0f - ax) * ((tap >> 1) ? ay : 1.0f - ay);
                if (tx < 0 || ty < 0 || tx >= width || ty >= height)
                {
                    if (border == Border::Zero)
                    {
                        Y += w * 16.0f; // black: Y 16, U = V = 128
                        U += w * 128.0f;
                        V += w * 128.0f;
                        continue;
                    }
                    tx = std::clamp(tx, 0, width - 1);
                    ty = std::clamp(ty, 0, height - 1);
                }
                Y += w * static_cast<float>(yPlane[static_cast<size_t>(ty) * static_cast<size_t>(width) + static_cast<size_t>(tx)]);
                const size_t ci = static_cast<size_t>(ty / 2) * static_cast<size_t>(width) + static_cast<size_t>(tx & ~1);
                U += w * static_cast<float>(uvPlane[ci]);
                V += w * static_cast<float>(uvPlane[ci + 1]);
            }
            const float C = 1.164383f * (Y - 16.0f), D = U - 128.0f, E = V - 128.0f;
            const float r = std::clamp(C + 1.596027f * E, 0.0f, 255.0f);
            const float g = std::clamp(C - 0.391762f * D - 0.812968f * E, 0.0f, 255.0f);
            const float b = std::clamp(C + 2.017232f * D, 0.0f, 255.0f);
            float* p = out + (static_cast<size_t>(v) * static_cast<size_t>(size) + static_cast<size_t>(u)) * 3;
            p[0] = r * k + bias;
            p[1] = g * k + bias;
            p[2] = b * k + bias;
        }
    }
}

// The landmark model's output (x, y in the 256 x 256 crop's pixels, z on the crop's scale) -> image
// pixels, z in pixels too (MediaPipe's LandmarkProjection: z scales as x).
inline void ProjectLandmarks(const float* raw, const Roi& roi, float* xyz)
{
    const float c = std::cos(roi.rotation), s = std::sin(roi.rotation);
    const float inv = 1.0f / static_cast<float>(kLandmarkSize);
    for (int i = 0; i < kLandmarks; ++i)
    {
        const float nx = raw[i * 3] * inv - 0.5f, ny = raw[i * 3 + 1] * inv - 0.5f;
        xyz[i * 3] = (c * nx - s * ny) * roi.side + roi.cx;
        xyz[i * 3 + 1] = (s * nx + c * ny) * roi.side + roi.cy;
        xyz[i * 3 + 2] = raw[i * 3 + 2] * inv * roi.side;
    }
}

// ==== Smoothing ====

// MediaPipe's landmark smoothing in its VIDEO mode (LandmarksSmoothingCalculator, OneEuroFilter: min cutoff
// 0.05, beta 80, derivative cutoff 1; the speed in face sizes a second): matches its output to 0.05 px (p95)
// on tools/face_golden.py's video. One filter per coordinate; the rate comes from the timestamps.
class LandmarkSmoother
{
public:
    static constexpr double kMinCutoff = 0.05, kBeta = 80.0, kDerivativeCutoff = 1.0, kDefaultHz = 30.0;

    void Reset()
    {
        m_x.fill(Low{});
        m_dx.fill(Low{});
        m_lastUs = 0;
        m_hz = kDefaultHz;
    }

    // In place: kLandmarks x (x, y, z) pixels at tUs. As MediaPipe's filter, a sample no newer than the last
    // one passes through untouched - the first one too when it is at time 0 (a video's first frame).
    void Apply(int64_t tUs, float* xyz)
    {
        if (m_lastUs >= tUs) return;
        if (m_lastUs != 0 && tUs != 0) m_hz = 1e6 / static_cast<double>(tUs - m_lastUs);
        m_lastUs = tUs;
        float x0 = xyz[0], x1 = xyz[0], y0 = xyz[1], y1 = xyz[1];
        for (int i = 1; i < kLandmarks; ++i)
        {
            x0 = std::min(x0, xyz[i * 3]);
            x1 = std::max(x1, xyz[i * 3]);
            y0 = std::min(y0, xyz[i * 3 + 1]);
            y1 = std::max(y1, xyz[i * 3 + 1]);
        }
        const double scale = 1.0 / std::max(1e-6, 0.5 * (static_cast<double>(x1 - x0) + static_cast<double>(y1 - y0)));
        const double alphaD = Alpha(kDerivativeCutoff);
        for (int k = 0; k < kLandmarks * 3; ++k)
        {
            const double value = xyz[k];
            const double d = m_x[k].init ? (value - m_x[k].raw) * scale * m_hz : 0.0;
            const double ed = m_dx[k].Apply(d, alphaD);
            xyz[k] = static_cast<float>(m_x[k].Apply(value, Alpha(kMinCutoff + kBeta * std::fabs(ed))));
        }
    }

private:
    struct Low
    {
        double raw = 0.0, stored = 0.0;
        bool init = false;
        double Apply(double v, double alpha)
        {
            stored = init ? alpha * v + (1.0 - alpha) * stored : v;
            init = true;
            raw = v;
            return stored;
        }
    };
    double Alpha(double cutoff) const
    {
        const double te = 1.0 / m_hz, tau = 1.0 / (2.0 * 3.14159265358979323846 * cutoff);
        return 1.0 / (1.0 + tau / te);
    }
    std::array<Low, kLandmarks * 3> m_x{}, m_dx{};
    int64_t m_lastUs = 0;
    double m_hz = kDefaultHz;
};

// ==== The head ====

// The symmetric 4x4 eigenvector of the largest eigenvalue (cyclic Jacobi).
inline void LargestEigenvector4(double a[4][4], double out[4])
{
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        double off = 0.0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-24) break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
            {
                if (std::fabs(a[p][q]) < 1e-30) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 4; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 4; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 4; ++k)
                {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k)
        if (a[k][k] > a[best][best]) best = k;
    for (int k = 0; k < 4; ++k) out[k] = v[k][best];
}

// The rotation that best turns the points `from` onto `to` (weighted least squares, centroids removed; Horn's
// closed form): a quaternion (x, y, z, w) with w >= 0 acting as q v q* (FaceMath::Rotate).
inline DirectX::XMFLOAT4 FitRotation(const float* from, const float* to, const float* weights, int count)
{
    double cf[3] = {}, ct[3] = {}, wsum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double w = weights[i];
        wsum += w;
        for (int k = 0; k < 3; ++k)
        {
            cf[k] += w * from[i * 3 + k];
            ct[k] += w * to[i * 3 + k];
        }
    }
    if (!(wsum > 0.0)) return {0.0f, 0.0f, 0.0f, 1.0f};
    for (int k = 0; k < 3; ++k)
    {
        cf[k] /= wsum;
        ct[k] /= wsum;
    }
    double S[3][3] = {}; // S[a][b] = sum w (from_a)(to_b)
    for (int i = 0; i < count; ++i)
    {
        const double w = weights[i];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) S[a][b] += w * (from[i * 3 + a] - cf[a]) * (to[i * 3 + b] - ct[b]);
    }
    double N[4][4] = {
        {S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
        {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
        {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
        {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
    double e[4];
    LargestEigenvector4(N, e); // (w, x, y, z)
    const double n = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
    const double sgn = e[0] < 0.0 ? -1.0 : 1.0;
    return {static_cast<float>(sgn * e[1] / n), static_cast<float>(sgn * e[2] / n), static_cast<float>(sgn * e[3] / n),
            static_cast<float>(sgn * e[0] / n)};
}

// A rotation in the camera's frame (right-handed: x the image's right = the subject's LEFT, y up, z toward
// the camera) -> the same turn in FaceMath's subject frame (R = +X the subject's right, U = +Y, F = +Z): the
// frames differ by x -> -x, so the axis (x, y, z) becomes (x, -y, -z).
inline DirectX::XMFLOAT4 CameraToSubject(const DirectX::XMFLOAT4& q)
{
    return {q.x, -q.y, -q.z, q.w};
}

// The scale of the similarity that best maps `from` onto `to` once `rotation` (FitRotation's) turns it:
// sum w (R (f - f0)) . (t - t0) / sum w |f - f0|^2 (MediaPipe's procrustes solver; 0 when degenerate).
inline double FitScale(const float* from, const float* to, const float* weights, int count, const DirectX::XMFLOAT4& rotation)
{
    double cf[3] = {}, ct[3] = {}, wsum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double w = weights[i];
        wsum += w;
        for (int k = 0; k < 3; ++k)
        {
            cf[k] += w * from[i * 3 + k];
            ct[k] += w * to[i * 3 + k];
        }
    }
    if (!(wsum > 0.0)) return 0.0;
    for (int k = 0; k < 3; ++k)
    {
        cf[k] /= wsum;
        ct[k] /= wsum;
    }
    const double qx = rotation.x, qy = rotation.y, qz = rotation.z, qw = rotation.w;
    double num = 0.0, den = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double w = weights[i];
        const double v[3] = {from[i * 3] - cf[0], from[i * 3 + 1] - cf[1], from[i * 3 + 2] - cf[2]};
        // v' = v + 2 qw (u x v) + 2 u x (u x v), u = (qx, qy, qz)
        const double c1[3] = {qy * v[2] - qz * v[1], qz * v[0] - qx * v[2], qx * v[1] - qy * v[0]};
        const double c2[3] = {qy * c1[2] - qz * c1[1], qz * c1[0] - qx * c1[2], qx * c1[1] - qy * c1[0]};
        for (int k = 0; k < 3; ++k)
        {
            num += w * (v[k] + 2.0 * qw * c1[k] + 2.0 * c2[k]) * (to[i * 3 + k] - ct[k]);
            den += w * v[k] * v[k];
        }
    }
    return den > 1e-12 ? num / den : 0.0;
}

// MediaPipe's face geometry environment for the Face Landmarker (geometry_pipeline_metadata_landmarks with
// the task's default environment): the image's origin at its top-left, a perspective camera of 63 degrees
// vertical field of view, near plane 1. Its mesh is the first 468 landmarks (the irises' ten are not in it).
constexpr float kGeometryFovYDeg = 63.0f;
constexpr float kGeometryNear = 1.0f;
constexpr int kGeometryMeshPoints = 468;

// The head's turn from the landmarks (image pixels, z on x's scale; imageW x imageH): MediaPipe's
// screen-to-metric conversion (geometry_pipeline.cc) - the landmarks onto the near plane, the face's scale
// estimated twice, the landmarks unprojected through the camera to that scale - then the canonical face's
// procrustes points fitted onto them (weighted, MediaPipe's basis). A face off the image's centre is seen a
// little from the side by that camera; a fit on the image's own pixels would read where the face is as how
// it is turned (about 4 deg of yaw per 100 px at 640 x 480). FaceMath's frame.
inline DirectX::XMFLOAT4 HeadFromLandmarks(const float* xyz, int imageW, int imageH, const uint16_t* ids, const float* canonical,
                                           const float* weights, int count)
{
    const double W = std::max(imageW, 1), H = std::max(imageH, 1);
    const double heightAtNear = 2.0 * kGeometryNear * std::tan(0.5 * kGeometryFovYDeg * FaceMath::kPi / 180.0);
    const double widthAtNear = W * heightAtNear / H;
    // ProjectXY: x and y onto the near plane (y up), z on x's scale.
    auto projected = [&](int i, double& outX, double& outY, double& outZ) {
        const float* p = xyz + static_cast<size_t>(i) * 3;
        outX = p[0] / W * widthAtNear - 0.5 * widthAtNear;
        outY = (1.0 - p[1] / H) * heightAtNear - 0.5 * heightAtNear;
        outZ = p[2] / W * widthAtNear;
    };
    double depthOffset = 0.0;
    for (int i = 0; i < kGeometryMeshPoints; ++i)
    {
        double X, Y, Z;
        projected(i, X, Y, Z);
        depthOffset += Z;
    }
    depthOffset /= kGeometryMeshPoints;
    std::vector<float> to(static_cast<size_t>(count) * 3);
    // scale <= 0: the projected points with the handedness changed (the first pass); otherwise z moved to
    // the face's depth at that scale and x, y unprojected along their rays (MoveAndRescaleZ, UnprojectXY).
    auto place = [&](double scale) {
        for (int k = 0; k < count; ++k)
        {
            double X, Y, Z;
            projected(ids[k], X, Y, Z);
            float* t = to.data() + static_cast<size_t>(k) * 3;
            if (scale <= 0.0)
            {
                t[0] = static_cast<float>(X);
                t[1] = static_cast<float>(Y);
                t[2] = static_cast<float>(-Z);
                continue;
            }
            const double z = (Z - depthOffset + kGeometryNear) / scale;
            t[0] = static_cast<float>(X * z / kGeometryNear);
            t[1] = static_cast<float>(Y * z / kGeometryNear);
            t[2] = static_cast<float>(-z);
        }
    };
    auto scaleOf = [&]() { return FitScale(canonical, to.data(), weights, count, FitRotation(canonical, to.data(), weights, count)); };
    place(0.0);
    const double first = scaleOf();
    if (first > 1e-9)
    {
        place(first);
        const double second = scaleOf();
        if (second > 1e-9) place(first * second);
    }
    return CameraToSubject(FitRotation(canonical, to.data(), weights, count));
}

} // namespace FaceTracking::Tracker
