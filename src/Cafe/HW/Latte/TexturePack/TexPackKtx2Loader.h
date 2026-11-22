#pragma once
// texture-pack KTX2 reader and transcoder (emulator independent).
//
// Normative reference: docs/texpack/SPEC.md sections 5, 6, 7, 9.
//   - container: KTX2, Basis Universal UASTC LDR 4x4, Zstd supercompression
//   - every present valid mip level is transcoded, no level is generated or post-processed
//   - GPU targets BC7, ASTC 4x4 or RGBA8; the caller picks one by device support (desktop BC7 first, Android ASTC 4x4 first)
//   - error handling: unreadable/invalid container -> no replacement; invalid level dimension -> keep the
//     previous levels and drop it and all following ones; a level that fails to transcode -> zero filled
//
// Only depends on the C++ standard library in this header. The implementation uses the Basis Universal
// transcoder (Apache-2.0) and is the only place that includes it.

#include "Cafe/HW/Latte/TexturePack/TexPackKey.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace texpack
{
	enum class TargetFormat : uint8_t
	{
		BC7,     // 16 bytes per 4x4 block
		ASTC4x4, // 16 bytes per 4x4 block
		RGBA8,   // 4 bytes per pixel, tightly packed rows
	};
	const char* TargetFormatName(TargetFormat format);
	// Bytes of one level of the given size in the given target format.
	size_t TargetLevelSize(TargetFormat format, uint32_t width, uint32_t height);

	// Result of reading only the KTX2 header and level index (no transcoding).
	struct Ktx2Info
	{
		uint32_t width{};
		uint32_t height{};
		uint32_t levelCountInFile{};  // levelCount field (0 is read as 1)
		uint32_t usableLevelCount{};  // levels that pass the header checks (SPEC 9: truncated at the first invalid one)
		uint32_t supercompression{};  // 0 = none, 2 = Zstd
		bool srgbTransfer{};          // DFD transfer function (informational, the host format follows the GX2 format)
		bool hasAlpha{};
	};

	enum class Ktx2Status
	{
		Ok,
		Truncated,            // shorter than its header / level index
		NotKtx2,              // wrong identifier
		NotUastc,             // not Basis Universal UASTC LDR 4x4 (raw vkFormat, ETC1S, UASTC HDR ...)
		UnsupportedLayout,    // not a single 2D image (depth, array layers, cube faces)
		UnsupportedSupercompression,
		InvalidDimensions,    // width/height zero or larger than 65536
		InvalidLevelIndex,    // level 0 data outside the file / wrong size
		TranscoderRejected,   // Basis Universal could not parse the container
		Level0Failed,         // level 0 could not be transcoded: no usable replacement
	};
	const char* Ktx2StatusName(Ktx2Status status);

	struct TranscodedLevel
	{
		uint32_t width{};
		uint32_t height{};
		bool zeroFilled{};           // transcoding failed, buffer is all zero (SPEC 9)
		std::vector<uint8_t> data;   // TargetLevelSize(format, width, height) bytes
	};

	struct TranscodedTexture
	{
		TargetFormat format{};
		uint32_t width{};
		uint32_t height{};
		bool hasAlpha{};
		std::vector<TranscodedLevel> levels;
		size_t ByteSize() const;
	};

	// What an emulator texture object needs to remember about its replacement (the pixel data stays in the cache).
	struct ReplacementDesc
	{
		TextureKey key;
		TargetFormat format{};
		uint32_t width{};       // size of the first used level (file level firstLevel)
		uint32_t height{};
		uint32_t levelCount{};  // levels used, starting at firstLevel
		uint32_t firstLevel{};  // file levels skipped because the device/memory limit is below the file's level 0
		bool srgb{};            // host format variant, from the ORIGINAL GX2 format (0x400 bit), not from the KTX2 DFD
	};

	// Header + level index checks. Does not touch the image payload. `messages` receives one line per
	// tolerated defect (e.g. dropped levels) or the reason for the failure.
	Ktx2Status ProbeKtx2(const uint8_t* data, size_t size, Ktx2Info& info, std::vector<std::string>& messages);

	// Full load: probe, then transcode every usable level into `target`.
	Ktx2Status TranscodeKtx2(const uint8_t* data, size_t size, TargetFormat target, TranscodedTexture& out, std::vector<std::string>& messages);
}
