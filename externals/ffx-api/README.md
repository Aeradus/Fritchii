# AMD FidelityFX API headers (subset)

The C headers of AMD's FidelityFX API, used to run AMD FSR 4 in place of a game's own upscale pass
(`src/video_core/renderer_vulkan/vk_ffx_upscaler.cpp`).

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0`
  (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`), `Kits/FidelityFX/api/include/` and
  `Kits/FidelityFX/upscalers/include/ffx_upscale.h`.
- Licence: MIT (the header of each file).
- Changed: `ffx_upscale.h` includes `ffx_api.h` and `ffx_api_types.h` from this folder.

Nothing of AMD's is linked. At run time the emulator loads AMD's signed
`amd_fidelityfx_loader_vk.dll`, which loads `amd_fidelityfx_upscaler_vk.dll`, both from the
SDK's `Kits/FidelityFX/signedbin/` and placed next to the executable. AMD's licence for those
binaries (the SDK's `Kits/FidelityFX/docs/license.md`) allows redistributing them in binary form
with its notice.
