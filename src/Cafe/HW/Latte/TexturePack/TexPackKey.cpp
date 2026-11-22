#include "Cafe/HW/Latte/TexturePack/TexPackKey.h"

#include <xxhash.h>

#include <algorithm>
#include <cstdio>

namespace texpack
{
	namespace
	{
		// SPEC 2.1 (TEX header byte for reference): TEX 0x03 -> GX2 0x1a (RGBA8), TEX 0x0b -> GX2 0x31 (BC1), TEX 0x0c -> GX2 0x32 (BC2)
		constexpr ProfileFormat kProfileFormats[] = {
			{0x1a, 0x03, 1, 4},
			{0x31, 0x0b, 4, 8},
			{0x32, 0x0c, 4, 16},
		};

		bool ParseDecimal(std::string_view s, uint32_t& out)
		{
			if (s.empty() || s.size() > 5 || (s.size() > 1 && s[0] == '0'))
				return false;
			uint32_t v = 0;
			for (char c : s)
			{
				if (c < '0' || c > '9')
					return false;
				v = v * 10 + (uint32_t)(c - '0');
			}
			out = v;
			return true;
		}

		bool ParseLowerHex(std::string_view s, size_t maxDigits, uint64_t& out)
		{
			if (s.empty() || s.size() > maxDigits)
				return false;
			uint64_t v = 0;
			for (char c : s)
			{
				uint32_t d;
				if (c >= '0' && c <= '9')
					d = (uint32_t)(c - '0');
				else if (c >= 'a' && c <= 'f')
					d = (uint32_t)(c - 'a' + 10);
				else
					return false;
				v = (v << 4) | d;
			}
			out = v;
			return true;
		}
	}

	const ProfileFormat* FindProfileFormat(uint32_t gx2Format)
	{
		for (const auto& f : kProfileFormats)
			if (f.gx2Format == gx2Format)
				return &f;
		return nullptr;
	}

	ProfileLayout ComputeProfileLayout(uint32_t width, uint32_t height, const ProfileFormat& format, uint32_t level)
	{
		const uint32_t w = std::max<uint32_t>(1, width >> level);
		const uint32_t h = std::max<uint32_t>(1, height >> level);
		const uint32_t columns = (w + format.blockEdge - 1) / format.blockEdge;
		const uint32_t rows = (h + format.blockEdge - 1) / format.blockEdge;
		const bool macro = (level == 0 && std::max(width, height) >= 16) || (columns >= 32 && rows >= 16);
		const uint32_t alignX = macro ? 32 : 8;
		const uint32_t alignY = macro ? 16 : 8;
		ProfileLayout layout;
		layout.hwTileMode = macro ? 4 : 2;
		layout.pitchElements = (columns + alignX - 1) / alignX * alignX;
		layout.heightElements = (rows + alignY - 1) / alignY * alignY;
		layout.lengthBytes = (uint64_t)layout.pitchElements * layout.heightElements * format.elementBytes;
		return layout;
	}

	const char* KeyStatusName(KeyStatus status)
	{
		switch (status)
		{
		case KeyStatus::Ok: return "ok";
		case KeyStatus::UnsupportedFormat: return "unsupported_format";
		case KeyStatus::UnsupportedSurface: return "unsupported_surface";
		case KeyStatus::InvalidDimensions: return "invalid_dimensions";
		case KeyStatus::TileModeMismatch: return "tilemode_mismatch";
		case KeyStatus::SpanMismatch: return "span_mismatch";
		case KeyStatus::DataUnavailable: return "data_unavailable";
		}
		return "unknown";
	}

	uint64_t HashLevel0(const void* data, size_t length)
	{
		return XXH3_64bits(data, length);
	}

	std::string FormatKeyBaseName(const TextureKey& key)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "tex_%ux%u_%016llx_0x%x", key.width, key.height, (unsigned long long)key.hash, key.gx2Format);
		return buf;
	}

	std::string FormatKtx2FileName(const TextureKey& key)
	{
		return FormatKeyBaseName(key) + ".ktx2";
	}

	std::string FormatDumpFileName(const TextureKey& key, uint32_t mipIndex)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "_mip%02u.png", mipIndex);
		return FormatKeyBaseName(key) + buf;
	}

	std::optional<TextureKey> ParseKtx2FileName(std::string_view name)
	{
		constexpr std::string_view prefix = "tex_";
		constexpr std::string_view suffix = ".ktx2";
		if (name.size() <= prefix.size() + suffix.size() || name.substr(0, prefix.size()) != prefix ||
			name.substr(name.size() - suffix.size()) != suffix)
			return std::nullopt;
		std::string_view body = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
		// body: <W>x<H>_<hash16>_0x<fmt>
		const size_t us1 = body.find('_');
		if (us1 == std::string_view::npos)
			return std::nullopt;
		const size_t us2 = body.find('_', us1 + 1);
		if (us2 == std::string_view::npos || body.find('_', us2 + 1) != std::string_view::npos)
			return std::nullopt;
		const std::string_view dims = body.substr(0, us1);
		const std::string_view hash = body.substr(us1 + 1, us2 - us1 - 1);
		const std::string_view fmt = body.substr(us2 + 1);
		const size_t x = dims.find('x');
		if (x == std::string_view::npos)
			return std::nullopt;
		TextureKey key;
		uint64_t hashValue, fmtValue;
		if (!ParseDecimal(dims.substr(0, x), key.width) || !ParseDecimal(dims.substr(x + 1), key.height))
			return std::nullopt;
		if (key.width == 0 || key.height == 0 || key.width > 8191 || key.height > 8191)
			return std::nullopt; // outside the key range (SPEC 2): can never match an original texture
		if (hash.size() != 16 || !ParseLowerHex(hash, 16, hashValue))
			return std::nullopt;
		if (fmt.size() < 3 || fmt.substr(0, 2) != "0x")
			return std::nullopt;
		const std::string_view fmtDigits = fmt.substr(2);
		if ((fmtDigits.size() > 1 && fmtDigits[0] == '0') || !ParseLowerHex(fmtDigits, 8, fmtValue))
			return std::nullopt;
		key.hash = hashValue;
		key.gx2Format = (uint32_t)fmtValue;
		return key;
	}
}
