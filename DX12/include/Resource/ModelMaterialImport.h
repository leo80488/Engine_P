#pragma once
// Resource/ModelMaterialImport - a model's materials from its SOURCE, shared by the importers:
//
//   * ModelTextureCook - the source's images (a file beside the model or one embedded in it)
//     cooked once each into <stem>/Textures/<name>.itex, named for the .imat by the
//     project-relative folder the model is written under. Colour maps BC7 sRGB, normal maps BC5 as
//     the source has them (an OpenGL-convention map is right as it is through assimp's path; the
//     green flip is for a DirectX-convention source), masks BC4, and the engine's ARM map
//     (R = occlusion, G = roughness, B = metalness) composed from whatever images carry those.
//   * FillMaterialFromAssimp - an assimp material's factors, alpha mode, two-sidedness and
//     textures onto a MaterialComponent (SceneImporter: glTF / FBX / OBJ / VRM). PmxImporter fills
//     its own (MMD's fields).
//
// Editor-only (EditorCore), like the importers. A cook with no folder cooks nothing and leaves the
// textures to the defaults - for a caller that does not write the files where an .imat would look.

#include "ECS/Components.h"   // MaterialComponent

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct aiMaterial;
struct aiScene;

namespace Resource
{
    class ModelTextureCook
    {
    public:
        // One source image: its identity, a file name (the stem names the .itex,
        // the extension picks the decoder) and its bytes.
        struct Image
        {
            std::string          key;
            std::string          name;
            std::vector<uint8_t> bytes;
            bool Valid() const { return !bytes.empty(); }
        };

        enum class Kind { Color, Normal, Mask };   // BC7 sRGB / BC5 / BC4
        struct Result
        {
            std::string path;               // the .imat reference; "" = not cooked (warned)
            bool        hasAlpha = false;   // @p probeAlpha: some texel is not opaque
        };

        struct Channel { const Image* img = nullptr; int channel = 0; };

        // @p refRoot: the project-relative folder the importer's files are written
        // under (each file's relativePath begins "<stem>/"); "" cooks nothing.
        // @p sourceDir: the folder the source's texture paths are relative to.
        ModelTextureCook(std::string refRoot, std::string stem, std::string sourceDir);
        bool Enabled() const { return !m_refRoot.empty(); }

        // A file the source names - relative to its folder or absolute, either
        // separator; an absolute path that is not there is looked for by its
        // file name in the source's folder (an author's machine path).
        Image FromFile(const std::string& path) const;
        // An assimp texture reference: embedded ("*N", or a name the scene
        // embeds) or a file.
        Image FromAssimp(const aiScene& scene, const std::string& path) const;

        // Cooked once per (image, kind, green flip) whatever asks.
        Result Cook(const Image& img, Kind kind, bool flipGreen = false, bool probeAlpha = false);

        // The engine's ARM from up to three images: a channel of each (0 = R ..
        // 3 = A); a missing one is written 255 (occlusion none, and the full
        // roughness / metalness range - the material's factors scale them).
        // Sized as the first image present; the others are resized to it.
        std::string CookSurface(Channel occlusion, Channel roughness, Channel metalness, const std::string& name);

        // What was cooked, "<stem>/Textures/<file>.itex" -> blob, for the
        // importer's file list; and the counts for its log line.
        std::vector<std::pair<std::string, std::vector<uint8_t>>>& Files() { return m_files; }
        uint32_t Cooked() const { return static_cast<uint32_t>(m_files.size()); }
        uint32_t Failed() const { return m_failed; }

    private:
        std::string UniqueFileName(const std::string& stem);
        std::string Emit(const std::string& fileStem, std::vector<uint8_t>&& itex);

        std::string m_refRoot, m_stem, m_sourceDir;
        std::unordered_map<std::string, Result> m_done;    // image key | kind -> what it became
        std::unordered_map<std::string, int>    m_taken;   // file stems in use (lower case)
        std::vector<std::pair<std::string, std::vector<uint8_t>>> m_files;
        uint32_t m_failed = 0;
    };

    // What FillMaterialFromAssimp found, for the importer's log.
    struct ImportedMaterialNotes
    {
        uint32_t textured      = 0;   // materials given at least one texture
        uint32_t missingImages = 0;   // texture references that did not resolve
        uint32_t otherUvSet    = 0;   // textures on a UV set other than 0 (sampled at 0)
        uint32_t transmissive  = 0;   // glTF materials that transmit (BlendMode::Transmissive)
        uint32_t transmissionMaps = 0;   // ... of them with a transmission / thickness map (not carried)
    };

    // An assimp material onto @p out (default-constructed by the caller, its name already set): base
    // colour, opacity and alpha mode (glTF OPAQUE / MASK / BLEND; elsewhere an opacity below 1 or an
    // opacity map), two-sidedness, metallic / roughness, emissive, and the textures through @p cook.
    // @p gltf: the source's colours are LINEAR (written sRGB, as the engine stores them) and its
    // factors PBR - normal maps are taken as they are (see the .cpp); @p vertexColors: a mesh using
    // the material has COLOR_0 (glTF multiplies the base colour by it); @p flipGreen: the source's
    // normal maps are DirectX-convention (the flipGreen import option).
    void FillMaterialFromAssimp(const aiMaterial& src, const aiScene& scene, bool gltf, bool vertexColors,
                                ModelTextureCook& cook, MaterialComponent& out, ImportedMaterialNotes& notes,
                                bool flipGreen = false);
}
