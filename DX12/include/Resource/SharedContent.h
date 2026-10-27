#pragma once

// Resource/SharedContent.h - a git worktree reads the binary assets it did not check out from
// the MAIN checkout.
//
// `python tools/worktree_content.py share` (run in a worktree) checks out only the TEXT assets
// (scenes, materials, prefabs, scripts, .meta), leaves the binary payloads (.itex, .ianim,
// .meshlib, .fbx, .png, fonts, ...) out with a sparse checkout (except those the branch changed
// and those a scenario pack's writing harness flag names), and writes <DX12>/.shared_content.json
// (untracked):
//
//   {"root": "F:/C++/DX12/DX12Engine/DX12",      the main checkout's DX12 folder
//    "mainHead": "<sha>",                          the commit the mask was taken at
//    "masked": ["asset/x.itex", ...]}              deleted on this branch
//
// Configure() registers that folder as AssetFS's LAST search root, serving only asset/ and
// engine/ paths and never a masked one: a path this checkout has is read here (the branch's
// version wins), one it lacks is read from the main checkout. Nothing is ever written there;
// code that writes a file it resolved uses AssetFS::ResolveLocal.
//
// -no_shared_content boots without it (the A/B switch: a file only the main checkout has then
// fails to resolve); -shared_content=<file> registers another config's root AFTER this checkout's
// own (a test's temporary root). The main checkout itself never has the file, so this is a no-op
// there and in every packaged build.

#include <string>
#include <vector>

namespace Resource::SharedContent
{
    struct Config
    {
        std::string              root;       // "" = no shared root
        std::string              mainHead;
        std::vector<std::string> masked;
        std::string              file;       // where it was read from
    };

    // The prefixes the shared root serves.
    const std::vector<std::string>& Prefixes();

    // Read the config(s) (unless -no_shared_content), validate each, register
    // the roots with AssetFS. Logs a [CONTENT] line per config present.
    // Idempotent. True when a shared root is active.
    bool Configure();

    const Config& Active();   // the first root registered
}
