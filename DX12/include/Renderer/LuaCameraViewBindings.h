#pragma once
// Engine.GetCameraPosition / Engine.GetCameraForward - the active camera's
// world pose as the renderer's FrameView has it.

namespace sol { class state; }
class Renderer;

// Appends to the `Engine` table. @p renderer outlives every call (App::Run's).
void RegisterLuaCameraViewBindings(sol::state& state, Renderer& renderer);
