#pragma once
// texture-pack Wii U original-texture key (emulator independent).
//
// Normative reference: docs/texpack/SPEC.md sections 2 (key, hash span), 3 (file names), 10 (dump names).
//
// Key = { original W, original H, XXH3-64 (unseeded) over the unmodified tiled GX2 level-0 bytes,
//         original numeric GX2 format }. Dimensions and format are NOT fed into the hash.
//
// This header only depends on the C++ standard library so it can be reused by other emulators/ports.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace texpack
{
	struct TextureKey
	{
		uint32_t width{};     // original Wii U mip-0 width (decimal in file names)
		uint32_t height{};    // original Wii U mip-0 height
		uint64_t hash{};      // XXH3-64 of the tiled level-0 bytes
		uint32_t gx2Format{}; // original GX2 surface format, all bits

		bool operator==(const TextureKey& o) const
		{
			return width == o.width && height == o.height && hash == o.hash && gx2Format == o.gx2Format;
		}
		bool operator!=(const TextureKey& o) const { return !(*this == o); }
	};

	struct TextureKeyHasher
	{
		size_t operator()(const TextureKey& k) const
		{
			return (size_t)(k.hash ^ ((uint64_t)k.gx2Format << 48) ^ ((uint64_t)k.width << 24) ^ (uint64_t)k.height);
		}
	};

	// Original GX2 formats covered by the verified TEX hash profile (exact values, no masking).
	struct ProfileFormat
	{
		uint32_t gx2Format;
		uint32_t texFormat;   // Wii U TEX header format byte
		uint32_t blockEdge;   // B: pixels per element edge (1 = uncompressed, 4 = BCn)
		uint32_t elementBytes;// E: bytes per element
	};
	const ProfileFormat* FindProfileFormat(uint32_t gx2Format);

	// Tiled layout of one mip level according to surface_layout().
	struct ProfileLayout
	{
		uint32_t hwTileMode;  // 4 = 2D_TILED_THIN1 (macro), 2 = 1D_TILED_THIN1 (micro)
		uint32_t pitchElements;
		uint32_t heightElements;
		uint64_t lengthBytes;
	};
	ProfileLayout ComputeProfileLayout(uint32_t width, uint32_t height, const ProfileFormat& format, uint32_t level);

	// Why a surface has (no) texture-pack key. Every value except Ok means: no key, no texture-pack dump, no replacement.
	enum class KeyStatus
	{
		Ok,
		UnsupportedFormat,     // GX2 format not in the verified profile
		UnsupportedSurface,    // not a single 2D image (array, cube, 3D, depth, MSAA ...)
		InvalidDimensions,     // zero or > 8191 (TEX header limit)
		TileModeMismatch,      // hardware tile mode differs from the profile's tile mode
		SpanMismatch,          // emulator surface size differs from the profile length
		DataUnavailable,       // level-0 span not fully readable
	};
	const char* KeyStatusName(KeyStatus status);

	// XXH3-64, seed 0, default secret.
	uint64_t HashLevel0(const void* data, size_t length);

	// "tex_<W>x<H>_<hash16>_0x<fmt>" (shared base of all texture-pack file names)
	std::string FormatKeyBaseName(const TextureKey& key);
	// "tex_<W>x<H>_<hash16>_0x<fmt>.ktx2"
	std::string FormatKtx2FileName(const TextureKey& key);
	// "tex_<W>x<H>_<hash16>_0x<fmt>_mip<NN>.png", NN at least two digits
	std::string FormatDumpFileName(const TextureKey& key, uint32_t mipIndex);

	// Strict parser for "tex_<W>x<H>_<16 lowercase hex>_0x<lowercase hex>.ktx2".
	// Rejects upper case hex, missing leading zeros, leading zeros in W/H, W/H outside 1..8191, other extensions and
	// mip suffixes.
	std::optional<TextureKey> ParseKtx2FileName(std::string_view fileName);
}
