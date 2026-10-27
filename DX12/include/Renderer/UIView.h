#pragma once

// UIView - one off-screen picture of a Canvas UI subtree (the UI Editor's designer
// view). Plain data: what to draw and how, and what the service reports back.
// UIViewService.h owns the pictures.

#include "ECS/ECS.h"            // Entity
#include "UI/UICanvas.h"        // UI::UICanvas (a node root's preview scaler)
#include "UI/UICanvasLayout.h"  // UI::StatePreview

#include <cstdint>
#include <string>
#include <vector>

namespace UIView
{
    // What the design frame shows behind the UI.
    enum class Background : std::uint8_t { Checker = 0, Solid, None };

    // How a UIRect-rooted subject (a UI template under no canvas) is framed.
    // Fill: the root's parent rect is the design screen (it lays out like a child of a
    // canvas). Custom: the root itself is forced to `size` (reference pixels, scaled by
    // the preview scaler) and centered - for a settings row, whose root rect is written
    // by script at runtime.
    struct RootFrame
    {
        enum class Mode : std::uint8_t { Fill = 0, Custom } mode = Mode::Fill;
        UI::Vec2 size{ 640.f, 34.f };
    };

    struct Desc
    {
        // ---- What ----
        Entity        subject    = NullEntity;   // a UICanvas root, or a UIRect root
        // World::Epoch() when the request was latched. Entity ids carry no
        // generation (ECS.h), so after a world swap the same id can name another
        // entity: a request from an older epoch draws nothing.
        std::uint32_t worldEpoch = 0;
        UI::Vec2      designSize{ 1920.f, 1080.f }; // the screen the subject is laid out for
        RootFrame     frame;                       // UIRect root only
        bool          hasScaler  = false;          // UIRect root: use `scaler` (else defaults)
        UI::UICanvas  scaler;

        // ---- How ----
        std::uint32_t targetW = 0, targetH = 0;    // the view box in pixels; 0 = no picture
        float         zoom    = 1.f;               // design px -> view px
        UI::Vec2      pan{ 0.f, 0.f };             // view px of design (0,0)
        Background    background    = Background::Checker;
        std::uint32_t solidRgba     = 0xFF202020u; // Color32 packing (0xAABBGGRR)
        std::uint32_t workspaceRgba = 0xFF141414u; // outside the design frame
        bool          clipToFrame   = true;        // cut what leaves the frame, as the screen does
        UI::StatePreview state       = UI::StatePreview::Live;
        Entity           stateTarget = NullEntity; // NullEntity = every interactable
        bool          showHidden = false;          // lay out visible=false nodes, paint them ghosted
        bool          realtime   = true;           // advance the view's UI clock (jitter, shaders)
        // Hidden in the designer (UMG's eye; designer-only, never saved): these
        // nodes and their subtrees are laid out - the rest keeps its place - but
        // not painted.
        std::vector<Entity> skip;

        // Keep the target's picture as it is: nothing is laid out, painted or recorded (a
        // subject set aside for Play; the designer shows its last picture until it comes
        // back).
        bool freeze = false;

        // The requester's number for this description (any increasing value),
        // echoed as Status::paintedSerial: a shot waits until the target holds
        // the picture of its own description (Status::current).
        std::uint64_t serial = 0;

        // The UI Editor packs' reverse arms (UIEditorWindow::HarnessConfigure), each
        // breaking exactly what one pack proves; 0 in every real use.
        std::uint8_t  debug = 0;   // kDebug* bits
    };
    constexpr std::uint8_t kDebugRunRectOff    = 1;   // the view transform leaves text-run rects in design px
    constexpr std::uint8_t kDebugIdleRecord    = 2;   // record every frame: an idle designer costs a draw
    constexpr std::uint8_t kDebugWriteComputed = 4;   // the designer's layout overwrites UIComputedRect

    // A custom text shader's run rect as the view draws it (view px origin, the
    // reciprocal extent): uieditor_zoom_text checks the zoom transformed it.
    struct RunRect { float x = 0.f, y = 0.f, invW = 0.f, invH = 0.f; };

    struct Status
    {
        bool          live      = false;
        bool          hasOutput = false;           // a target exists and has been drawn
        std::uint32_t width = 0, height = 0;       // the target
        std::uint64_t renders = 0;                 // GPU records so far (the idle check reads it)
        std::uint64_t lastRenderFrame = 0;
        std::uint64_t paints = 0;                  // CPU paints so far
        std::uint32_t nodes = 0;                   // laid out
        std::uint32_t verts = 0, indices = 0, cmds = 0, effects = 0;
        bool          truncated = false;           // the uint16 index cap was reached
        std::uint64_t designHash = 0;              // the draw list in design pixels (before the view transform)
        float         canvasScale = 1.f;
        UI::Vec2      designSize{ 0.f, 0.f };      // the screen the last paint laid out for
        // Images of the last paint whose texture was not resident yet (drawn as
        // nothing): a picture with any is not final.
        std::uint32_t pendingImages = 0;
        std::uint64_t paintedSerial = 0;           // Desc::serial of the last paint
        RunRect       runRects[4];                 // the first custom-text run rects (view px)
        std::uint32_t runRectCount = 0;
        // The target holds exactly the last paint's pixels (recorded, or nothing
        // to record because they did not change). With paintedSerial: the picture
        // on the target is the one of that description.
        bool          current = false;
        std::string   why;                         // why nothing (or only the backdrop) is drawn; "" = fine
    };
}
