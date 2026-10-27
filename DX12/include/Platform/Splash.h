#pragma once

// Platform/Splash.h - the window a player sees while the game boots: the product's name, a loading
// line in their language and a bar that moves with the boot (Sys::Boot marks). Its own thread and
// window, so it paints while the main thread is busy creating the device and loading content; it
// closes itself at the first frame, or when Close is called (an error dialog must not sit under it).

#include <string>

namespace Plat::Splash
{
    // Opens the splash, unless one is open. @p title, @p line: UTF-8.
    void Show(const std::string& title, const std::string& line);
    // Closes it and waits for its thread. Safe to call more than once, or with none open.
    void Close();
}
