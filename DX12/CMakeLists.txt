set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(ENGINE_EXTERNAL "${ENGINE_ROOT}/external")
set(ENGINE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ENGINE_BIN "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ENGINE_BIN}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
if(ENGINE_ENABLE_FACE_TRACKING)
    set(ENGINE_ENABLE_MEDIA ON)
endif()
set(configure_args --out "${ENGINE_GENERATED}")
foreach(pair IN ITEMS "NAV;Nav" "MEDIA;Media" "VIDEO;Video" "FACE_TRACKING;FaceTracking" "STREAMLINE;Streamline")
    list(GET pair 0 option_name)
    list(GET pair 1 module_name)
    if(ENGINE_ENABLE_${option_name})
        list(APPEND configure_args --module "${module_name}")
    endif()
endforeach()
if(NOT ENGINE_ENABLE_HARNESS)
    list(APPEND configure_args --no-harness)
endif()
if(NOT ENGINE_ENABLE_CONTROL_CHANNEL)
    list(APPEND configure_args --no-control)
endif()
if(ENGINE_ENABLE_TRACY)
    list(APPEND configure_args --tracy)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${ENGINE_ROOT}/tools/cmake_configure.py" ${configure_args}
    COMMAND_ERROR_IS_FATAL ANY)
include("${ENGINE_GENERATED}/Sources.cmake")
file(GLOB_RECURSE source_membership CONFIGURE_DEPENDS "${ENGINE_ROOT}/src/*.cpp")
file(GLOB module_descriptors CONFIGURE_DEPENDS "${ENGINE_ROOT}/src/Modules/*.module.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ENGINE_ROOT}/tools/cmake_sources.json" "${ENGINE_ROOT}/tools/cmake_configure.py" "${ENGINE_ROOT}/tools/cmake_modules.py" "${ENGINE_ROOT}/tools/cmake_source_rules.py" ${module_descriptors})

add_library(EngineOptions INTERFACE)
target_include_directories(EngineOptions INTERFACE
    "${ENGINE_ROOT}/include" "${ENGINE_EXTERNAL}" "${ENGINE_EXTERNAL}/imgui"
    "${ENGINE_EXTERNAL}/DirectX12" "${ENGINE_EXTERNAL}/DirectXTex"
    "${ENGINE_EXTERNAL}/dxc/include" "${ENGINE_NRD_SDK_DIR}/Include"
    "${ENGINE_EXTERNAL}/lua" "${ENGINE_EXTERNAL}/sol2/include"
    "${ENGINE_EXTERNAL}/FreeType/include" "${ENGINE_EXTERNAL}/tracy/public"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/api/include"
    "${ENGINE_FFX_SDK_DIR}/FidelityFX/upscalers/include")
target_compile_features(EngineOptions INTERFACE cxx_std_20)
target_compile_options(EngineOptions INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/bigobj;/MP;/permissive-;/Zc:__cplusplus>" /FS)
target_compile_definitions(EngineOptions INTERFACE UNICODE _UNICODE NOMINMAX
    JPH_DEBUG_RENDERER JPH_PROFILE_ENABLED JPH_OBJECT_STREAM JPH_FLOATING_POINT_EXCEPTIONS_ENABLED
    SOL_ALL_SAFETIES_ON=1 WITH_RUNTIME_SHADER_COMPILE WITH_PROFILER
    ENGINE_WITH_RTXDI=0 ENGINE_WITH_ONNXRUNTIME=$<BOOL:${ENGINE_ENABLE_FACE_TRACKING}>
    ENGINE_WITH_STREAMLINE=$<BOOL:${ENGINE_ENABLE_STREAMLINE}>)
if(ENGINE_ENABLE_HARNESS)
    target_compile_definitions(EngineOptions INTERFACE WITH_HARNESS)
endif()
if(ENGINE_ENABLE_HARNESS AND ENGINE_ENABLE_CONTROL_CHANNEL)
    target_compile_definitions(EngineOptions INTERFACE WITH_CONTROL)
endif()
if(ENGINE_ENABLE_TRACY)
    target_compile_definitions(EngineOptions INTERFACE TRACY_ENABLE TRACY_ON_DEMAND)
endif()

include(cmake/Dependencies.cmake)
set(runtime_args --root "${ENGINE_ROOT}" --out "${ENGINE_BIN}")
foreach(dll IN LISTS ENGINE_CORE_DLLS)
    list(APPEND runtime_args --dll "${dll}")
endforeach()
add_custom_target(EngineRuntime
    COMMAND "${Python3_EXECUTABLE}" "${ENGINE_ROOT}/tools/cmake_stage.py" ${runtime_args}
    VERBATIM)
add_library(EngineCore STATIC ${ENGINE_CORE_SOURCES})
target_link_libraries(EngineCore PUBLIC EngineOptions EngineDependencies EngineJolt)
target_compile_definitions(EngineCore PRIVATE _LIB)
target_precompile_headers(EngineCore PRIVATE "${ENGINE_ROOT}/include/pch.h")
foreach(source IN LISTS ENGINE_CORE_SOURCES ENGINE_EDITOR_SOURCES)
    string(FIND "${source}" "${ENGINE_EXTERNAL}/" external_pos)
    if(external_pos EQUAL 0)
        set_source_files_properties("${source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()
set_source_files_properties("${ENGINE_ROOT}/src/System/BuildInfo.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties("${ENGINE_ROOT}/external/lua/onelua.c" PROPERTIES LANGUAGE C)
set(build_info_dir "${ENGINE_GENERATED}/$<CONFIG>")
add_custom_target(EngineBuildInfo
    COMMAND "${Python3_EXECUTABLE}" "${ENGINE_ROOT}/tools/gen_build_info.py"
        --out "${build_info_dir}" --config "$<CONFIG>" --harness "$<BOOL:${ENGINE_ENABLE_HARNESS}>"
        --module-set "${ENGINE_GENERATED}/modules" --msvc "${MSVC_VERSION}" --sdk "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    VERBATIM)
add_custom_target(EngineMaterialLayout
    COMMAND "${Python3_EXECUTABLE}" "${ENGINE_ROOT}/tools/gen_material_layout.py"
    VERBATIM)
add_dependencies(EngineCore EngineBuildInfo EngineMaterialLayout)
target_include_directories(EngineCore BEFORE PRIVATE "${build_info_dir}")

foreach(module IN LISTS ENGINE_CPP_MODULES)
    add_library(EngineModule_${module} STATIC ${ENGINE_MODULE_${module}_SOURCES})
    target_link_libraries(EngineModule_${module} PRIVATE EngineOptions)
    target_compile_definitions(EngineModule_${module} PRIVATE _LIB)
    target_precompile_headers(EngineModule_${module} PRIVATE "${ENGINE_ROOT}/include/pch.h")
    if(module STREQUAL "Nav")
        target_include_directories(EngineModule_${module} PRIVATE
            "${ENGINE_EXTERNAL}/recastnavigation/Recast/Include" "${ENGINE_EXTERNAL}/recastnavigation/Detour/Include")
    elseif(module STREQUAL "Media" OR module STREQUAL "Video")
        target_compile_definitions(EngineModule_${module} PRIVATE WITH_FFMPEG)
        target_include_directories(EngineModule_${module} PRIVATE "${ENGINE_EXTERNAL}/ffmpeg/include")
    elseif(module STREQUAL "FaceTracking")
        target_include_directories(EngineModule_${module} PRIVATE "${ENGINE_EXTERNAL}/onnxruntime/include")
    elseif(module STREQUAL "Streamline")
        target_include_directories(EngineModule_${module} PRIVATE "${ENGINE_EXTERNAL}/Streamline/include")
    endif()
endforeach()
if(ENGINE_ENABLE_FACE_TRACKING)
    set_source_files_properties("${ENGINE_ROOT}/src/FaceTracking/Sources/UdpReceiver.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

if(ENGINE_BUILD_EDITOR)
    add_library(EditorCore STATIC ${ENGINE_EDITOR_SOURCES})
    target_link_libraries(EditorCore PUBLIC EngineOptions)
    target_compile_definitions(EditorCore PRIVATE _LIB WITH_EDITOR IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    target_precompile_headers(EditorCore PRIVATE "${ENGINE_ROOT}/include/pch.h")
endif()
function(engine_add_app name)
    add_executable(${name} WIN32 src/main.cpp src/App.cpp src/App_Render.cpp
        src/Platform/App.rc "${ENGINE_GENERATED}/modules/Modules.generated.cpp")
    target_link_libraries(${name} PRIVATE EngineCore)
    foreach(module IN LISTS ENGINE_CPP_MODULES)
        target_link_libraries(${name} PRIVATE EngineModule_${module})
    endforeach()
    target_compile_definitions(${name} PRIVATE _CONSOLE)
    target_include_directories(${name} BEFORE PRIVATE "${build_info_dir}" "${ENGINE_ROOT}/src/Platform")
    target_precompile_headers(${name} PRIVATE "${ENGINE_ROOT}/include/pch.h")
    target_link_options(${name} PRIVATE "/MANIFESTINPUT:${ENGINE_ROOT}/src/Platform/App.manifest" /DEBUG)
    add_dependencies(${name} EngineBuildInfo)
    set_target_properties(${name} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_ROOT}")
    add_dependencies(${name} EngineRuntime)
    if(name STREQUAL "Editor")
        target_link_libraries(${name} PRIVATE EditorCore)
        target_compile_definitions(${name} PRIVATE WITH_EDITOR IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ENGINE_EXTERNAL}/assimp/bin/$<IF:$<CONFIG:Debug>,Debug,Release>/assimp-vc143-mt$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:${name}>"
            VERBATIM)
    endif()
    install(TARGETS ${name} RUNTIME DESTINATION .)
    if(name STREQUAL "Editor")
        install(FILES "${ENGINE_EXTERNAL}/assimp/bin/$<IF:$<CONFIG:Debug>,Debug,Release>/assimp-vc143-mt$<$<CONFIG:Debug>:d>.dll" DESTINATION .)
    endif()
    if(BUILD_TESTING AND ENGINE_ENABLE_HARNESS)
        add_test(NAME ${name}.startup COMMAND "${Python3_EXECUTABLE}" "${ENGINE_ROOT}/tools/cmake_smoke.py"
            --exe "$<TARGET_FILE:${name}>" --root "${ENGINE_ROOT}" --artifacts "${ENGINE_ROOT}/tests/_artifacts/cmake/$<CONFIG>/${name}")
        set_tests_properties(${name}.startup PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
    endif()
endfunction()
if(ENGINE_BUILD_EDITOR)
    engine_add_app(Editor)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Editor)
endif()
if(ENGINE_BUILD_GAME)
    engine_add_app(Game)
endif()
install(FILES ${ENGINE_CORE_DLLS} DESTINATION .)
install(DIRECTORY "${ENGINE_ROOT}/engine" "${ENGINE_ROOT}/asset" "${ENGINE_ROOT}/shaders"
    DESTINATION . PATTERN shader_cache_dxil EXCLUDE PATTERN "*.bak" EXCLUDE)
install(FILES game.json engine.json settings.default.json input_profiles.json DESTINATION .)
install(FILES settings.default.json DESTINATION . RENAME settings.json)
install(FILES "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" DESTINATION .)
file(GLOB_RECURSE license_files "${ENGINE_EXTERNAL}/*LICENSE*" "${ENGINE_EXTERNAL}/*License*"
    "${ENGINE_EXTERNAL}/*COPYING*" "${ENGINE_EXTERNAL}/*FTL*" "${ENGINE_EXTERNAL}/FidelityFX/docs/license.md")
foreach(license IN LISTS license_files)
    file(RELATIVE_PATH relative_license "${ENGINE_EXTERNAL}" "${license}")
    get_filename_component(license_dir "${relative_license}" DIRECTORY)
    install(FILES "${license}" DESTINATION "third_party/${license_dir}")
endforeach()
