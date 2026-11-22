# Integrating KTX2 texture packs into an emulator

This guide is for developers who want to load Wii U KTX2 texture packs in another Cemu fork, a port or a different
emulator. The format itself is defined in [`SPEC.md`](SPEC.md); this document explains how the reference
implementation in this repository meets it and what you can reuse.

## 1. Reusable modules

These files have no Cemu dependency (C++ standard library, xxHash, Basis Universal transcoder, Zstd; built as C++20
here) and can be copied as they are:

| File | Purpose |
|---|---|
| `TexPackKey.{h,cpp}` | key type, hash-span profile (`ComputeProfileLayout`), XXH3-64, strict file-name formatter/parser |
| `TexPackKtx2Loader.{h,cpp}` | KTX2 acceptance checks (`ProbeKtx2`) and transcoding to BC7 / ASTC 4×4 / RGBA8 (`TranscodeKtx2`), error rules of SPEC 9 |
| `TexPackKtx2Pack.{h,cpp}` | pack index (recursive scan, name check, duplicate/conflict handling) and a thread-safe LRU cache of transcoded textures |
| `TexPackDecode.{h,cpp}` | integer-exact BC1/BC2 decoding for the PNG dump (SPEC 10) |

All of them live in `src/Cafe/HW/Latte/TexturePack/`. At runtime only the Basis Universal **transcoder**
(`basisu_transcoder.cpp`) and Zstd are used. This fork links it through the `basisu::basisu_encoder` library of its vcpkg
overlay port (which contains the transcoder); another emulator can compile just `basisu_transcoder.cpp` instead. The
encoder part is used only by `texpack_ktx2_tool` (block decoders for its checks).

`TexPackCemu.{h,cpp}` is the Cemu glue (hooks, worker threads, statistics, settings). It is not reusable as is, but it
is the best template for your own glue.

## 2. Data flow

```
game loads a texture (guest memory)          load/textures/<titleid>/**/tex_<W>x<H>_<hash>_0x<fmt>.ktx2
        │                                                   │  index built on first use (name check, conflicts)
        ▼                                                   ▼
key = W, H, XXH3-64(tiled level-0 bytes), GX2 format ──► lookup ──► cache of transcoded textures (LRU)
        │                                                   │  not cached: transcode on a worker thread
        ▼                                                   ▼
create the texture with ORIGINAL data            UASTC → BC7 | ASTC 4×4 | RGBA8 (by device support)
        │                                                   │
        └────────── at a safe point between GPU commands ───┘
                    recreate: host image with replacement size/format/levels, upload
```

## 3. Step by step

1. **Choose the target format once** after the GPU device is created (SPEC 7). Query real format support
   (Vulkan: `vkGetPhysicalDeviceFormatProperties` for the UNORM and SRGB variants of `VK_FORMAT_BC7_*_BLOCK` and
   `VK_FORMAT_ASTC_4x4_*_BLOCK`, optimal tiling with sampling, linear filtering and transfer destination). Desktop prefers BC7, Android prefers ASTC 4×4. Also read `maxImageDimension2D`.
   Reference: `VulkanRenderer.cpp`, search for `TexturePack`.
2. **Compute the key** where the emulator (re)loads level 0 of a sampled texture (SPEC 2). Check format, dimension and
   that the emulator's own tile mode and level-0 size equal the profile; otherwise there is no key. Hash the guest
   bytes directly. Recompute on every level-0 load.
   Reference: `TexPackCemu::ComputeKey`, called from `LatteTextureLoader.cpp`.
3. **Look up the pack.** Build the index lazily per title (`Ktx2PackIndex::Scan`). Ask the store whether the key exists;
   if the transcoded data is not cached yet, queue a transcode and keep the original for now.
4. **Transcode off the render thread.** The reference uses two worker threads (`TexPackTranscode`). Never transcode a
   new replacement inside the GPU/command thread: an 8192×8192 texture takes 0.4–1.3 s. (The reference has one
   exception: re-uploading a replaced texture whose data was evicted from the cache meanwhile transcodes it again on the
   GPU thread. With the default cache size this did not occur in testing.)
5. **Swap at a safe point.** Between two GPU commands, recreate the texture object with the replacement's size,
   format and levels and upload all levels. Limit swaps per frame (reference: half a 60 Hz frame of work per frame,
   at least one) so loading many textures does not stall. Upload large textures in slices (reference: 16 MiB staging
   per copy).
6. **Scale the sampler side.** The host image is larger than the guest texture. Everything that depends on the size
   must follow the replacement, the same way resolution rules of graphic packs do in Cemu:
   - shader texture-size scaling and `texelFetch` coordinates;
   - sampler LOD: a min-LOD of 0 stays 0 (full replacement resolution reachable); min-LOD > 0 and max-LOD are shifted
     by `log2(scale)` after clamping to the guest levels; LOD bias, filter and anisotropy stay as the game or a graphic
     pack LOD/anisotropy rule set them;
   - mip filter NONE: only level 0.
7. **Limit levels** to what the game declares at runtime (SPEC 6) and to the device limit (SPEC 8: start at a smaller
   file level, never below the original size).
8. **Protect game output** (SPEC 8): never replace render targets or copy targets; drop the replacement when the game
   renders or copies into the texture; disable replacement for a surface whose data keeps changing (reference: more
   than 8 recreations in a row).
9. **Handle errors** exactly as in SPEC 9. `TranscodeKtx2` already implements the per-level rules.
10. **Optional:** runtime toggle (recreate affected textures, clear the cache), a priority setting against graphic-pack
    size/format rules, and the PNG dumper (SPEC 10).

## 4. Hook points in Cemu

| File | Hook |
|---|---|
| `Core/LatteTexture.cpp` | constructor → `TexPackCemu::OnTextureCreated` (after graphic pack rules); pending recreates |
| `Core/LatteTextureLegacy.cpp` | sampler path marks textures as eligible; no copies or rescale matches with replaced textures |
| `Core/LatteTextureLoader.cpp` | key on level-0 load, recreate on changed data, upload of the replacement chain, PNG dump |
| `Core/LatteAsyncCommands.cpp` | safe point between commands: recreate, toggle, statistics |
| `Core/LatteRenderTarget.cpp`, `Core/LatteSurfaceCopy.cpp` | drop the replacement when rendering/copying into the texture |
| `Renderer/Vulkan/*` | target format selection, image in replacement format/size/levels, upload, sampler LOD |

## 5. Settings and paths in this fork

| Setting (`settings.xml`) | Meaning |
|---|---|
| `Ktx2TexturePacks` | replacement on/off, applies immediately |
| `Ktx2TexturePackPriority` | `false`: a graphic pack size/format rule wins; `true`: the texture pack wins |

- Pack path: `<user folder>/load/textures/<titleid>/` (desktop portable mode: `bin/portable/`; Android:
  `Android/data/<app id>/files/`).
- Test override of the target format: environment variable `CEMU_TEXPACK_TARGET=BC7|ASTC4x4|RGBA8`.
- Log lines start with `TexturePack:` (pack index, `replacing …`, `not in pack: …`, statistics at most every 30 s when something changed).

## 6. Testing your integration

- `texpack_ktx2_tool selftest <valid .ktx2 with a full mip chain, at least 3 levels> <empty temp dir>` tests the
  name parser, the container checks (bit flips, truncation, level rules) and the pack index/cache with the same modules.
- Dump a scene with your dumper and with this fork: the PNG names (keys) must be identical and the decoded pixels of
  every PNG identical (the PNG files themselves may differ in compression).
- In game: every key in the log's `replacing` lines must also appear in `keys.csv`; check the statistics line for
  `transcode failures 0`.

## 7. Known limits of the reference

- Vulkan only. OpenGL and Metal do not register a target format (the PNG dump works there).
- Size queries in shaders (`textureSize`, GPU7 `GET_TEXTURE_RESINFO`) return the replacement size. A shader that derives
  integer `texelFetch` coordinates from the queried size would scale twice. This is the same behaviour as Cemu's
  graphic-pack resolution rules and has not been observed in practice so far.
- Cube maps, arrays, 3D, depth and formats outside SPEC 2.1 are not replaced.
