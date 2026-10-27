function(engine_require path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required dependency missing: ${path}. See docs/BUILD.md for the SDK layout and restore instructions.")
    endif()
endfunction()
foreach(path IN ITEMS
    "${ENGINE_EXTERNAL}/dxc/bin/dxcompiler.dll" "${ENGINE_EXTERNAL}/dxc/bin/dxil.dll"
    "${ENGINE_NRD_SDK_DIR}/Include/NRD.h" "${ENGINE_NRD_SDK_DIR}/_Bin/Release/NRD.lib"
    "${ENGINE_NRD_SDK_DIR}/_Build/_deps/shadermake-build/Release/ShaderMakeBlob.lib"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/api/include/ffx_api.h"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    "${ENGINE_ROOT}/freetype.dll")
    engine_require("${path}")
endforeach()
set(flavor "$<IF:$<CONFIG:Debug>,Debug,Release>")
add_library(EngineDependencies INTERFACE)
target_link_directories(EngineDependencies INTERFACE
    "${ENGINE_EXTERNAL}/DirectXTex/lib" "${ENGINE_EXTERNAL}/dxc/lib" "${ENGINE_EXTERNAL}/FreeType/lib"
    "${ENGINE_EXTERNAL}/meshoptimizer/${flavor}" "${ENGINE_NRD_SDK_DIR}/_Bin/${flavor}"
    "${ENGINE_NRD_SDK_DIR}/_Build/_deps/shadermake-build/${flavor}"
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/${flavor}" "${ENGINE_EXTERNAL}/recastnavigation/Detour/${flavor}"
    "${ENGINE_EXTERNAL}/assimp/lib/${flavor}" "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin")
target_link_libraries(EngineDependencies INTERFACE dxcompiler freetype NRD ShaderMakeBlob amd_fidelityfx_loader_dx12)
set(ENGINE_CORE_DLLS "${ENGINE_EXTERNAL}/dxc/bin/dxcompiler.dll" "${ENGINE_EXTERNAL}/dxc/bin/dxil.dll"
    "${ENGINE_ROOT}/freetype.dll"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll")
if(ENGINE_ENABLE_MEDIA OR ENGINE_ENABLE_VIDEO)
    target_link_directories(EngineDependencies INTERFACE "${ENGINE_EXTERNAL}/ffmpeg/lib")
    foreach(dll IN ITEMS avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll swresample-6.dll)
        engine_require("${ENGINE_ROOT}/${dll}")
        list(APPEND ENGINE_CORE_DLLS "${ENGINE_ROOT}/${dll}")
    endforeach()
endif()
if(ENGINE_ENABLE_FACE_TRACKING)
    engine_require("${ENGINE_EXTERNAL}/onnxruntime/include/onnxruntime_cxx_api.h")
    foreach(dll IN ITEMS onnxruntime.dll onnxruntime_providers_shared.dll)
        engine_require("${ENGINE_EXTERNAL}/onnxruntime/bin/${dll}")
        list(APPEND ENGINE_CORE_DLLS "${ENGINE_EXTERNAL}/onnxruntime/bin/${dll}")
    endforeach()
endif()
if(ENGINE_ENABLE_STREAMLINE)
    engine_require("${ENGINE_EXTERNAL}/Streamline/include/sl.h")
    engine_require("${ENGINE_EXTERNAL}/Streamline/lib/x64/sl.interposer.lib")
    target_link_directories(EngineDependencies INTERFACE "${ENGINE_EXTERNAL}/Streamline/lib/x64")
    file(GLOB sl_dlls "${ENGINE_EXTERNAL}/Streamline/bin/x64/*.dll" "${ENGINE_EXTERNAL}/Streamline/bin/x64/Release/*.dll")
    list(APPEND ENGINE_CORE_DLLS ${sl_dlls})
endif()
file(GLOB_RECURSE jolt_sources CONFIGURE_DEPENDS "${ENGINE_EXTERNAL}/Jolt/*.cpp")
add_library(EngineJolt STATIC ${jolt_sources})
target_include_directories(EngineJolt PUBLIC "${ENGINE_EXTERNAL}")
target_compile_definitions(EngineJolt PUBLIC JPH_DEBUG_RENDERER JPH_PROFILE_ENABLED JPH_OBJECT_STREAM JPH_FLOATING_POINT_EXCEPTIONS_ENABLED)
target_compile_options(EngineJolt PRIVATE /utf-8 /bigobj /MP /GR- "$<$<NOT:$<CONFIG:Debug>>:/arch:AVX2;/fp:fast;/Ob2;/GS->")
set_target_properties(EngineJolt PROPERTIES CXX_STANDARD 17 FOLDER Dependencies)
