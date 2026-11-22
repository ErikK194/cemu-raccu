#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Loader.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <basisu/transcoder/basisu_transcoder.h>

namespace texpack
{
	namespace
	{
		constexpr uint8_t kKtx2Identifier[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
		constexpr size_t kHeaderSize = 80;     // identifier .. sgdByteLength
		constexpr size_t kLevelEntrySize = 24; // byteOffset, byteLength, uncompressedByteLength (uint64 each)
		constexpr uint8_t kColorModelUastc = 166; // KHR_DF_MODEL_UASTC
		constexpr uint8_t kTransferSrgb = 2;      // KHR_DF_TRANSFER_SRGB

		uint32_t ReadU32(const uint8_t* p)
		{
			uint32_t v;
			memcpy(&v, p, 4);
			return v;
		}

		uint64_t ReadU64(const uint8_t* p)
		{
			uint64_t v;
			memcpy(&v, p, 8);
			return v;
		}

		uint32_t BlockCount(uint32_t width, uint32_t height)
		{
			return ((width + 3) / 4) * ((height + 3) / 4);
		}

		uint32_t MaxLevelCount(uint32_t width, uint32_t height)
		{
			uint32_t levels = 1;
			for (uint32_t m = std::max(width, height); m > 1; m >>= 1)
				levels++;
			return levels;
		}

		basist::transcoder_texture_format ToBasis(TargetFormat format)
		{
			switch (format)
			{
			case TargetFormat::BC7: return basist::transcoder_texture_format::cTFBC7_RGBA;
			case TargetFormat::ASTC4x4: return basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
			default: return basist::transcoder_texture_format::cTFRGBA32;
			}
		}

		void InitTranscoderOnce()
		{
			static std::once_flag s_once;
			std::call_once(s_once, [] { basist::basisu_transcoder_init(); });
		}
	}

	const char* TargetFormatName(TargetFormat format)
	{
		switch (format)
		{
		case TargetFormat::BC7: return "BC7";
		case TargetFormat::ASTC4x4: return "ASTC4x4";
		case TargetFormat::RGBA8: return "RGBA8";
		}
		return "?";
	}

	size_t TargetLevelSize(TargetFormat format, uint32_t width, uint32_t height)
	{
		if (format == TargetFormat::RGBA8)
			return (size_t)width * height * 4;
		return (size_t)BlockCount(width, height) * 16;
	}

	size_t TranscodedTexture::ByteSize() const
	{
		size_t total = 0;
		for (const auto& level : levels)
			total += level.data.size();
		return total;
	}

	const char* Ktx2StatusName(Ktx2Status status)
	{
		switch (status)
		{
		case Ktx2Status::Ok: return "ok";
		case Ktx2Status::Truncated: return "truncated";
		case Ktx2Status::NotKtx2: return "not_ktx2";
		case Ktx2Status::NotUastc: return "not_uastc_ldr_4x4";
		case Ktx2Status::UnsupportedLayout: return "unsupported_layout";
		case Ktx2Status::UnsupportedSupercompression: return "unsupported_supercompression";
		case Ktx2Status::InvalidDimensions: return "invalid_dimensions";
		case Ktx2Status::InvalidLevelIndex: return "invalid_level_index";
		case Ktx2Status::TranscoderRejected: return "transcoder_rejected";
		case Ktx2Status::Level0Failed: return "level0_failed";
		}
		return "?";
	}

	Ktx2Status ProbeKtx2(const uint8_t* data, size_t size, Ktx2Info& info, std::vector<std::string>& messages)
	{
		info = {};
		if (!data || size < kHeaderSize)
		{
			messages.emplace_back("file shorter than the KTX2 header");
			return Ktx2Status::Truncated;
		}
		if (memcmp(data, kKtx2Identifier, sizeof(kKtx2Identifier)) != 0)
		{
			messages.emplace_back("missing KTX2 identifier");
			return Ktx2Status::NotKtx2;
		}
		const uint32_t vkFormat = ReadU32(data + 12);
		const uint32_t width = ReadU32(data + 20);
		const uint32_t height = ReadU32(data + 24);
		const uint32_t depth = ReadU32(data + 28);
		const uint32_t layers = ReadU32(data + 32);
		const uint32_t faces = ReadU32(data + 36);
		const uint32_t levelCount = std::max(ReadU32(data + 40), 1u);
		const uint32_t scheme = ReadU32(data + 44);
		const uint32_t dfdOffset = ReadU32(data + 48);
		const uint32_t dfdLength = ReadU32(data + 52);

		if (vkFormat != 0)
		{
			messages.emplace_back("vkFormat is not VK_FORMAT_UNDEFINED (not a Basis Universal texture)");
			return Ktx2Status::NotUastc;
		}
		if (depth != 0 || layers > 1 || faces != 1)
		{
			messages.emplace_back("not a single 2D image (depth/layers/faces)");
			return Ktx2Status::UnsupportedLayout;
		}
		if (scheme != 0 && scheme != 2)
		{
			messages.emplace_back("supercompression scheme " + std::to_string(scheme) + " (expected Zstd)");
			return Ktx2Status::UnsupportedSupercompression;
		}
		if (width == 0 || height == 0 || width > 65536 || height > 65536)
		{
			messages.emplace_back("invalid level-0 dimensions");
			return Ktx2Status::InvalidDimensions;
		}
		if ((uint64_t)dfdOffset + dfdLength > size || dfdLength < 44)
		{
			messages.emplace_back("data format descriptor outside the file");
			return Ktx2Status::Truncated;
		}
		const uint8_t* dfd = data + dfdOffset;
		if (dfd[12] != kColorModelUastc)
		{
			messages.emplace_back("color model " + std::to_string(dfd[12]) + " is not UASTC");
			return Ktx2Status::NotUastc;
		}
		if (kHeaderSize + (uint64_t)levelCount * kLevelEntrySize > size)
		{
			messages.emplace_back("level index outside the file");
			return Ktx2Status::Truncated;
		}

		// SPEC 9: keep valid levels, drop the first invalid one and everything after it
		const uint32_t maxLevels = MaxLevelCount(width, height);
		uint32_t usable = std::min(levelCount, maxLevels);
		if (levelCount > maxLevels)
			messages.emplace_back("level count " + std::to_string(levelCount) + " exceeds the " + std::to_string(maxLevels) + " possible levels of " +
				std::to_string(width) + "x" + std::to_string(height) + ", extra levels dropped");
		for (uint32_t level = 0; level < usable; level++)
		{
			const uint8_t* entry = data + kHeaderSize + (size_t)level * kLevelEntrySize;
			const uint64_t offset = ReadU64(entry);
			const uint64_t length = ReadU64(entry + 8);
			const uint64_t unpacked = ReadU64(entry + 16);
			const uint32_t lw = std::max(1u, width >> level);
			const uint32_t lh = std::max(1u, height >> level);
			const uint64_t expected = (uint64_t)BlockCount(lw, lh) * 16;
			const bool inFile = length > 0 && offset <= size && length <= size - offset;
			const bool sizeOk = unpacked == expected && (scheme != 0 || length == expected);
			if (!inFile || !sizeOk)
			{
				messages.emplace_back("level " + std::to_string(level) + " (" + std::to_string(lw) + "x" + std::to_string(lh) + ") has an invalid " +
					(inFile ? "size" : "byte range") + ", it and all following levels dropped");
				usable = level;
				break;
			}
		}
		if (usable == 0)
			return Ktx2Status::InvalidLevelIndex;

		info.width = width;
		info.height = height;
		info.levelCountInFile = levelCount;
		info.usableLevelCount = usable;
		info.supercompression = scheme;
		info.srgbTransfer = dfd[14] == kTransferSrgb;
		const uint8_t channelId = dfd[31] & 0x0F; // first sample: KHR_DF_CHANNEL_UASTC_*
		info.hasAlpha = channelId == 3 /*RGBA*/ || channelId == 5 /*RRRG*/;
		return Ktx2Status::Ok;
	}

	Ktx2Status TranscodeKtx2(const uint8_t* data, size_t size, TargetFormat target, TranscodedTexture& out, std::vector<std::string>& messages)
	{
		out = {};
		Ktx2Info info;
		Ktx2Status status = ProbeKtx2(data, size, info, messages);
		if (status != Ktx2Status::Ok)
			return status;

		// Basis Universal parses every level listed in the header. Levels dropped by the probe are hidden from it
		// by lowering levelCount in a private copy of the file.
		std::vector<uint8_t> patched;
		const uint8_t* src = data;
		if (info.usableLevelCount != info.levelCountInFile)
		{
			patched.assign(data, data + size);
			memcpy(patched.data() + 40, &info.usableLevelCount, 4);
			src = patched.data();
		}

		InitTranscoderOnce();
		basist::ktx2_transcoder transcoder;
		if (!transcoder.init(src, (uint32_t)size) || !transcoder.is_uastc() || !transcoder.start_transcoding())
		{
			messages.emplace_back("Basis Universal rejected the container");
			return Ktx2Status::TranscoderRejected;
		}
		basist::ktx2_transcoder_state state;
		state.clear();

		out.format = target;
		out.width = info.width;
		out.height = info.height;
		out.hasAlpha = transcoder.get_has_alpha() != 0;
		out.levels.resize(info.usableLevelCount);
		for (uint32_t level = 0; level < info.usableLevelCount; level++)
		{
			TranscodedLevel& dst = out.levels[level];
			dst.width = std::max(1u, info.width >> level);
			dst.height = std::max(1u, info.height >> level);
			dst.data.assign(TargetLevelSize(target, dst.width, dst.height), 0);

			basist::ktx2_image_level_info levelInfo{};
			bool ok = transcoder.get_image_level_info(levelInfo, level, 0, 0) &&
				levelInfo.m_orig_width == dst.width && levelInfo.m_orig_height == dst.height;
			if (ok)
			{
				if (target == TargetFormat::RGBA8)
					ok = transcoder.transcode_image_level(level, 0, 0, dst.data.data(), dst.width * dst.height, ToBasis(target), 0, dst.width, dst.height, -1, -1, &state);
				else
					ok = transcoder.transcode_image_level(level, 0, 0, dst.data.data(), levelInfo.m_total_blocks, ToBasis(target), 0, 0, 0, -1, -1, &state);
			}
			if (!ok)
			{
				// SPEC 9: zero-initialised buffer for this level, keep the texture valid
				std::fill(dst.data.begin(), dst.data.end(), 0);
				dst.zeroFilled = true;
				messages.emplace_back("level " + std::to_string(level) + " could not be transcoded to " + TargetFormatName(target) + ", zero filled");
				if (level == 0)
				{
					out = {};
					return Ktx2Status::Level0Failed;
				}
			}
		}
		return Ktx2Status::Ok;
	}
}
