#pragma once

// Registers the per-category Notify property panels; called once from EditorLayer
// setup. Idempotent: registering a category again overwrites its drawer.

void RegisterDefaultNotifyEditors();
