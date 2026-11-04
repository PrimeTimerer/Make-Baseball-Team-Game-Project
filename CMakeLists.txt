cmake_minimum_required(VERSION 3.21)

project(YKTK3PC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Clang/GCC (the Linux build, docs/LINUX.md): the same code model as MSVC's /fp:precise build. The runtime uses SSE4.1
# intrinsics, which these compilers only accept with the ISA enabled (MSVC takes them without /arch). The recompiled
# game code assumes MSVC's semantics: no type-based aliasing, wrapping signed arithmetic, no FMA contraction.
if(NOT MSVC)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
    add_compile_options(-msse4.1 -fno-strict-aliasing -fwrapv -ffp-contract=off)
    # The executable finds the libraries built next to it (SDL3, the PCSX2 GS) in its own folder.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # raylib's GLFW as X11 only: PCSX2's GS renders into an X11 child window of the game window (XWayland on a
    # Wayland desktop).
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()

# runtime/: the PS2Recomp runtime (EE/IOP/VU/GS emulation), recompiler and offline tools, kept in this repository
# with the port's changes (runtime/README.md). thirdparty/: PCSX2's GS and what it needs (thirdparty/README.md).
# The recompiler is prebuilt in tools/ps2_recomp.exe (built from runtime/ps2xRecomp).
# Pass -DURBAN_BUILD_RECOMP=ON to build it from source instead.
option(URBAN_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
# runtime/ps2xTest holds the offline tools: vu1_recomp (VU1 microcode AOT), gs_replay, vu0_audit.
option(URBAN_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
# In-game ImGui "Runtime Debugger" (threads, kernel, IOP/SIF, RPC history, GS, DMA, memory); F1 toggles it.
option(URBAN_DEBUG_UI "Build the ImGui runtime debugger into urban.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${URBAN_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoder for the libmpeg HLE (URBAN_MPEG_HLE=1 only; the FMVs go through the emulated IPU by default).
# A prebuilt LGPL FFmpeg is downloaded at configure time on Windows; elsewhere it is the system's (pkg-config), so it
# is off by default there.
if(WIN32)
    option(URBAN_FFMPEG "Decode MPEG video with FFmpeg" ON)
else()
    option(URBAN_FFMPEG "Decode MPEG video with FFmpeg" OFF)
endif()
set(PS2X_ENABLE_FFMPEG ${URBAN_FFMPEG} CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory(runtime EXCLUDE_FROM_ALL)
# Building ps2_recomp from source comes before any generated code: then only the recompiler is configured.
if(URBAN_BUILD_RECOMP AND NOT EXISTS "${CMAKE_SOURCE_DIR}/generated/register_functions.cpp")
    message(STATUS "Recompiled game code not generated yet: configuring ps2_recomp only")
    return()
endif()
add_subdirectory(src)

# The mod kit's example (modsdk/): ExampleMod.dll (ExampleMod.so on Linux) in build/port/bin/mods/ExampleMod, to try
# the mod API.
option(URBAN_BUILD_MOD_EXAMPLE "Build modsdk/example (ExampleMod.dll / ExampleMod.so)" ON)
if(URBAN_BUILD_MOD_EXAMPLE AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(URBAN_MODS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/mods")
    add_subdirectory(modsdk/example)
    add_dependencies(urban ExampleMod)
endif()

# The mods that ship with the game (mods/): built into build/port/bin/mods/<Name>.
option(URBAN_BUILD_MODS "Build the mods in mods/ (CoopTool: Urban Reign only)" OFF)
if(URBAN_BUILD_MODS AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(URBAN_MODS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/mods")
    add_subdirectory(mods/CoopTool)
    add_dependencies(urban CoopTool)
endif()

# PCSX2's GS as pcsx2gs.dll (docs/GS_PCSX2.md): built with urban, loaded by it when [Video] Renderer=pcsx2. On Linux
# libpcsx2gs.so on Vulkan (docs/LINUX.md).
option(URBAN_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll / libpcsx2gs.so)" ON)
if(URBAN_PCSX2_GS AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    add_dependencies(urban pcsx2gs)
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
    target_link_libraries(urban PRIVATE pcsx2gs_api) # the in-game menu panels (src/urban/mods.cpp)
endif()

# URBAN_LTO: whole-program optimization (/GL, linked with /LTCG) of the runtime, the IOP and the port sources. The
# generated code (~10k files) stays out: LTCG over it would blow up link time and memory (docs/ROADMAP.md A4).
option(URBAN_LTO "Whole-program optimization of the runtime, the IOP and the port (not the generated code)" OFF)
if(MSVC AND URBAN_LTO)
    foreach(target ps2_runtime ps2_iop urban)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/GL>)
    endforeach()
    set_property(TARGET ps2_runtime ps2_iop APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
endif()
