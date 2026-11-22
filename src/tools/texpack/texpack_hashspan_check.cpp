// texture-pack hash-span conformance check.
//
// Compares Cemu's level-0 surface size (LatteAddrLib::GX2CalculateSurfaceInfo, level 0, surfSize)
// with the normative texture-pack hash-span profile (docs/texpack/SPEC.md section 2.2; re-implemented here independently
// of texpack::ComputeProfileLayout)
// for every supported GX2 format and every W x H in 1..MAX. The texture-pack runtime key hashes exactly
// this many bytes starting at the texture's physical image address, so both values must agree.
//
// The profile implies a tile mode per surface: 2D_TILED_THIN1 (4) when its macro condition holds,
// otherwise 1D_TILED_THIN1 (2). In default mode the check passes exactly that tile mode to AddrLib
// and also requires AddrLib to keep it as hardware tile mode. At runtime the texture-pack key is only
// computed when the texture's hardware tile mode equals this profile tile mode.
//
// Usage: texpack_hashspan_check [maxDim=8191] [gx2TileMode=profile]   (8191 = largest key dimension, SPEC 2)
// Exit code 0 = all sizes (and tile modes) agree.

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

#include <cstdio>

namespace
{
	struct ProfileFormat { uint32 gx2; uint32 block; uint32 element; };
	constexpr ProfileFormat kFormats[] = { {0x1a, 1, 4}, {0x31, 4, 8}, {0x32, 4, 16} };

	struct ProfileLevel0 { uint32 tileMode; uint64 length; };

	// hash-span profile of SPEC 2.2 for level 0
	ProfileLevel0 ProfileLayoutLevel0(uint32 w, uint32 h, uint32 block, uint32 element)
	{
		const uint32 columns = (std::max<uint32>(1, w) + block - 1) / block;
		const uint32 rows = (std::max<uint32>(1, h) + block - 1) / block;
		const bool macro = std::max(w, h) >= 16 || (columns >= 32 && rows >= 16);
		const uint32 alignX = macro ? 32 : 8;
		const uint32 alignY = macro ? 16 : 8;
		const uint64 pitch = (columns + alignX - 1) / alignX * alignX;
		const uint64 height = (rows + alignY - 1) / alignY * alignY;
		return { macro ? 4u : 2u, pitch * height * element };
	}
}

int main(int argc, char** argv)
{
	const uint32 maxDim = argc > 1 ? (uint32)atoi(argv[1]) : 8191;
	const bool forcedTileMode = argc > 2;
	const uint32 forcedTileModeValue = forcedTileMode ? (uint32)atoi(argv[2]) : 0;
	uint64 checked = 0, mismatches = 0;
	for (const auto& f : kFormats)
	{
		for (uint32 h = 1; h <= maxDim; h++)
		{
			for (uint32 w = 1; w <= maxDim; w++)
			{
				const ProfileLevel0 profile = ProfileLayoutLevel0(w, h, f.block, f.element);
				const uint32 tileMode = forcedTileMode ? forcedTileModeValue : profile.tileMode;
				LatteAddrLib::AddrSurfaceInfo_OUT info{};
				LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)f.gx2, w, h, 1, Latte::E_DIM::DIM_2D, (Latte::E_GX2TILEMODE)tileMode, 0, 0, &info);
				const bool sizeOk = info.surfSize == profile.length;
				const bool tileOk = forcedTileMode || (uint32)info.hwTileMode == profile.tileMode;
				checked++;
				if (!sizeOk || !tileOk)
				{
					if (mismatches < 40)
						printf("MISMATCH fmt=0x%02x %ux%u cemu=%llu profile=%llu gx2TileMode=%u hwTileMode=%u profileTileMode=%u pitch=%u height=%u\n",
							f.gx2, w, h, (unsigned long long)info.surfSize, (unsigned long long)profile.length, tileMode,
							(uint32)info.hwTileMode, profile.tileMode, info.pitch, info.height);
					mismatches++;
				}
			}
		}
	}
	printf("checked=%llu mismatches=%llu\n", (unsigned long long)checked, (unsigned long long)mismatches);
	return mismatches == 0 ? 0 : 1;
}
