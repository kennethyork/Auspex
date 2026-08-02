cmake_minimum_required(VERSION 3.28)
project(auspex
    VERSION 0.2.0
    DESCRIPTION "Auspex Shell — native C++ port for Linux Mint / XFCE"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Our warning set, applied per-target. NOT add_compile_options() — that would
# also apply to the vendored whisper.cpp/ggml tree and bury our diagnostics.
set(AUSPEX_WARNINGS -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Target CPU capability level
# ---------------------------------------------------------------------------
# ggml defaults GGML_NATIVE=ON, which emits -march=native. That binary requires
# the ISA of whatever machine compiled it and dies with SIGILL elsewhere, so it
# must never be used for a build that will be copied to another machine.
#
# The levels below are the standard x86-64 microarchitecture levels (as used by
# glibc's hwcaps and GCC's -march=x86-64-vN):
#
#   v1  SSE2                                    every x86_64 CPU ever made
#   v2  + SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT       Nehalem / Bulldozer, ~2009+
#   v3  + AVX/AVX2/BMI1/BMI2/FMA/F16C           Haswell / Excavator, ~2013+
#   v4  + AVX-512F/BW/CD/DQ/VL                  Skylake-X / Zen 4, ~2017+
#
# "native" stays the default because setup.sh builds on the machine that will run
# it, where native is both correct and fastest. bin/release.sh overrides this to
# produce one portable artifact per level.
set(AUSPEX_ARCH_LEVEL "native" CACHE STRING "Target CPU: native|v1|v2|v3|v4")
set_property(CACHE AUSPEX_ARCH_LEVEL PROPERTY STRINGS native v1 v2 v3 v4)

if(AUSPEX_ARCH_LEVEL STREQUAL "native")
    message(STATUS "CPU target: native (-march=native; NOT portable to other machines)")
    set(GGML_NATIVE ON CACHE BOOL "" FORCE)
else()
    # Hand ggml an explicit ISA set; GGML_NATIVE must be off or it overrides these.
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)

    set(_lvl_sse42 OFF) 
    set(_lvl_avx   OFF)
    set(_lvl_avx2  OFF)
    set(_lvl_fma   OFF)
    set(_lvl_f16c  OFF)
    set(_lvl_avx512 OFF)

    if(AUSPEX_ARCH_LEVEL STREQUAL "v2")
        set(_lvl_sse42 ON)
    elseif(AUSPEX_ARCH_LEVEL STREQUAL "v3")
        set(_lvl_sse42 ON) 
        set(_lvl_avx ON) 
        set(_lvl_avx2 ON) 
        set(_lvl_fma ON) 
        set(_lvl_f16c ON)
    elseif(AUSPEX_ARCH_LEVEL STREQUAL "v4")
        set(_lvl_sse42 ON) 
        set(_lvl_avx ON) 
        set(_lvl_avx2 ON) 
        set(_lvl_fma ON) 
        set(_lvl_f16c ON)
        set(_lvl_avx512 ON)
    elseif(NOT AUSPEX_ARCH_LEVEL STREQUAL "v1")
        message(FATAL_ERROR "AUSPEX_ARCH_LEVEL must be native|v1|v2|v3|v4")
    endif()

    set(GGML_SSE42  ${_lvl_sse42}  CACHE BOOL "" FORCE)
    set(GGML_AVX    ${_lvl_avx}    CACHE BOOL "" FORCE)
    set(GGML_AVX2   ${_lvl_avx2}   CACHE BOOL "" FORCE)
    set(GGML_FMA    ${_lvl_fma}    CACHE BOOL "" FORCE)
    set(GGML_F16C   ${_lvl_f16c}   CACHE BOOL "" FORCE)
    set(GGML_AVX512 ${_lvl_avx512} CACHE BOOL "" FORCE)

    # Our own translation units get the matching -march so they cannot emit
    # instructions the target level lacks either.
    if(AUSPEX_ARCH_LEVEL STREQUAL "v1")
        list(APPEND AUSPEX_WARNINGS_ARCH -march=x86-64)
    else()
        list(APPEND AUSPEX_WARNINGS_ARCH -march=x86-64-${AUSPEX_ARCH_LEVEL})
    endif()
    add_compile_options(${AUSPEX_WARNINGS_ARCH})
    message(STATUS "CPU target: x86-64-${AUSPEX_ARCH_LEVEL} (portable)")
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Prefer the distro package (nlohmann-json3-dev), fall back to the vendored
# single header so a plain `cmake && make` works without root.
find_package(nlohmann_json 3.11 QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json: system package")
    set(AUSPEX_JSON_TARGET nlohmann_json::nlohmann_json)
else()
    message(STATUS "nlohmann_json: vendored header (third_party/)")
    add_library(auspex-json INTERFACE)
    target_include_directories(auspex-json INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    set(AUSPEX_JSON_TARGET auspex-json)
endif()

# ---------------------------------------------------------------------------
# whisper.cpp — ASR backend, replacing torch + transformers
# ---------------------------------------------------------------------------
# Backend selection. Vulkan needs glslc -- the SPIR-V compiler from shaderc, which
# whisper.cpp execs by name; glslang-tools is a different tool and does NOT
# provide it. CUDA needs nvcc. Neither is present on a stock desktop, so probe
# rather than assume. Vulkan is preferred over CUDA here only because it needs no
# ~3GB toolkit install to reach the same GPU.
set(AUSPEX_WHISPER_BACKEND "auto" CACHE STRING "ASR backend: auto|cpu|vulkan|cuda")
set_property(CACHE AUSPEX_WHISPER_BACKEND PROPERTY STRINGS auto cpu vulkan cuda)

if(AUSPEX_WHISPER_BACKEND STREQUAL "auto")
    find_program(AUSPEX_GLSLC glslc)
    find_program(AUSPEX_NVCC nvcc)
    if(AUSPEX_GLSLC)
        set(AUSPEX_WHISPER_BACKEND "vulkan")
    elseif(AUSPEX_NVCC)
        set(AUSPEX_WHISPER_BACKEND "cuda")
    else()
        set(AUSPEX_WHISPER_BACKEND "cpu")
    endif()
    message(STATUS "ASR backend: ${AUSPEX_WHISPER_BACKEND} (auto-detected)")
else()
    message(STATUS "ASR backend: ${AUSPEX_WHISPER_BACKEND} (requested)")
endif()

if(AUSPEX_WHISPER_BACKEND STREQUAL "cpu")
    message(STATUS "  GPU acceleration OFF. For GPU on NVIDIA without the CUDA")
    message(STATUS "  toolkit, install glslc: Debian/Ubuntu/Fedora 'glslc',")
    message(STATUS "  Arch/SUSE/Alpine/Void 'shaderc'. Then reconfigure.")
endif()

set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)

if(AUSPEX_WHISPER_BACKEND STREQUAL "vulkan")
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
elseif(AUSPEX_WHISPER_BACKEND STREQUAL "cuda")
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()

# ggml-vulkan does find_package(SPIRV-Headers CONFIG REQUIRED). That package is
# not installed by default anywhere and its name differs per distro
# (spirv-headers, spirv-headers-devel, ...), so it is fetched instead. This keeps
# the GPU build root-free and identical on every distribution.
#
# OVERRIDE_FIND_PACKAGE (CMake >= 3.24) makes the nested find_package() resolve to
# this content rather than searching the system.
#
# Pinned to 1.4.309.0, which is what Ubuntu's spirv-headers package ships and what
# whisper.cpp v1.9.1 expects. Do NOT pin to the Vulkan *loader* version instead:
# matching the loader (1.3.275 here) gives headers too old for ggml-vulkan, which
# uses spv:: enums added later, and the build fails with "'spv' has not been
# declared". The headers track whisper.cpp's requirements, not the local loader.
if(NOT AUSPEX_WHISPER_BACKEND STREQUAL "cpu")
    FetchContent_Declare(SPIRV-Headers
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
        GIT_TAG        vulkan-sdk-1.4.309.0
        GIT_SHALLOW    TRUE
        OVERRIDE_FIND_PACKAGE
    )
endif()

FetchContent_Declare(whisper_cpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(whisper_cpp)

# Work around an upstream bug in whisper.cpp v1.9.1's Vulkan backend:
# ggml-vulkan.cpp references spv::WordCountShift, spv::OpCodeMask and friends at
# 17 sites but includes no SPIRV header, and ggml-vulkan/CMakeLists.txt calls
# find_package(SPIRV-Headers CONFIG REQUIRED) without ever linking the imported
# target -- so the include directory never propagates and the file cannot compile
# ("'spv' has not been declared") whether or not SPIRV-Headers is installed.
#
# Rather than patch vendored sources, hand the target the include directory and
# force-include the header. Guarded by TARGET so this silently does nothing once
# upstream fixes it or when building CPU-only.
# NB: FetchContent keeps the declared capitalisation here, so the variable is
# SPIRV-Headers_SOURCE_DIR, not the usual all-lowercase form.
set(_spirv_dir "${SPIRV-Headers_SOURCE_DIR}")
if(NOT _spirv_dir)
    set(_spirv_dir "${spirv-headers_SOURCE_DIR}")
endif()

if(TARGET ggml-vulkan AND _spirv_dir)
    target_include_directories(ggml-vulkan PRIVATE "${_spirv_dir}/include")
    target_compile_options(ggml-vulkan PRIVATE -include spirv/unified1/spirv.hpp)
    message(STATUS "ggml-vulkan: force-including spirv.hpp (upstream v1.9.1 omits it)")
endif()

# ---------------------------------------------------------------------------
# miniaudio — decoding + capture, replacing sounddevice/numpy
# ---------------------------------------------------------------------------
add_library(auspex-miniaudio STATIC src/miniaudio_impl.cpp)
target_include_directories(auspex-miniaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(auspex-miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# ---------------------------------------------------------------------------
# auspex-core — config, ollama, asr, tts, audio. No GTK. No Python.
# ---------------------------------------------------------------------------
add_library(auspex-core STATIC
    src/config.cpp
    src/autostart.cpp
    src/tray.cpp
    src/timekeeping.cpp
    src/calendar.cpp
    src/notifications.cpp
    src/ollama_client.cpp
    src/audio.cpp
    src/asr.cpp
    src/tts.cpp
    src/theme.cpp
    src/panel_dock.cpp
    src/process.cpp
    src/desktop.cpp
    src/sysmon.cpp
    src/agents.cpp
    src/commands.cpp
    src/crew.cpp
    src/projects.cpp
    src/sandbox.cpp
    src/director.cpp
    src/coder.cpp
    src/auditor.cpp
    src/crew_run.cpp
    src/code_index.cpp
    src/skills.cpp
    src/vad.cpp
    src/voice_gate.cpp
    src/desktop_entries.cpp
    src/webui.cpp
    src/canvas.cpp
    src/display.cpp
    src/display_x11.cpp
    src/session.cpp
)
target_include_directories(auspex-core PUBLIC include)
target_compile_options(auspex-core PRIVATE ${AUSPEX_WARNINGS})
target_link_libraries(auspex-core PUBLIC
    CURL::libcurl
    ${AUSPEX_JSON_TARGET}
    auspex-miniaudio
    whisper
)

# ---------------------------------------------------------------------------
# CLI tools
# ---------------------------------------------------------------------------
# auspex-session links no GTK -- it only supervises the shell -- so it belongs with
# the CLI tools and is built even in the GTK-free configuration. Without a shell to
# start it is useless, which `auspex-session --check` reports rather than hiding.
foreach(tool probe listen say selftest serve session)
    add_executable(auspex-${tool} apps/${tool}/main.cpp)
    target_compile_options(auspex-${tool} PRIVATE ${AUSPEX_WARNINGS})
    target_link_libraries(auspex-${tool} PRIVATE auspex-core)
endforeach()

# ---------------------------------------------------------------------------
# The GTK4 shell. Off until the widget layer is ported.
# ---------------------------------------------------------------------------
option(AUSPEX_BUILD_SHELL "Build the GTK4 shell (needs gtkmm-4.0 + libadwaita-1)" OFF)

if(AUSPEX_BUILD_SHELL)
    pkg_check_modules(GTKMM4 REQUIRED IMPORTED_TARGET gtkmm-4.0)
    # libadwaita has no packaged C++ bindings on Mint 22.2 (no libadwaitamm),
    # so it is consumed through its C API from C++.
    pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

    add_executable(auspex-shell
        apps/shell/main.cpp
        src/gtk/theming.cpp
        src/gtk/desktop_window.cpp
        src/gtk/panel.cpp
        src/gtk/voice.cpp
        src/gtk/windows.cpp
        src/gtk/tray.cpp
    )
    target_compile_options(auspex-shell PRIVATE ${AUSPEX_WARNINGS})
    target_link_libraries(auspex-shell PRIVATE
        auspex-core PkgConfig::GTKMM4 PkgConfig::ADWAITA)
endif()

install(TARGETS auspex-probe auspex-listen auspex-say auspex-selftest auspex-serve
        RUNTIME DESTINATION bin)
if(AUSPEX_BUILD_SHELL)
    install(TARGETS auspex-shell RUNTIME DESTINATION bin)
endif()
