#pragma once

// MaterialInstanceRepair - finds and (optionally) removes the accidental override marks an
// inferred-mark .imatinst (no imatfmt = 2) carries.
//
// Such a file's key set was derived at save time by comparing its values against the
// resolved parent. For a custom-shader material that comparison is wrong: the parent's bag
// holds only the keys literally in its own text, while the child's is filled with EVERY
// reflected //@ parameter by MaterialReflectionSync, so nearly every parameter was written
// as an override the artist never made, pinned against later edits to the master.
//
// The loader cannot fix this alone: it derives the mark set of such a file from the keys
// the file authors, and these keys ARE authored. Telling them apart from a real override
// needs the shader's declared default, which only a Reflection has.
//
// DRY RUN BY DEFAULT: dropping a mark is a creative decision made on the artist's behalf,
// so the safe version reports first. With apply=true every rewritten file gets a
// `<name>.imatinst.bak` beside it.
//
// Editor-only: it needs a ShaderLibrary to compile and reflect a shader no live entity
// uses.

#include <string>
#include <vector>

class ShaderLibrary;
class Renderer;

namespace Editor
{
    struct MaterialInstanceRepairReport
    {
        struct Item
        {
            std::string              path;
            std::string              parentPath;
            // Marks that look accidental: the key names a custom-shader parameter, no ancestor
            // authored it, and its value equals the shader's declared default. Empty when the file is
            // healthy.
            std::vector<std::string> droppable;
            // Non-empty when the file was examined but deliberately left alone.
            std::string              skipReason;
            bool                     rewritten = false;
        };

        std::vector<Item> items;
        int scanned   = 0;
        int flagged   = 0;   // files with at least one droppable mark
        int rewritten = 0;   // files actually written (apply only)
        int skipped   = 0;
    };

    // Walks every .imatinst under @p root. With @p apply false nothing is written.
    //
    // A file is SKIPPED, never touched, when:
    //   * it has no parent key (flattened: nothing records its parent, so re-pointing it is
    //     authoring, not migration)
    //   * its parent chain does not resolve (the values are defaults, so "equals the default"
    //     would mean nothing)
    //   * it already carries imatfmt = 2 (its marks are authored, not inferred)
    //   * it uses no custom shader, or that shader will not compile
    //   * its shader is not registered and no @p renderer was supplied
    //
    // @p lib is the renderer's material library, used READ-ONLY: shader ids are looked up with
    // ShaderLibrary::FindDynamic, and reflections compiled there.
    // @p renderer is optional and is how a shader nothing has registered yet gets its id
    // (Renderer::RegisterMaterialShader, one registry for every library). Pass it whenever you
    // have one: without it such a shader cannot be reflected and its file is reported as
    // skipped. MAIN/GAME THREAD ONLY when it is non-null.
    MaterialInstanceRepairReport RepairMaterialInstances(
        ShaderLibrary& lib, const std::string& root, bool apply,
        Renderer* renderer = nullptr);
}
