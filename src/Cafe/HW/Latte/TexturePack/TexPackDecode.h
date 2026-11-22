#pragma once
// texture-pack original-texture decoding for the dumper (emulator independent).
//
// Integer-exact BC1/BC2 decoding (lossless RGBA8 export of the original data, docs/texpack/SPEC.md section 10):
//   - RGB565 -> RGB888 by bit replication (exact endpoints)
//   - four-colour mode: c2 = (2*c0 + c1) / 3, c3 = (c0 + 2*c1) / 3   (integer division, rounds down)
//   - BC1 with c0 <= c1: c2 = (c0 + c1) / 2, c3 = transparent black
//   - BC2: explicit 4-bit alpha expanded as (a << 4) | a; the colour block ALWAYS uses the four-colour mode
//     (D3D/GX2 BC2 semantics, also what the host GPU shows in Cemu).
// Input blocks are little-endian, as stored in Wii U GX2 surfaces.

#include <cstdint>

namespace texpack
{
	// Decodes one 8-byte BC1 block into 16 RGBA8 pixels (row-major, 4x4, 64 bytes).
	void DecodeBC1Block(const uint8_t* block, uint8_t rgbaOut[64]);
	// Decodes one 16-byte BC2 block into 16 RGBA8 pixels (row-major, 4x4, 64 bytes).
	void DecodeBC2Block(const uint8_t* block, uint8_t rgbaOut[64]);
}
