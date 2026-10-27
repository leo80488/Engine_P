// MediaModule: FFmpeg decoding on the CPU as an engine module (src/Modules/Media.module.json) - the
// reader the face tracker's video files use (Media/VideoReader.h). It has no hook: its users call
// the reader directly, and the module only puts FFmpeg into the exe.

#include "Module/EngineModule.h"

class MediaModule final : public Module::IEngineModule
{
};

ENGINE_MODULE(MediaModule)
