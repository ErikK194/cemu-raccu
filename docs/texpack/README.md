# KTX2 texture packs for Wii U games

This fork of Cemu replaces Wii U textures with **KTX2 files encoded as Basis Universal UASTC**. One pack runs
unchanged on desktop and Android: at runtime each platform transcodes it into the format its GPU reads directly
(BC7 on desktop, ASTC 4×4 on Android). The approach is game-independent.

| Document | For |
|---|---|
| [`SPEC.md`](SPEC.md) | the specification: texture key, file names, KTX2 requirements, loader rules, PNG dump |
| [`PACK_AUTHORING.md`](PACK_AUTHORING.md) | pack authors: dump, encode, check, install |
| [`INTEGRATION.md`](INTEGRATION.md) | emulator developers: reusable modules, hook points, step-by-step integration |

## Using a pack

1. Copy the pack into `<Cemu user folder>/load/textures/<titleid>/` (e.g. `load/textures/0005000010117200/`).
2. Desktop: *Options → General settings → Graphics → Texture packs (KTX2) → Replace textures*.
   Android: *Settings → Graphics → Replace textures (KTX2 texture packs)*; in game also via the in-game menu
   ("Texture pack (KTX2)") or a hotkey ("Toggle texture pack (KTX2)").
3. The switch takes effect immediately. Vulkan only.

A texture appears in its original form first and switches to the replacement once it is transcoded in the background
(cached replacements appear at once), so transcoding does not stall the game. Only indexing the pack when a game starts
takes a moment.

## Why KTX2 with UASTC

1. **One pack for all platforms.** UASTC is an intermediate format that transcodes into every relevant GPU format.
   The same 7,177 KTX2 files (5.8 GB) of the test pack ran on a PC and on a Retroid Pocket 5 without conversion.
2. **GPU-native formats instead of RGBA8.** Textures stay compressed in video memory (8 instead of 32 bit per pixel).
   This saves memory and, above all, memory bandwidth, the bottleneck on handhelds.
3. **No quality loss on mobile.** UASTC → ASTC 4×4 is bit-identical to the RGBA8 decode, i.e. lossless against the
   UASTC content. UASTC → BC7 is ≥ 44.1 dB.
4. **Fast on weak hardware.** UASTC is designed around ASTC 4×4; on a Snapdragon 865 transcoding to ASTC takes about a
   third less time than to BC7.
5. **Device coverage.** ASTC is available on practically all current Android GPUs (Adreno, Mali, PowerVR, Xclipse) and
   required by the Android Vulkan baseline profile; BC7 only on some Android drivers. Desktop GPUs have BC7 and almost
   never ASTC. UASTC covers both.
6. **No stalls.** Transcoding runs on worker threads; the original stays visible until the replacement is ready.

### Measurements

Test pack: 7,177 KTX2 files (5.8 GB) for Monster Hunter 3 Ultimate (EU, `0005000010117200`), UASTC level 4, Zstd.

**Retroid Pocket 5** (Snapdragon 865, Adreno 650, 8 GB, Android 13), same route (title, village, hunt), about
3 minutes each, 0 transcode failures in both runs:

| | BC7 | ASTC 4×4 |
|---|---|---|
| median per texture | 8.2 ms | 5.2 ms |
| same 363 textures, sum | 16.8 s | 11.1 s (−34 %) |
| 8192×8192 | 1.09 – 1.46 s | 0.58 – 1.28 s |
| 16384×8192 | 1.97 s | 1.31 s |
| memory per pixel | 8 bit | 8 bit |
| quality against UASTC | ≥ 44.1 dB | lossless |

**Desktop** (BC7): median 7 ms per texture, 8192×8192 in 0.4 – 1.2 s, 16384×8192 in 1.6 s.

Limits of the measurement: one game, one Android device, single sessions; Mali, PowerVR and Apple GPUs not measured.
Times are per-file transcode times from the log, not frame times.

## Fixes outside the texture-pack feature

Bugs found and fixed in code this fork took over from Cemu or from the Android port
([cemu-project/Cemu#1909](https://github.com/cemu-project/Cemu/pull/1909)). They are independent of texture packs and
could be offered upstream.

| Area | Problem | Fix |
|---|---|---|
| Android port: touch input (`CanvasOnTouchListener.kt`, overlay `Button.kt`, `DPad.kt`, `Joystick.kt`) | A back gesture (e.g. to open the side menu) starts as a touch on the game image; Android then cancels it with `ACTION_CANCEL`, which was not handled. The emulated touch screen stayed pressed, so games ignored button input (e.g. A in Monster Hunter 3 Ultimate) while the stick still worked, until the next tap. Overlay buttons, D-pad and stick had the same gap. | `ACTION_CANCEL` is treated as release. |
| Android port: controller mapping (`InputMapper.kt`, `InputHandler.kt`) | Built-in gamepads that report analog L2/R2 as `AXIS_BRAKE`/`AXIS_GAS` (e.g. Retroid Pocket 5): these axes were ignored, so ZL/ZR had no analog value (only digital L2/R2 keys, where a device sends them, worked). The automatic "map all inputs" had no default for the HOME key (`KEYCODE_BUTTON_MODE`) for any controller type. | BRAKE/GAS are mapped and forwarded as triggers (stronger value of each pair); HOME is mapped to the emulated HOME button by default. |
| AArch64 recompiler (`BackendAArch64.cpp`) | The code generator (xbyak_aarch64) throws for some functions ("label is too far"; up to 5 functions per session in Monster Hunter 3 Ultimate). The exception was not caught and crashed the emulator on Android. | Code-generator exceptions count as a failed recompilation; the function runs in the interpreter. |
| Debug texture dump (*Debug → Dump → Textures*, desktop; `LatteTextureLoader.h`) | BCn → 8-bit conversion truncated instead of rounding: 45 of the 96 RGB565 endpoint levels came out one step too dark, interpolated colours and BC2–BC5 alpha as well. | Round to nearest for the 30 BC1–BC5 UNORM conversions of the desktop decoders' debug dump. Rendering is unchanged. The decoders the Android port adds (`TextureDecoder_BC4_To_R8`, `…BC5_To_R8G8`, …) are not changed; Android has no debug dump menu. |

Adaptations needed to build the Android port on current upstream Cemu (no behaviour change intended):
`FileStreamUnix::WriteFileAtomic` naming, missing `MotionHandler` include in `DeviceController.h`, `Suspend/ResumeActiveThreads`
using the current active-thread list, `pkgconf` lookup for Android builds on a Windows host, and the basisu vcpkg port
enabled for Android.

## Building

There are no prebuilt downloads; the code is here to document and demonstrate the format. To try it, build it yourself.

Desktop: see [`BUILD.md`](../../BUILD.md); the texture-pack code is part of the normal build. It needs two extra
libraries, **xxHash** and **Basis Universal** (basisu); the vcpkg build gets them automatically (basisu via the
overlay port in `dependencies/vcpkg_overlay_ports/basisu`). Builds without vcpkg (e.g. distribution packages) must
provide both as CMake packages (`xxHash`, `basisu`); basisu's own build does not export one, the overlay port adds it
(`export-cmake-config.diff`). The tools
`texpack_ktx2_tool` and `texpack_hashspan_check` (`src/tools/texpack/`) are built with it and land in
`<build dir>/texpack_tools/`.

Android: the Android port is based on [cemu-project/Cemu#1909](https://github.com/cemu-project/Cemu/pull/1909) and
lives in `src/android/`. Before the first build run `git submodule update --init --recursive`, then build with Gradle
from `src/android/` (`gradlew assembleRelease`; needs the Android SDK, NDK and CMake). The APK installs as "Cemu TexPack" (`info.cemu.cemu.texpack`) next to an existing Cemu.
On first start with no controller mapping, the device's built-in gamepad is mapped automatically (tested on the
Retroid Pocket 5; other Android handhelds with a built-in gamepad, such as AYN devices, use the same path but are not
tested).
