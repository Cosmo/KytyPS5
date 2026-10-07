# spirv_to_dxil

The D3D12 backend turns the SPIR-V that Kyty's shader recompiler makes into DXIL with Mesa's standalone `spirv_to_dxil`, and signs the result with the Windows SDK's `dxil.dll` validator. Nothing of Mesa is involved in rendering: the translation runs when a shader is first used.

`spirv_to_dxil.patch` goes on Mesa (26.2.0-devel, commit `e24dc5bd1e7fe6101bdc866fb16a15a8fcae1aae`; `git apply`). It adds:

- `spirv_to_dxil_linked`, which translates the stages of a graphics pipeline together (the DXIL signatures of consecutive stages only match when the stages are linked)
- `SV_Barycentrics` pixel shader inputs (shader model 6.1; the Xbox does not report support for them)
- the `clip_halfz`, `force_sample_rate_shading` and `shadow_sampler_space_offset` runtime options
- mesh shaders (shader model 6.5; not available on the Xbox): output counts, per-vertex and per-primitive outputs, triangle indices, `SV_CullPrimitive`
- fixes for DXIL validator 1.8 and for wave operations in helper lanes

Build it once, with the static C runtime so that the DLL imports nothing a UWP app does not have (it imports only `KERNEL32.dll` and an API set):

```
meson setup <build> <mesa> --backend=ninja --wrap-mode=nodownload -Dbuildtype=release
  -Db_ndebug=true -Db_vscrt=mt -Dplatforms=windows -Dgallium-drivers= -Dvulkan-drivers=microsoft-experimental
  -Dspirv-to-dxil=true -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dspirv-tools=disabled -Dbuild-tests=false
  -Dvideo-codecs= -Dmin-windows-version=10 -Dzlib=disabled -Dzstd=disabled
ninja -C <build> src/microsoft/spirv_to_dxil/spirv_to_dxil.dll
```

Mesa needs its DirectX-Headers subproject present in `subprojects/` (no downloads). Copy `spirv_to_dxil.h` and `dxil_versions.h` (from `src/microsoft/spirv_to_dxil` and `src/microsoft/compiler`) to `<root>/include`, the import library to `<root>/lib/spirv_to_dxil.lib` and the DLL to `<root>/bin`, then configure Kyty with `-DKYTY_SPIRV_TO_DXIL_ROOT=<root>` (`uwp.ps1 configure` takes it from `$env:KYTY_SPIRV_TO_DXIL_ROOT`). The build puts `spirv_to_dxil.dll` and the SDK's `dxil.dll` next to the executable (for the UWP app, into the package layout).
