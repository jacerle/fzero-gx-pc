cmake_minimum_required(VERSION 3.25)
project(fzero-gx-pc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(UNIX AND NOT APPLE AND EXISTS "/home/linuxbrew/.linuxbrew")
    list(PREPEND CMAKE_PREFIX_PATH "/home/linuxbrew/.linuxbrew")
    list(PREPEND CMAKE_PROGRAM_PATH "/home/linuxbrew/.linuxbrew/bin")
    if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH} "/home/linuxbrew/.linuxbrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "/home/linuxbrew/.linuxbrew/lib/pkgconfig")
    endif()
endif()

include(FetchContent)
include(CTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig QUIET)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

# SDL3: prefer installed package (e.g. Homebrew on Bazzite), fallback to FetchContent (CI/Windows)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found locally; fetching SDL3 via FetchContent...")
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

# Vulkan-Headers (header-only; avoids requiring system libvulkan-dev)
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.369
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(VulkanHeaders)

# volk (header-only / static loader; loads libvulkan.so.1 / vulkan-1.dll dynamically)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_HEADERS_ONLY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(volk)

# Zstandard (prefer installed package, fallback to FetchContent)
find_package(zstd CONFIG QUIET)
add_library(fzgx_zstd INTERFACE)
if(TARGET zstd::libzstd)
    target_link_libraries(fzgx_zstd INTERFACE zstd::libzstd)
elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(fzgx_zstd INTERFACE zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    target_link_libraries(fzgx_zstd INTERFACE zstd::libzstd_static)
else()
    message(STATUS "zstd not found locally; fetching facebook/zstd via FetchContent...")
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
    target_link_libraries(fzgx_zstd INTERFACE libzstd_static)
endif()

# Optional FFmpeg (libavformat/libavcodec/libavutil/libswscale/libswresample) for Sofdec .sfd FMV playback
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
endif()

# nlohmann_json
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_lib PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
)
target_link_libraries(imgui_lib PUBLIC
    SDL3::SDL3
    Vulkan::Headers
    volk_headers
)

# Compile GLSL shaders -> SPIR-V -> embedded C++ header
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
set(EMBEDDED_SHADERS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.hpp")

set(SHADER_SOURCES
    "test_scene_vert:${CMAKE_CURRENT_SOURCE_DIR}/shaders/test_scene.vert"
    "test_scene_frag:${CMAKE_CURRENT_SOURCE_DIR}/shaders/test_scene.frag"
    "present_crt_vert:${CMAKE_CURRENT_SOURCE_DIR}/shaders/present_crt.vert"
    "present_crt_frag:${CMAKE_CURRENT_SOURCE_DIR}/shaders/present_crt.frag"
    "gx_3d_vert:${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_3d.vert"
    "gx_3d_frag:${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_3d.frag"
    "fsr_easu_frag:${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr_easu.frag"
    "fsr_rcas_frag:${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr_rcas.frag"
)

set(SPV_FILES "")
set(EMBED_ARGS "")
foreach(entry IN LISTS SHADER_SOURCES)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 var_name)
    list(GET parts 1 src_file)
    set(spv_file "${SHADER_OUTPUT_DIR}/${var_name}.spv")
    add_custom_command(
        OUTPUT "${spv_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V "${src_file}" -o "${spv_file}"
        DEPENDS "${src_file}"
        COMMENT "Compiling shader ${var_name}"
        VERBATIM
    )
    list(APPEND SPV_FILES "${spv_file}")
    list(APPEND EMBED_ARGS "${var_name}" "${spv_file}")
endforeach()

add_custom_command(
    OUTPUT "${EMBEDDED_SHADERS_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spv.py"
            "${EMBEDDED_SHADERS_HEADER}" ${EMBED_ARGS}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spv.py" ${SPV_FILES}
    COMMENT "Embedding SPIR-V shaders into C++ header"
    VERBATIM
)

add_custom_target(fzgx_shaders DEPENDS "${EMBEDDED_SHADERS_HEADER}")

# Warning flags helper
function(fzgx_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
endfunction()

# Core portable library (config, disc, sha1, rvz, dol/rel, assets, audio, gci, input math, SDK bridge)
add_library(fzgx_core STATIC
    src/core/paths.cpp
    src/core/config.cpp
    src/disc/sha1.cpp
    src/disc/disc_image.cpp
    src/disc/rvz_backend.cpp
    src/assets/avlz.cpp
    src/assets/u8_arc.cpp
    src/assets/gx_texture.cpp
    src/assets/gma_model.cpp
    src/game/dol_rel_loader.cpp
    src/audio/adx_decoder.cpp
    src/audio/audio_engine.cpp
    src/save/gci_card.cpp
    src/input/input_manager.cpp
    src/platform/platform_bridge.cpp
    src/video/gx_state.cpp
)
target_include_directories(fzgx_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(fzgx_core PUBLIC
    nlohmann_json::nlohmann_json
    SDL3::SDL3
    fzgx_zstd
)
fzgx_apply_warnings(fzgx_core)

# Video & UI library (Vulkan 1.3 + volk + GX-to-Vulkan + librashader + Sofdec FMV + Dear ImGui overlay)
add_library(fzgx_video_ui STATIC
    src/video/volk_impl.c
    src/video/gx_vulkan.cpp
    src/video/librashader_bridge.cpp
    src/video/sofdec_player.cpp
    src/video/vulkan_renderer.cpp
    src/ui/overlay.cpp
)
add_dependencies(fzgx_video_ui fzgx_shaders)
target_include_directories(fzgx_video_ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_include_directories(fzgx_video_ui SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librashader"
)
target_link_libraries(fzgx_video_ui PUBLIC
    fzgx_core
    imgui_lib
    Vulkan::Headers
    volk_headers
    SDL3::SDL3
)
if(FFMPEG_FOUND)
    target_compile_definitions(fzgx_video_ui PUBLIC FZGX_HAS_FFMPEG=1)
    target_link_libraries(fzgx_video_ui PUBLIC PkgConfig::FFMPEG)
else()
    target_compile_definitions(fzgx_video_ui PUBLIC FZGX_HAS_FFMPEG=0)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(fzgx_video_ui PUBLIC dl)
endif()
fzgx_apply_warnings(fzgx_video_ui)

# --- Decompiled game TUs (external/fzero-gx-decomp) + native Dolphin SDK replacement ---
#
# Decomp TUs compile from the submodule as-is. Where a TU still has a 64-bit/LE portability
# blocker, patches/decomp/<path>.patch is applied to a copy under build/decomp_overlay/;
# the overlay shrinks as upstream lands R4/R5 (docs/RECOMPILATION_SCOPE.md).
find_package(Git REQUIRED)
set(FZGX_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/fzero-gx-decomp")
set(FZGX_DECOMP_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches/decomp")
set(FZGX_DECOMP_OVERLAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/decomp_overlay")

function(fzgx_decomp_sources out_var)
    set(result "")
    foreach(rel IN LISTS ARGN)
        set(src "${FZGX_DECOMP_DIR}/${rel}")
        set(patch "${FZGX_DECOMP_PATCH_DIR}/${rel}.patch")
        if(EXISTS "${patch}")
            set(dst "${FZGX_DECOMP_OVERLAY_DIR}/${rel}")
            get_filename_component(dst_dir "${dst}" DIRECTORY)
            get_filename_component(src_dir "${src}" DIRECTORY)
            # GIT_CEILING_DIRECTORIES stops git from discovering this repo, so `git apply`
            # patches the overlay copy relative to the overlay root like GNU patch.
            add_custom_command(
                OUTPUT "${dst}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy "${src}" "${dst}"
                COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
                        "${GIT_EXECUTABLE}" apply -p1 --whitespace=nowarn "${patch}"
                WORKING_DIRECTORY "${FZGX_DECOMP_OVERLAY_DIR}"
                DEPENDS "${src}" "${patch}"
                COMMENT "Applying downstream overlay patch to ${rel}"
                VERBATIM
            )
            # Keep the original directory first for quoted includes relative to the TU.
            set_source_files_properties("${dst}" PROPERTIES INCLUDE_DIRECTORIES "${src_dir}")
            list(APPEND result "${dst}")
        else()
            list(APPEND result "${src}")
        endif()
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Shared compile environment for decomp TUs and the native SDK: the port's replacements for the
# decomp's hardware/asm headers (searched first), decomp headers, and mwcc_shim.h force-included
# so s32/u32 stay 32-bit on LP64/LLP64 hosts.
set(FZGX_DECOMP_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/fzgx_port/decomp_overrides"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${FZGX_DECOMP_DIR}/include"
)
set(FZGX_DECOMP_SHIM "${CMAKE_CURRENT_SOURCE_DIR}/include/fzgx_port/mwcc_shim.h")
# Decomp code is not ours to restyle; only 64-bit/ABI hazards are fatal.
set(FZGX_DECOMP_GNU_ERROR_FLAGS
    -Werror=pointer-to-int-cast
    -Werror=int-to-pointer-cast
    -Werror=implicit-function-declaration
    -Werror=incompatible-pointer-types
    -Werror=int-conversion
)
add_library(fzgx_decomp_env INTERFACE)
target_include_directories(fzgx_decomp_env INTERFACE ${FZGX_DECOMP_INCLUDE_DIRS})
if(MSVC)
    target_compile_options(fzgx_decomp_env INTERFACE
        "$<$<COMPILE_LANGUAGE:C>:/FI${FZGX_DECOMP_SHIM}>")
else()
    target_compile_options(fzgx_decomp_env INTERFACE
        "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${FZGX_DECOMP_SHIM}>"
        "$<$<COMPILE_LANGUAGE:C>:${FZGX_DECOMP_GNU_ERROR_FLAGS}>"
    )

    # Compile-readiness burn-down of main_rel: compiles every TU with the decomp environment
    # and rewrites docs/MAIN_REL_STATUS.md. Not part of the default build.
    list(TRANSFORM FZGX_DECOMP_GNU_ERROR_FLAGS PREPEND "--flag=" OUTPUT_VARIABLE FZGX_STATUS_FLAG_ARGS)
    add_custom_target(main_rel_status
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/decomp_status.py"
                --cc "${CMAKE_C_COMPILER}"
                --decomp "${FZGX_DECOMP_DIR}"
                --module main_rel
                --shim "${FZGX_DECOMP_SHIM}"
                --include-dirs ${FZGX_DECOMP_INCLUDE_DIRS}
                ${FZGX_STATUS_FLAG_ARGS}
                --out "${CMAKE_CURRENT_SOURCE_DIR}/docs/MAIN_REL_STATUS.md"
        COMMENT "Compiling main_rel TUs for docs/MAIN_REL_STATUS.md"
        VERBATIM
    )
endif()

# Native Dolphin SDK replacement (OS / DVD / VI / PAD / CARD) over platform_api.h.
add_library(fzgx_sdk STATIC
    src/sdk/sdk_runtime.c
    src/sdk/os.c
    src/sdk/dvd.c
    src/sdk/vi.c
    src/sdk/pad.c
    src/sdk/card.c
    src/sdk/ppc_state.c
)
target_link_libraries(fzgx_sdk PUBLIC fzgx_decomp_env fzgx_core)
fzgx_apply_warnings(fzgx_sdk)

# main.dol TUs that compile natively against the SDK replacement.
fzgx_decomp_sources(FZGX_DECOMP_DOL_SOURCES
    src/dol/arc.c
    src/dol/ClampStick.c
    src/dol/fn_8007ED90.c
)
add_library(fzgx_decomp STATIC
    ${FZGX_DECOMP_DOL_SOURCES}
    src/decomp_port/dol_data.c
    src/decomp_port/arc_host_order.c
)
target_link_libraries(fzgx_decomp PUBLIC fzgx_decomp_env fzgx_sdk)

# main_rel TUs that compile natively (docs/MAIN_REL_STATUS.md, target main_rel_status). Built
# but not linked yet: the module links once all 85 TUs, its data and the DOL code it calls do.
# GCC/Clang only until decomp data (__declspec(section ...)) has an MSVC spelling.
if(NOT MSVC)
    fzgx_decomp_sources(FZGX_MAIN_REL_SOURCES
        src/rel/main_rel/alloc_b.c
        src/rel/main_rel/bg_tow.c
        src/rel/main_rel/car_test_b.c
        src/rel/main_rel/light.c
        src/rel/main_rel/objput.c
        src/rel/main_rel/ranking_c.c
    )
    add_library(fzgx_main_rel STATIC ${FZGX_MAIN_REL_SOURCES})
    target_link_libraries(fzgx_main_rel PUBLIC fzgx_decomp)
endif()

# Native boot path (main.dol game_main.c equivalent): SDK init in retail order, then main_rel.
add_library(fzgx_boot STATIC
    src/boot/boot.c
)
target_link_libraries(fzgx_boot PUBLIC fzgx_decomp)
fzgx_apply_warnings(fzgx_boot)

# Main executable
add_executable(fzero-gx-pc
    src/main.cpp
)
target_link_libraries(fzero-gx-pc PRIVATE
    fzgx_video_ui
    fzgx_core
    fzgx_boot
)
fzgx_apply_warnings(fzero-gx-pc)

if(WIN32)
    add_custom_command(TARGET fzero-gx-pc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:fzero-gx-pc>
    )
endif()

# Unit tests
if(BUILD_TESTING)
    add_executable(fzgx_tests
        tests/test_main.cpp
        tests/test_sdk.c
        tests/test_sdk_card.c
        tests/test_decomp_layer.c
    )
    target_link_libraries(fzgx_tests PRIVATE fzgx_core fzgx_boot)
    fzgx_apply_warnings(fzgx_tests)
    add_test(NAME fzgx_unit_tests COMMAND fzgx_tests)
endif()
