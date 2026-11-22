# Wii U KTX2 Texture Pack – Specification 1.0

This document defines what a Wii U texture pack in KTX2 format must look like, and how a loader must identify,
load and use it. It is game-independent and emulator-independent. Cemu in this repository is the reference
implementation (`src/Cafe/HW/Latte/TexturePack/`).

The key words **must**, **must not**, **should** and **may** are used in their usual normative sense.

| Section | Pack authors | Loader / emulator developers |
|---|---|---|
| 1 Overview | ✓ | ✓ |
| 2 Texture key | (dumper does it for you) | ✓ |
| 3 File names, 4 Pack layout | ✓ | ✓ |
| 5 KTX2 container, 6 Mip levels | ✓ | ✓ |
| 7 GPU formats, 8 Runtime, 9 Errors | | ✓ |
| 10 PNG dump | ✓ (input) | ✓ |

## 1. Overview

- A pack is **one set of KTX2 files for all platforms**. Every file holds one replacement texture with its mip levels,
  encoded as **Basis Universal UASTC LDR 4×4**.
- Each file is matched to an original Wii U texture by its **file name**, which encodes the texture key (section 2).
  There is no path-, archive- or context-based matching.
- At runtime the loader transcodes UASTC into a format the GPU reads directly: **BC7** on desktop GPUs,
  **ASTC 4×4** on mobile GPUs, **RGBA8** as fallback. There are no separate packs per platform.

Why UASTC: it transcodes to ASTC 4×4 bit-exactly (lossless against the UASTC content) and to BC7 at ≥ 44 dB, both at
8 bit per pixel, fast enough for runtime use on a 2020 handheld (measurements in [`README.md`](README.md)).

## 2. Texture key

A Wii U texture is identified by four values:

| Part | Value |
|---|---|
| `W`, `H` | width and height of mip 0 of the **original** texture, 1 … 8191 |
| `HASH` | XXH3-64 over the level-0 bytes (2.2) |
| `FMT` | the original numeric GX2 surface format, all bits |

Width, height and format are **not** fed into the hash. An upscaled replacement keeps the key of its original.

### 2.1 Supported surfaces

A key exists only for a surface that is

- a single 2D image (`GX2_SURFACE_DIM_2D`, depth 1; no array, cube map, 3D, MSAA or depth surface), and
- in one of these GX2 formats (exact value, no masking):

| `FMT` | Format | Element | Bytes per element `E` |
|---|---|---|---|
| `0x1a` | `GX2_SURFACE_FORMAT_TCS_R8_G8_B8_A8_UNORM` | 1×1 pixel | 4 |
| `0x31` | `GX2_SURFACE_FORMAT_T_BC1_UNORM` | 4×4 block | 8 |
| `0x32` | `GX2_SURFACE_FORMAT_T_BC2_UNORM` | 4×4 block | 16 |

Further formats may be added in later versions once their hash span is verified. Existing keys never change.

### 2.2 Hash span

The hash is computed over the **unmodified, tiled level-0 bytes** exactly as they are stored in Wii U memory: no
detiling, no endian swap, no decoding. The length is given by this profile (`B` = 1 for RGBA8, 4 for BC1/BC2):

```
columns = ceil(W / B)
rows    = ceil(H / B)
macro   = max(W, H) >= 16                      // level 0
tile mode         = macro ? 4 (2D_TILED_THIN1) : 2 (1D_TILED_THIN1)
pitch_elements    = align_up(columns, macro ? 32 : 8)
height_elements   = align_up(rows,    macro ? 16 : 8)
length            = pitch_elements * height_elements * E
HASH = XXH3_64bits(level0_bytes, length)        // seed 0, default secret
```

Pitch padding and tiled border areas inside `length` are part of the hash; the hardware tile mode is not a key part.

A loader **must not** produce a key for a surface whose actual hardware tile mode differs from the profile's tile mode,
or whose level-0 size differs from `length`, or whose bytes are not fully readable. Such surfaces are not dumped and
not replaced. (The tool `texpack_hashspan_check` compares this profile with Cemu's GX2 address library for every
format and size.)

The key is recomputed **every time level 0 is (re)loaded**, because games reuse memory for other textures.

## 3. File names

```
tex_<W>x<H>_<HASH>_0x<FMT>.ktx2
```

| Part | Rule |
|---|---|
| `W`, `H` | decimal, no leading zeros, original mip-0 size (not the replacement size) |
| `HASH` | exactly 16 lower-case hex digits, leading zeros kept |
| `FMT` | lower-case hex with `0x` prefix, no leading zeros |
| mip index | none; all levels are inside the KTX2 |

Example: `tex_2048x2048_0dde09b489cef590_0x32.ktx2`

A loader **must** match names exactly. Files ending in `.ktx2` that do not match are ignored (and should be logged);
other files are not part of the pack.

## 4. Pack layout

- One folder per game, named after the 16-digit title ID in lower-case hex. In Cemu:
  `<Cemu user folder>/load/textures/<titleid>/`, e.g. `load/textures/0005000010117200/`.
- Sub-folders are free and only for organisation; only the file name counts.
- The same file name in two places:
  - identical bytes: allowed, one of them is used;
  - different bytes: **conflict**. The key is not replaced and both paths are logged. The loader must not pick one.

## 5. KTX2 container

A file is accepted when all of these hold:

| Field | Requirement |
|---|---|
| Identifier | KTX 2.0 (`«KTX 20»\r\n\x1A\n`) |
| `vkFormat` | `0` (`VK_FORMAT_UNDEFINED`, Basis Universal) |
| Data format descriptor | colour model `KHR_DF_MODEL_UASTC` (166), i.e. **UASTC LDR 4×4**. ETC1S is not accepted |
| Image | one 2D image: `pixelDepth` 0, `layerCount` 0 or 1, `faceCount` 1 |
| Size | `pixelWidth`/`pixelHeight` 1 … 65536 |
| Supercompression | none (0) or **Zstandard (2)** |
| Levels | see section 6; each level's byte range must lie inside the file and have the exact UASTC size |

Recommended encoding for official packs (not checked by the loader):

- UASTC quality level 4 (highest), RDO off, Zstandard supercompression;
- colour space as in the original GX2 format: the three formats of section 2.1 are UNORM, so encode as
  `R8G8B8A8_UNORM` with a linear transfer function (pixel values stored unchanged). The loader takes sRGB/linear from
  the original GX2 format (section 7), not from the file;
- replacement size = original size × a power of two (2×, 4×, …) with the same aspect ratio. Other sizes load, but
  shader-side scaling and LOD mapping work best with power-of-two factors;
- validate every file with `ktx validate` (KTX-Software) and `texpack_ktx2_tool check` (this repository).

## 6. Mip levels

**For pack authors:** the KTX2 has as many levels as the original texture, with the same indices. Level `i` has the
size `max(1, W_r >> i) × max(1, H_r >> i)` of the replacement (`W_r × H_r` = replacement level 0).

- The reference count is the level count of the original game file. If you only have a dump, the `mip_levels` column of
  `keys.csv` (the count the game declares at runtime) is equivalent for the loader; it can be one lower, because some
  games never sample their last level.
- No artificial chain down to 1×1: an original with one level (typical for UI) gives a KTX2 with one level.
- Do not invent levels the original does not have.

**For loaders:**

- Use the valid levels of the file, but **at most as many as the game declares for the texture at runtime** (texture
  descriptor, in Cemu `LAST_LEVEL + 1`). This can be fewer than the original file had; a game may never sample its
  last level. Further KTX2 levels are then simply not uploaded.
- A file with fewer levels than the original is accepted as it is. Missing levels are not generated.
- The loader does not generate, filter or modify mips and does not reorder channels.

## 7. GPU formats and colour

The loader checks the formats the **device actually supports** (not only the OS) and transcodes all used levels into
one target format:

| Platform | 1st choice | 2nd choice | Fallback |
|---|---|---|---|
| Desktop GPUs (Windows, Linux) | BC7 | ASTC 4×4 | RGBA8 |
| Mobile (Android and other ASTC GPUs) | ASTC 4×4 | BC7 | RGBA8 |

Desktop GPUs offer BC7 and almost never ASTC; mobile GPUs offer ASTC almost everywhere, BC7 only on some drivers.
ASTC 4×4 is lossless from UASTC and transcodes about a third faster than BC7 on a Snapdragon 865.

- **sRGB/linear** follows the original GX2 format of the replaced texture.
- Channel meaning, including alpha and material masks, is kept. No normal-map or BC5 special path.
- A loader may offer a test override for the target; it only applies to formats the device supports.

## 8. Runtime behaviour

- **What is replaced:** only textures the game samples. Render targets, copy destinations and read-back copies are
  never replaced. If the game renders or copies into a replaced texture, the replacement is dropped; game output wins.
- **No stalls:** a new replacement must not be transcoded on the render/GPU thread. The original is shown until the
  replacement is ready, then swapped.
- **Unstable surfaces:** if a replaced surface keeps changing its data (e.g. a streamed atlas), the loader must stop
  replacing it rather than ever show stale data.
- **Cache:** transcoded data may be cached and reused; the size should follow device memory.
- **Device limits:** if a replacement exceeds the device's maximum 2D image size, the loader starts at a smaller level
  of the file, **never below the original size**; if that is impossible, the original stays. For the uncompressed
  RGBA8 fallback a loader may cap the edge length further (reference: 4096).
- **Toggling:** a loader may switch replacement on and off at runtime; affected textures are then recreated with
  original or replacement data.

## 9. Error handling

| Case | Behaviour |
|---|---|
| No file for the key | original texture |
| Conflict in the pack (section 4) | original, log |
| File unreadable or not accepted (section 5) | original, log |
| Invalid size or byte range at level `k` | keep levels `0 … k-1`, drop `k` and all later levels, log |
| Level `k > 0` cannot be transcoded | fill that level with zeros, log |
| Level 0 cannot be transcoded | original, log |
| File failed once | remembered for the session, not retried until the pack is re-read |

In every case: no undefined memory access and no emulator crash because of a pack file.

## 10. PNG dump

A dumper exports the **original** textures as input for pack authors.

- One lossless RGBA8 PNG per original mip level that the game actually loads:

  ```
  tex_<W>x<H>_<HASH>_0x<FMT>_mip<NN>.png
  ```

  `W`, `H`, `HASH`, `FMT` are those of mip 0 for all levels; `NN` is the zero-based mip index, at least two digits
  (`mip00`, `mip01`, … `mip10`). The PNG itself has the size of that level. Drop `_mipNN.png` and add `.ktx2` to get
  the pack file name.
- Decoding is integer-exact so all dumpers produce identical PNGs:

  | `FMT` | Decoding |
  |---|---|
  | `0x1a` | bytes per pixel copied unchanged as R, G, B, A |
  | `0x31` BC1 | RGB565 endpoints expanded by bit replication (`r5<<3 \| r5>>2`, `g6<<2 \| g6>>4`). `c0 > c1`: four colours, `(2·c0 + c1) / 3` and `(c0 + 2·c1) / 3` rounded down, alpha 255. `c0 ≤ c1`: three colours, `(c0 + c1) / 2` rounded down, index 3 = black with alpha 0 |
  | `0x32` BC2 | colour block **always** in four-colour mode (GX2/D3D semantics); explicit 4-bit alpha expanded as `(a<<4) \| a` |

- No scaling, filtering, gamma/sRGB conversion or channel changes.
- Only original data: no GPU-written textures, slice 0 only, no replaced textures (turn replacement off for a full
  dump). Graphic pack size/format rules do not change the dump.
- Each name is written at most once per session, atomically (temporary `.part` file, then renamed). An existing file
  is never overwritten.
- While the dump is on, a `keys.csv` next to the PNGs gets one row per key and per kind of rejected surface in each
  session (rows are appended, so a later session can repeat them): `status` (`ok`, `unsupported_format`,
  `unsupported_surface`, `invalid_dimensions`, `tilemode_mismatch`, `span_mismatch`, `data_unavailable`), key, size,
  format, tile mode and the game's mip count (`mip_levels`). Surfaces without a key get no PNG.
- Because levels the game never loads are not dumped, counting PNGs is not a reliable way to get the mip count. Use the
  `mip_levels` column of `keys.csv` or the original game files (section 6).

## Version history

- **1.0 (2026-11-22):** first public version.
