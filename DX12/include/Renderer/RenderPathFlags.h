#pragma once
// -pathtrace / -gipath=<raster|restir_di|restir_gi|restir|pathtrace|pathtrace_realtime>:
// the render path from the command line. An unknown -gipath token refuses the run
// (PostQuitMessage(PreconditionFail)). The ReSTIR backend flags (-di_backend / -gi_backend /
// -pt_backend, -di_backend_initial / -di_backend_reuse, -restir_compare) are validated here
// too, with the same refusal.

class Renderer;

// Call before Lua captures the settings snapshot (App::Run does, right after the
// settings are applied).
void ApplyRenderPathFlags(Renderer& renderer);

// Call AFTER engine.json's developer settings are applied: the command line wins over
// engine.json for this run (App::BootRenderer does, right after m_devSettings.ApplyTo).
void ApplyRestirBackendFlags(Renderer& renderer);
