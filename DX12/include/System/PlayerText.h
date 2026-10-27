#pragma once

// System/PlayerText.h - the few lines C++ itself shows a player (the error dialogs, the splash, the
// boot's loading line), in the player's language. The game's own text is the script layer's
// (asset/scripts/loc/*.lua, the same language codes); these must work before it runs and after it
// is gone.
//
// And what went wrong, for main's last dialog: the device notes a lost device or a GPU below the
// minimum (NotePlayerError) where it finds one, and main's error handler says that instead of
// "a DX12 call failed".

#include <string>

namespace Sys
{
    enum class PlayerText
    {
        PreparingShaders,   // %d percent
        Loading,
        FatalTitle,         // %s product
        CrashWithFolder,    // %s crash folder, %s reason
        CrashNoFolder,      // %s reason, %s log folder
        GraphicsTitle,      // %s product
        DeviceLost,         // %s DXGI reason, %s log folder
        RestartAfterLoss,   // appended to DeviceLost when the game offers to restart
        MinimumSpec,        // %s this computer's GPU
        StartFailed,        // %s what failed, %s log folder
        Count
    };

    // The text in the player's language, UTF-8; a printf format where the enumerator says so.
    const char* Text(PlayerText id);
    // The language Text answers in: settings.json's ui.language when the player chose one, else
    // the Windows display language ("ja", any Chinese as "zh-Hant"), else "en".
    std::string PlayerLanguage();

    enum class PlayerError
    {
        None,
        DeviceLost,    // detail: the DXGI reason
        MinimumSpec    // detail: the GPU
    };
    // The first note wins: a loss noticed twice keeps the reason first decoded. Thread-safe.
    void        NotePlayerError(PlayerError kind, const std::string& detail);
    PlayerError LastPlayerError(std::string* detail = nullptr);
}
