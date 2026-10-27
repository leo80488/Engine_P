#pragma once

// Project precompiled header: stable system / third-party headers only. A
// project header here invalidates the PCH and rebuilds everything on every edit.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// nlohmann/json: 62 files include it and most of the rest reach it through a header, so each translation unit
// parsed its 25k lines; here it is parsed once per project.
#include <nlohmann/json.hpp>

// Tracy is safe to include unconditionally: without TRACY_ENABLE (Debug /
// Release) every macro is a no-op. Engine.Cpp.props adds the include dir; living here
// makes ZoneScoped / FrameMark available in every TU.
#include <tracy/Tracy.hpp>
