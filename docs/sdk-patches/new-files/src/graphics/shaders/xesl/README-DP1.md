# XeSL sources for the DP1 shader rebuilds

Copied from xenia-project/xenia at commit 04d5c40d0d (2025-08-19): compiled
unmodified, `resolve_full_32bpp[_scaled].cs.xesl` produce bytecode identical to
the `bytecode/d3d12_5_1/*.h` that ReXGlue ships (checked 2026-09-07). Later
Xenia versions changed the resolve destination to ByteAddressBuffer (2026-02),
which does not match the typed uint4 UAVs the ReXGlue C++ binds.

DP1 changes: `gpu/shaders/resolve.xesli` (shadow alpha for 7e3 render
targets, see d3d12/render_target_cache.cpp in the SDK).

Build (PowerShell, from this directory; Git Bash mangles `/T`):

    $fxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
    & $fxc /nologo /T cs_5_1 /E main /O3 /D SHADING_LANGUAGE_HLSL_XE=1 `
      /Fh ..\bytecode\d3d12_5_1\resolve_full_32bpp_cs.h /Vn resolve_full_32bpp_cs `
      gpu\shaders\resolve_full_32bpp.cs.xesl
    & $fxc /nologo /T cs_5_1 /E main /O3 /D SHADING_LANGUAGE_HLSL_XE=1 `
      /Fh ..\bytecode\d3d12_5_1\resolve_full_32bpp_scaled_cs.h /Vn resolve_full_32bpp_scaled_cs `
      gpu\shaders\resolve_full_32bpp_scaled.cs.xesl
