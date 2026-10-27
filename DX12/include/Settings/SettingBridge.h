#pragma once

// A module's live half of a settings row. The row stays in the core table, so settings.json
// keeps its value while the module is off or absent; its apply / capture reach the module
// through the bridge registered under the row's path, and do nothing without one.

#include <string_view>

namespace Settings
{
    struct BoolBridge
    {
        void (*apply)(bool value) = nullptr;
        bool (*capture)()         = nullptr;
    };

    // Main thread, before the boot ApplyTo (Module::IEngineModule::Initialize). @p path must
    // outlive the process (a literal).
    void RegisterBoolBridge(std::string_view path, BoolBridge bridge);
    // Null when no enabled module bridges @p path.
    const BoolBridge* FindBoolBridge(std::string_view path);
}
