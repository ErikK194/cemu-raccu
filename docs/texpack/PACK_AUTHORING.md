# Building a KTX2 texture pack

This guide shows how to get from a game to a working pack. How you create the replacement images themselves
(upscaling, painting, restoring) is up to you; this guide covers what the files must look like so the loader accepts
them. The rules are defined in [`SPEC.md`](SPEC.md).

## 1. Dump the original textures

1. Start the game in this fork (desktop, Vulkan).
2. Turn replacement **off**: *Options → General settings → Graphics → Texture packs (KTX2) → Replace textures*.
   Replaced textures are not dumped.
3. Enable *Debug → Dump → Original textures for KTX2 packs (PNG + keys)*.
4. Play through the scenes you want to cover. Every texture the game loads is written once:

   ```
   <user folder>/dump/texture_pack/<titleid>/
       tex_256x256_cced746c82f692d7_0x31_mip00.png
       tex_256x256_cced746c82f692d7_0x31_mip01.png
       …
       keys.csv
   ```

   (`<user folder>` is `bin/portable/` in portable mode.)

5. Turn the dump off again; it costs time while playing.

The PNGs are lossless RGBA8 copies of the original data (SPEC 10). The part before `_mipNN.png` is the **key** of the
texture, and it is also the name your pack file must have.

## 2. Create the replacement image

Edit or upscale `…_mip00.png`. Keep:

- **the same aspect ratio**, ideally the original size × 2, × 4, … (e.g. 256×256 → 1024×1024);
- **the alpha channel and its meaning.** Alpha is often a mask (transparency, specular, emission), not just opacity.
  Do not drop it or fill it with 255 unless the original is fully opaque;
- **the original look of colours.** The pixel values are used as they are (no colour conversion on load).

## 3. Mip levels

Look up the texture in `keys.csv`: the column `mip_levels` is the number of levels the game uses. The KTX2 needs that
many levels (SPEC 6), each half the size of the previous one:

| Original in `keys.csv` | Replacement level 0 | KTX2 levels |
|---|---|---|
| 256×256, `mip_levels` 8 | 1024×1024 | 8: 1024, 512, … , 8 |
| 1024×512, `mip_levels` 1 (UI) | 4096×2048 | 1: 4096×2048 only |

Do **not** generate a chain down to 1×1 if the original has fewer levels, and do not drop levels. More levels than the
game uses are tolerated (they are not uploaded); fewer are tolerated too, but look worse at a distance.

## 4. Encode to KTX2 (UASTC)

Any encoder that writes a valid KTX2 with the parameters of SPEC 5 works. With
[KTX-Software](https://github.com/KhronosGroup/KTX-Software) (`ktx`, tested with 4.4.2), passing one PNG per level.
Example with three levels; use the level count `N` from step 3 and one PNG per level, largest first:

```
ktx create --format R8G8B8A8_UNORM --assign-tf linear --levels 3 --encode uastc --uastc-quality 4 --zstd 18 level0.png level1.png level2.png tex_256x256_cced746c82f692d7_0x31.ktx2
```

| Parameter | Why |
|---|---|
| `--encode uastc` | required: UASTC LDR 4×4. ETC1S (`basis-lz`) is **rejected** |
| `--format R8G8B8A8_UNORM --assign-tf linear` | the supported GX2 formats are UNORM; values stay unchanged |
| `--levels N` + one PNG per level | exact level count, no automatic mip generation |
| `--uastc-quality 4`, no `--uastc-rdo` | highest quality, no rate-distortion loss |
| `--zstd 18` | Zstandard supercompression (lossless); `--zstd` is optional, other schemes are rejected |

For a single-level texture: `--levels 1` and one input file.

Name the output exactly like the key plus `.ktx2`: lower-case hex, 16 hash digits, no `_mipNN` (SPEC 3).

## 5. Check the files

```
ktx validate tex_256x256_cced746c82f692d7_0x31.ktx2
texpack_ktx2_tool check tex_256x256_cced746c82f692d7_0x31.ktx2
texpack_ktx2_tool scan  <pack folder>
```

- `ktx validate` checks the KTX2 container against the Khronos specification.
- `texpack_ktx2_tool check` runs the emulator's own acceptance checks, transcodes every level to BC7, ASTC 4×4 and
  RGBA8 and compares them; `--ref <png>` additionally compares level 0 with your source image. Exit code 0 = accepted.
- `texpack_ktx2_tool scan` indexes a pack folder like the emulator does and reports invalid names and conflicts.

The tools are built with the emulator (`cmake --build <build dir> --target texpack_ktx2_tool`) and land in
`<build dir>/texpack_tools/`.

## 6. Install and test

1. Put the files into `<user folder>/load/textures/<titleid>/`. Sub-folders are fine, the file name is what counts.
   - Desktop portable: `bin/portable/load/textures/0005000010117200/`
   - Android (this fork, app ID `info.cemu.cemu.texpack`):
     `Android/data/info.cemu.cemu.texpack/files/load/textures/<titleid>/` (copy with `adb push`; the folder is not
     visible over MTP on Android 11+).
2. Enable *Replace textures*. It takes effect immediately, also in a running game.
3. Check `log.txt`:

   | Log line | Meaning |
   |---|---|
   | `KTX2 pack …: N files, N keys, 0 ignored names, 0 identical duplicates, 0 conflicts, GPU target BC7` | pack read; `ignored names` > 0 → wrong file names |
   | `replacing tex_… (…, game uses N levels) with …` | your file is used |
   | `not in pack: tex_…` | the game uses this texture, but the pack has no file for it |
   | `stats: … transcode failures 0 …` | at most every 30 s, when something changed; failures > 0 → run `texpack_ktx2_tool check` on the file |

## 7. Publishing a pack

- Ship only the KTX2 files in the folder layout above (one folder per title ID). No original game data, no dumps.
- Name the game version (title ID, region, game version) your keys were dumped from: a different version can have
  different textures and therefore different keys.
- One pack works on desktop and Android alike; there is nothing platform-specific to build.
