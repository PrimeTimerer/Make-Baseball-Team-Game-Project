# thirdparty/

| folder | what | from | license |
|---|---|---|---|
| `pcsx2/` | PCSX2's GS (hardware renderer), `common/` and the libraries they use (fmt, imgui, cpuinfo, D3D12MA, xbyak, fast_float, WIL, xxhash) | [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) commit `2954ebe` (2026-10-11) | GPL-3.0 (`pcsx2/COPYING.GPLv3`); each library's license is in its folder |
| `pcsx2-gs/` | ours: builds `pcsx2/` into `pcsx2gs.dll` with a C interface (`include/pcsx2gs_api.h`), the host stubs, the shaders/font and the shader cache. See `docs/GS_PCSX2.md` | this project | GPL-3.0 |
| `directx-headers/` | `include/` of DirectX-Headers (`directx/d3d12.h` and friends, needed by PCSX2's DX12 code) | [microsoft/DirectX-Headers](https://github.com/microsoft/DirectX-Headers) commit `461ef51` | MIT (`directx-headers/LICENSE`) |

`pcsx2/` holds only the files the build compiles or includes: the sources listed in `pcsx2-gs/CMakeLists.txt` and
the headers from Ninja's dependency log of a full build (`ninja -t deps`), plus the license files and the whole
`3rdparty/cpuinfo` (it is built with its own CMakeLists). The folder layout is PCSX2's, so a newer PCSX2 can be
copied over the same paths. `pcsx2-gs/tools/make_loaders.py` and `extract_config.py` read a full PCSX2 checkout.

For the Linux build (`docs/LINUX.md`) the same commit's Vulkan renderer was added: `pcsx2/GS/Renderers/Vulkan/`,
`common/Linux/LnxHostSys.cpp` and `LnxThreads.cpp`, `3rdparty/vulkan/` (Vulkan headers and VulkanMemoryAllocator, with
their licenses), `pcsx2/BuildVersion.h` and `common/CocoaTools.h` (included by the Vulkan files; the version values are
in `pcsx2-gs/compat/HostStubs.cpp`). `common/Linux/LnxMisc.cpp` is replaced by `pcsx2-gs/compat/LnxMisc.cpp` (no
dbus/XInput2).

One local change in a PCSX2 file: `pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp` also tries `libshaderc.so.1` when
`libshaderc_shared.so.1` does not load (Debian/Ubuntu's name for shaderc's shared library); marked "Urban Reign port".

YKTK3 port: `pcsx2/GS/GSState.cpp` `Transfer` stops a PACKED packet when finishing its current loop ends the packet or
the data (YKTK3's GIF data comes in pieces that end inside a loop; PCSX2 then ran a register handler past the end with
size 0 and wrapped size around); marked "YKTK3 port".
