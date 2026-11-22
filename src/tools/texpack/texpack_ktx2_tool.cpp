// TexturePack: headless test tool for the KTX2 replacement path. Uses exactly the modules Cemu uses at runtime
// (TexPackKtx2Loader, TexPackKtx2Pack, TexPackKey) plus Basis Universal's independent block decoders
// (basisu::unpack_block: bc7decomp / ASTC reference decoder) to check the transcoded GPU blocks.
//
//   texpack_ktx2_tool check [--ref <png>] [--ref-prefix <prefix>] [--levels <n>] [--full-chain] <file.ktx2>
//       probe + transcode to BC7, ASTC 4x4 and RGBA8; decode BC7/ASTC blocks of every level and compare them with
//       the RGBA8 transcode; optionally compare level 0 with the source PNG (--ref) or every level i with
//       <prefix>_L<i>.png (--ref-prefix). Prints one CSV line (see --csv-header).
//       Exit code 0 = container valid, all levels transcoded, BC7/ASTC decode consistent, (exactly <n> levels),
//       (full chain), (size = PNG).
//   texpack_ktx2_tool --csv-header
//   texpack_ktx2_tool dump <file.ktx2> <level> <bc7|astc|rgba8> <out.raw>
//   texpack_ktx2_tool scan <title folder>
//   texpack_ktx2_tool selftest <valid full-chain .ktx2 with >= 3 levels> <empty temp dir>

#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Loader.h"
#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Pack.h"
#include "Cafe/HW/Latte/TexturePack/TexPackKey.h"

#include <basisu/encoder/basisu_gpu_texture.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace texpack;

namespace
{
	bool ReadPngRgba(const fs::path& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba)
	{
		std::vector<uint8_t> file;
		if (!ReadWholeFile(path, file))
			return false;
		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		if (!png_image_begin_read_from_memory(&image, file.data(), file.size()))
			return false;
		image.format = PNG_FORMAT_RGBA;
		rgba.resize(PNG_IMAGE_SIZE(image));
		if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr))
		{
			png_image_free(&image);
			return false;
		}
		w = image.width;
		h = image.height;
		return true;
	}

	uint32_t MaxLevels(uint32_t w, uint32_t h)
	{
		uint32_t n = 1;
		for (uint32_t m = std::max(w, h); m > 1; m >>= 1)
			n++;
		return n;
	}

	// Decodes BC7 / ASTC 4x4 blocks with Basis Universal's standalone block decoders and returns the largest
	// per-channel difference to the RGBA8 transcode of the same level. -1 = a block could not be decoded.
	int CompareBlocks(const TranscodedLevel& blocks, TargetFormat format, const TranscodedLevel& rgba, double& sqErr, uint64_t& samples)
	{
		const basisu::texture_format tf = format == TargetFormat::BC7 ? basisu::texture_format::cBC7 : basisu::texture_format::cASTC_LDR_4x4;
		const uint32_t bx = (blocks.width + 3) / 4, by = (blocks.height + 3) / 4;
		int maxDiff = 0;
		basisu::color_rgba px[16];
		for (uint32_t y = 0; y < by; y++)
		{
			for (uint32_t x = 0; x < bx; x++)
			{
				// basisu 2.x: astc_srgb = false (raw UNORM decode, compared against the RGBA8 transcode)
				if (!basisu::unpack_block(tf, blocks.data.data() + ((size_t)y * bx + x) * 16, px, false))
					return -1;
				for (uint32_t py = 0; py < 4 && y * 4 + py < rgba.height; py++)
					for (uint32_t pxi = 0; pxi < 4 && x * 4 + pxi < rgba.width; pxi++)
					{
						const uint8_t* ref = rgba.data.data() + (((size_t)(y * 4 + py)) * rgba.width + (x * 4 + pxi)) * 4;
						for (int c = 0; c < 4; c++)
						{
							const int d = std::abs((int)px[py * 4 + pxi][c] - (int)ref[c]);
							maxDiff = std::max(maxDiff, d);
							sqErr += (double)d * d;
							samples++;
						}
					}
			}
		}
		return maxDiff;
	}

	std::string CsvEscape(std::string s)
	{
		std::replace(s.begin(), s.end(), '"', '\'');
		return "\"" + s + "\"";
	}

	const char* kCsvHeader = "file,status,width,height,levels_file,levels_used,full_chain,zero_filled_levels,alpha_flag,"
		"bc7_bytes,astc_bytes,rgba8_bytes,bc7_maxdiff,astc_maxdiff,bc7_psnr,ref_width,ref_height,ref_psnr,ref_maxdiff,"
		"ref_alpha_min,ref_alpha_max,dec_alpha_min,dec_alpha_max,result,messages";

	int CmdCheck(int argc, char** argv)
	{
		fs::path ref, file;
		std::string refPrefix;
		bool fullChain = false;
		int expectLevels = -1;
		for (int i = 2; i < argc; i++)
		{
			std::string a = argv[i];
			if (a == "--ref" && i + 1 < argc)
				ref = Utf8ToPath(argv[++i]);
			else if (a == "--full-chain")
				fullChain = true;
			else if (a == "--levels" && i + 1 < argc)
				expectLevels = std::stoi(argv[++i]); // exact level count (SPEC 6)
			else if (a == "--ref-prefix" && i + 1 < argc)
				refPrefix = argv[++i];               // <prefix>_L<i>.png = prepared input of level i
			else
				file = Utf8ToPath(a);
		}
		std::vector<uint8_t> data;
		std::vector<std::string> messages;
		std::string result = "pass";
		auto fail = [&](const std::string& why) { if (result == "pass") result = "fail"; messages.push_back(why); };

		TranscodedTexture t[3];
		Ktx2Status st[3] = { Ktx2Status::Truncated, Ktx2Status::Truncated, Ktx2Status::Truncated };
		Ktx2Info info{};
		if (!ReadWholeFile(file, data))
			fail("cannot read file");
		else
		{
			ProbeKtx2(data.data(), data.size(), info, messages);
			const TargetFormat targets[3] = { TargetFormat::BC7, TargetFormat::ASTC4x4, TargetFormat::RGBA8 };
			for (int i = 0; i < 3; i++)
			{
				std::vector<std::string> m;
				st[i] = TranscodeKtx2(data.data(), data.size(), targets[i], t[i], m);
				if (i == 2)
					messages.insert(messages.end(), m.begin(), m.end());
				if (st[i] != Ktx2Status::Ok)
					fail(std::string(TargetFormatName(targets[i])) + ": " + Ktx2StatusName(st[i]));
			}
		}
		const bool ok = st[0] == Ktx2Status::Ok && st[1] == Ktx2Status::Ok && st[2] == Ktx2Status::Ok;
		size_t zeroFilled = 0, bytes[3] = {};
		int maxDiff[2] = { 0, 0 };
		double sqErr[2] = { 0, 0 };
		uint64_t samples[2] = { 0, 0 };
		double bc7Psnr = 0;
		int decAlphaMin = 255, decAlphaMax = 0;
		if (ok)
		{
			if (info.usableLevelCount != info.levelCountInFile)
				fail("levels dropped by the reader");
			const bool isFull = t[2].levels.size() == MaxLevels(info.width, info.height);
			if (fullChain && !isFull)
				fail("incomplete mip chain");
			if (expectLevels >= 0 && ((int)info.levelCountInFile != expectLevels || (int)t[2].levels.size() != expectLevels))
				fail("level count " + std::to_string(info.levelCountInFile) + " != expected " + std::to_string(expectLevels));
			for (int i = 0; i < 3; i++)
				bytes[i] = t[i].ByteSize();
			for (size_t level = 0; level < t[2].levels.size(); level++)
			{
				const auto& rgba = t[2].levels[level];
				if (rgba.zeroFilled || t[0].levels[level].zeroFilled || t[1].levels[level].zeroFilled)
					zeroFilled++;
				for (int f = 0; f < 2; f++)
				{
					const auto& lv = t[f].levels[level];
					if (lv.data.size() != TargetLevelSize(t[f].format, lv.width, lv.height) || lv.width != rgba.width || lv.height != rgba.height)
						fail("level size mismatch");
					const int d = CompareBlocks(lv, t[f].format, rgba, sqErr[f], samples[f]);
					if (d < 0)
						fail(std::string("undecodable ") + TargetFormatName(t[f].format) + " block in level " + std::to_string(level));
					maxDiff[f] = std::max(maxDiff[f], d);
				}
			}
			if (zeroFilled)
				fail("zero-filled levels");
			const auto& l0 = t[2].levels[0];
			for (size_t p = 3; p < l0.data.size(); p += 4)
			{
				decAlphaMin = std::min<int>(decAlphaMin, l0.data[p]);
				decAlphaMax = std::max<int>(decAlphaMax, l0.data[p]);
			}
			// ASTC 4x4 from UASTC is lossless (exact match); BC7 from UASTC is near-lossless (all levels >= 40 dB vs. RGBA8)
			const double mse = samples[0] ? sqErr[0] / (double)samples[0] : 0;
			bc7Psnr = mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
			if (bc7Psnr < 40.0)
				fail("BC7 blocks deviate from the RGBA8 transcode");
			if (maxDiff[1] > 0)
				fail("ASTC blocks deviate from the RGBA8 transcode");
		}
		uint32_t rw = 0, rh = 0;
		double psnr = 0;
		int refMax = 0, refAlphaMin = 255, refAlphaMax = 0;
		if (!ref.empty())
		{
			std::vector<uint8_t> png;
			if (!ReadPngRgba(ref, rw, rh, png))
				fail("cannot read reference PNG");
			else
			{
				for (size_t p = 3; p < png.size(); p += 4)
				{
					refAlphaMin = std::min<int>(refAlphaMin, png[p]);
					refAlphaMax = std::max<int>(refAlphaMax, png[p]);
				}
				if (ok && (rw != t[2].width || rh != t[2].height))
					fail("level-0 size differs from the PNG");
				else if (ok)
				{
					const auto& l0 = t[2].levels[0].data;
					double se = 0;
					for (size_t i = 0; i < png.size(); i++)
					{
						const int d = (int)l0[i] - (int)png[i];
						se += (double)d * d;
						refMax = std::max(refMax, std::abs(d));
					}
					const double mse = se / (double)png.size();
					psnr = mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
					// alpha must survive: varying alpha stays varying, opaque stays (nearly) opaque
					if (refAlphaMin != refAlphaMax && decAlphaMin == decAlphaMax)
						fail("alpha variation lost");
					if (refAlphaMin == 255 && decAlphaMin < 250)
						fail("opaque source decoded with transparency");
				}
			}
		}
		// every level against its prepared input (RGBA8 transcode = what all GPU paths are derived from)
		std::string levelLine;
		if (!refPrefix.empty() && ok)
		{
			for (size_t level = 0; level < t[2].levels.size(); level++)
			{
				const auto& lv = t[2].levels[level];
				std::vector<uint8_t> png;
				uint32_t pw = 0, ph = 0;
				const std::string p = refPrefix + "_L" + std::to_string(level) + ".png";
				if (!ReadPngRgba(Utf8ToPath(p), pw, ph, png))
				{
					fail("cannot read level reference " + p);
					break;
				}
				if (pw != lv.width || ph != lv.height)
				{
					fail("level " + std::to_string(level) + " size differs from its reference");
					break;
				}
				double seRgb = 0, seA = 0;
				int maxA = 0;
				for (size_t i = 0; i < png.size(); i += 4)
				{
					for (int c = 0; c < 3; c++)
					{
						const int d = (int)lv.data[i + c] - (int)png[i + c];
						seRgb += (double)d * d;
					}
					const int da = std::abs((int)lv.data[i + 3] - (int)png[i + 3]);
					seA += (double)da * da;
					maxA = std::max(maxA, da);
				}
				const size_t px = png.size() / 4;
				const double mRgb = seRgb / (3.0 * px), mA = seA / px;
				char buf[96];
				snprintf(buf, sizeof(buf), "%sL%zu:%ux%u:%.2f:%.2f:%d", levelLine.empty() ? "" : " ", level, lv.width, lv.height,
					mRgb == 0 ? 99.0 : 10.0 * std::log10(65025.0 / mRgb), mA == 0 ? 99.0 : 10.0 * std::log10(65025.0 / mA), maxA);
				levelLine += buf;
			}
		}
		std::string msg;
		for (const auto& m : messages)
			msg += (msg.empty() ? "" : "; ") + m;
		if (!refPrefix.empty())
			printf("LEVELS %s\n", levelLine.c_str()); // L<i>:<w>x<h>:<rgb psnr>:<alpha psnr>:<alpha max diff>
		printf("%s,%s,%u,%u,%u,%u,%d,%zu,%d,%zu,%zu,%zu,%d,%d,%.3f,%u,%u,%.3f,%d,%d,%d,%d,%d,%s,%s\n", CsvEscape(PathToUtf8(file)).c_str(),
			Ktx2StatusName(st[2]), info.width, info.height, info.levelCountInFile, info.usableLevelCount,
			ok && t[2].levels.size() == MaxLevels(info.width, info.height) ? 1 : 0, zeroFilled, ok && t[2].hasAlpha ? 1 : 0,
			bytes[0], bytes[1], bytes[2], maxDiff[0], maxDiff[1], bc7Psnr, rw, rh, psnr, refMax,
			ref.empty() ? -1 : refAlphaMin, ref.empty() ? -1 : refAlphaMax, ok ? decAlphaMin : -1, ok ? decAlphaMax : -1,
			result.c_str(), CsvEscape(msg).c_str());
		return result == "pass" ? 0 : 1;
	}

	int CmdDump(char** argv)
	{
		std::vector<uint8_t> data;
		if (!ReadWholeFile(Utf8ToPath(argv[2]), data))
			return 2;
		const uint32_t level = (uint32_t)std::stoul(argv[3]);
		const std::string f = argv[4];
		const TargetFormat target = f == "bc7" ? TargetFormat::BC7 : f == "astc" ? TargetFormat::ASTC4x4 : TargetFormat::RGBA8;
		TranscodedTexture t;
		std::vector<std::string> messages;
		const Ktx2Status st = TranscodeKtx2(data.data(), data.size(), target, t, messages);
		for (const auto& m : messages)
			fprintf(stderr, "%s\n", m.c_str());
		if (st != Ktx2Status::Ok || level >= t.levels.size())
		{
			fprintf(stderr, "status %s\n", Ktx2StatusName(st));
			return 1;
		}
		const auto& lv = t.levels[level];
		std::ofstream(Utf8ToPath(argv[5]), std::ios::binary).write((const char*)lv.data.data(), (std::streamsize)lv.data.size());
		printf("%ux%u %zu bytes\n", lv.width, lv.height, lv.data.size());
		return 0;
	}

	int CmdScan(const char* root)
	{
		Ktx2PackIndex index;
		const auto s = index.Scan(Utf8ToPath(root), [](const std::string& m) { printf("  %s\n", m.c_str()); });
		printf("files %zu, indexed keys %zu, ignored names %zu, identical duplicates %zu, conflicts %zu\n", s.filesSeen, s.indexed, s.ignoredNames,
			s.identicalDuplicates, s.conflicts);
		return 0;
	}

	// ---------------- selftest ----------------

	int g_fail = 0;
	void Expect(bool cond, const std::string& what)
	{
		printf("%s  %s\n", cond ? "ok  " : "FAIL", what.c_str());
		if (!cond)
			g_fail++;
	}

	Ktx2Status Load(const std::vector<uint8_t>& d, TranscodedTexture& t, TargetFormat f = TargetFormat::RGBA8)
	{
		std::vector<std::string> m;
		return TranscodeKtx2(d.data(), d.size(), f, t, m);
	}

	void PutU32(std::vector<uint8_t>& d, size_t off, uint32_t v) { memcpy(d.data() + off, &v, 4); }
	uint64_t GetU64(const std::vector<uint8_t>& d, size_t off) { uint64_t v; memcpy(&v, d.data() + off, 8); return v; }

	void WriteFile(const fs::path& p, const std::vector<uint8_t>& d)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p, std::ios::binary).write((const char*)d.data(), (std::streamsize)d.size());
	}

	int CmdSelftest(const char* validPath, const char* tmpDir)
	{
		// --- file names (SPEC 3) ---
		const char* good = "tex_2048x2048_0dde09b489cef590_0x32.ktx2";
		auto k = ParseKtx2FileName(good);
		Expect(k && FormatKtx2FileName(*k) == good, "file name round trip");
		Expect(k && k->width == 2048 && k->height == 2048 && k->hash == 0x0dde09b489cef590ull && k->gx2Format == 0x32, "file name fields");
		Expect(ParseKtx2FileName("tex_64x64_0000000000000001_0x41a.ktx2").has_value(), "sRGB format bits kept (0x41a)");
		for (const char* bad : { "tex_2048x2048_0DDE09B489CEF590_0x32.ktx2", "tex_2048x2048_0dde09b489cef59_0x32.ktx2", "tex_02048x2048_0dde09b489cef590_0x32.ktx2",
				 "tex_2048x2048_0dde09b489cef590_0x032.ktx2", "tex_2048x2048_0dde09b489cef590_0x1A.ktx2", "tex_2048x2048_0dde09b489cef590_0x32.KTX2",
				 "tex_2048x2048_0dde09b489cef590_0x32_mip00.ktx2", "tex_2048x2048_0dde09b489cef590_0x32.png", "tex_0x2048_0dde09b489cef590_0x32.ktx2",
				 "tex_2048x2048_0dde09b489cef590_32.ktx2", "x_tex_2048x2048_0dde09b489cef590_0x32.ktx2" })
			Expect(!ParseKtx2FileName(bad).has_value(), std::string("rejects ") + bad);
		TextureKey zk{ 8, 8, 0, 0x1a };
		Expect(FormatKtx2FileName(zk) == "tex_8x8_0000000000000000_0x1a.ktx2", "leading zeros of the hash are written");
		Expect(FormatDumpFileName(zk, 3) == "tex_8x8_0000000000000000_0x1a_mip03.png", "dump name with two-digit mip");

		// --- container ---
		std::vector<uint8_t> valid;
		if (!ReadWholeFile(Utf8ToPath(validPath), valid))
		{
			printf("cannot read %s\n", validPath);
			return 2;
		}
		TranscodedTexture t;
		Expect(Load(valid, t) == Ktx2Status::Ok && t.levels.size() >= 3, "valid file loads");
		const size_t levels = t.levels.size();
		for (TargetFormat f : { TargetFormat::BC7, TargetFormat::ASTC4x4 })
		{
			TranscodedTexture tf;
			Expect(Load(valid, tf, f) == Ktx2Status::Ok && tf.levels.size() == levels, std::string("valid file -> ") + TargetFormatName(f));
		}

		// truncation anywhere: never Ok (level 0 is stored last), never a crash
		bool allRejected = true;
		std::vector<size_t> cuts;
		for (size_t n = 0; n < std::min<size_t>(valid.size(), 400); n++)
			cuts.push_back(n);
		for (size_t n = 400; n < valid.size(); n += std::max<size_t>(1, valid.size() / 300))
			cuts.push_back(n);
		cuts.push_back(valid.size() - 1);
		for (size_t n : cuts)
		{
			std::vector<uint8_t> d(valid.begin(), valid.begin() + n);
			if (Load(d, t) == Ktx2Status::Ok)
				allRejected = false;
		}
		Expect(allRejected, "truncated files (" + std::to_string(cuts.size()) + " lengths) rejected -> original kept");

		auto mutate = [&](auto fn) { std::vector<uint8_t> d = valid; fn(d); return d; };
		Expect(Load(mutate([](auto& d) { d[1] = 'X'; }), t) == Ktx2Status::NotKtx2, "wrong identifier -> not_ktx2");
		Expect(Load(mutate([](auto& d) { PutU32(d, 12, 37); }), t) == Ktx2Status::NotUastc, "vkFormat R8G8B8A8 (raw KTX2) -> not_uastc");
		Expect(Load(mutate([](auto& d) { PutU32(d, 36, 6); }), t) == Ktx2Status::UnsupportedLayout, "cube map -> unsupported_layout");
		Expect(Load(mutate([](auto& d) { PutU32(d, 32, 4); }), t) == Ktx2Status::UnsupportedLayout, "texture array -> unsupported_layout");
		Expect(Load(mutate([](auto& d) { PutU32(d, 44, 1); }), t) == Ktx2Status::UnsupportedSupercompression, "BasisLZ -> unsupported_supercompression");
		Expect(Load(mutate([](auto& d) { PutU32(d, 20, 0); }), t) == Ktx2Status::InvalidDimensions, "width 0 -> invalid_dimensions");
		{
			// more levels than the size allows: the extra levels are dropped, the rest stays usable (SPEC 9)
			auto d = mutate([&](auto& x) { PutU32(x, 40, (uint32_t)levels + 3); });
			std::vector<std::string> m;
			const Ktx2Status s = TranscodeKtx2(d.data(), d.size(), TargetFormat::RGBA8, t, m);
			Expect(s == Ktx2Status::Ok && t.levels.size() == levels, "level count too large -> extra levels dropped, " + std::to_string(t.levels.size()) + " kept");
		}
		{
			// level index entry of the last level points outside the file: that level is dropped
			auto d = mutate([&](auto& x) { const uint64_t off = x.size() + 100; memcpy(x.data() + 80 + (levels - 1) * 24, &off, 8); });
			const Ktx2Status s = Load(d, t);
			Expect(s == Ktx2Status::Ok && t.levels.size() == levels - 1, "invalid range of the last level -> it is dropped, earlier levels kept");
		}
		{
			// level 1 has a wrong size: levels 1.. dropped, level 0 kept
			auto d = mutate([&](auto& x) { const uint64_t bad = 12345; memcpy(x.data() + 80 + 1 * 24 + 16, &bad, 8); });
			const Ktx2Status s = Load(d, t);
			Expect(s == Ktx2Status::Ok && t.levels.size() == 1, "invalid level-1 size -> levels 1.. dropped, level 0 kept");
		}
		{
			// corrupt Zstd payload of the last level: zero-filled level, texture still valid
			auto d = mutate([&](auto& x) {
				const uint64_t off = GetU64(x, 80 + (levels - 1) * 24), len = GetU64(x, 80 + (levels - 1) * 24 + 8);
				for (uint64_t i = 0; i < len; i++) x[off + i] = 0xA5; });
			const Ktx2Status s = Load(d, t, TargetFormat::BC7);
			const bool zero = s == Ktx2Status::Ok && t.levels.back().zeroFilled &&
				std::all_of(t.levels.back().data.begin(), t.levels.back().data.end(), [](uint8_t v) { return v == 0; }) && !t.levels[0].zeroFilled;
			Expect(zero, "corrupt last-level payload -> that level zero filled, others intact");
		}
		{
			auto d = mutate([&](auto& x) {
				const uint64_t off = GetU64(x, 80), len = GetU64(x, 88);
				for (uint64_t i = 0; i < len; i++) x[off + i] = 0x5A; });
			Expect(Load(d, t) == Ktx2Status::Level0Failed, "corrupt level-0 payload -> level0_failed, original kept");
		}
		{
			std::mt19937 rng(1234);
			int crashesAvoided = 0;
			for (int i = 0; i < 3000; i++)
			{
				std::vector<uint8_t> d = valid;
				const int flips = 1 + (int)(rng() % 8);
				for (int j = 0; j < flips; j++)
				{
					const size_t pos = (i % 2 == 0) ? rng() % std::min<size_t>(d.size(), 256) : rng() % d.size();
					d[pos] ^= (uint8_t)(1u << (rng() % 8));
				}
				Load(d, t, (TargetFormat)(i % 3));
				crashesAvoided++;
			}
			Expect(crashesAvoided == 3000, "3000 random bit-flip variants processed without crash");
		}

		// --- pack index + cache (SPEC 4) ---
		const fs::path root = Utf8ToPath(tmpDir) / "load_textures_title";
		std::error_code ec;
		fs::remove_all(root, ec);
		const std::string nameA = "tex_64x64_00000000000000aa_0x31.ktx2";
		const std::string nameB = "tex_64x64_00000000000000bb_0x32.ktx2";
		WriteFile(root / "a" / nameA, valid);
		WriteFile(root / "b" / "deeper" / nameA, valid);               // identical duplicate
		WriteFile(root / nameB, valid);
		WriteFile(root / "c" / nameB, mutate([](auto& d) { d.back() ^= 1; })); // same key, other content
		WriteFile(root / "tex_64x64_00000000000000AA_0x31.ktx2", valid);       // upper case hex: ignored
		WriteFile(root / "tex_64x64_00000000000000cc_0x31_mip00.ktx2", valid); // mip suffix: ignored
		WriteFile(root / "readme.txt", { 'x' });
		Ktx2PackIndex index;
		const auto s = index.Scan(root);
		Expect(s.indexed == 1 && s.identicalDuplicates == 1 && s.conflicts == 1 && s.ignoredNames == 2, "scan: 1 key, 1 identical duplicate, 1 conflict, 2 ignored names");
		Expect(index.Find(*ParseKtx2FileName(nameA)) == Ktx2PackIndex::Lookup::Found, "identical duplicate -> one entry used");
		Expect(index.Find(*ParseKtx2FileName(nameB)) == Ktx2PackIndex::Lookup::Conflict, "conflicting contents -> never replaced");
		Expect(index.Find(TextureKey{ 64, 64, 0xaa, 0x32 }) == Ktx2PackIndex::Lookup::NotFound, "format is part of the key");
		Expect(index.Find(TextureKey{ 64, 32, 0xaa, 0x31 }) == Ktx2PackIndex::Lookup::NotFound, "size is part of the key");

		ReplacementStore store;
		store.Reset(root, TargetFormat::BC7, 1); // 1 byte budget: only the most recent entry stays cached
		const TextureKey keyA = *ParseKtx2FileName(nameA);
		Expect(!store.TryGetCached(keyA) && store.GetStats().loads == 0, "cache-only lookup never transcodes");
		auto r1 = store.Get(keyA);
		Expect(store.TryGetCached(keyA) == r1, "cache-only lookup returns the transcoded texture");
		auto r2 = store.Get(keyA);
		Expect(r1 && r1 == r2 && store.GetStats().loads == 1 && store.GetStats().cacheHits == 2, "second lookup is a cache hit (no second transcode)");
		Expect(!store.Get(*ParseKtx2FileName(nameB)), "conflict key yields no replacement");
		WriteFile(root / "d" / "tex_8x8_00000000000000dd_0x1a.ktx2", { 1, 2, 3 });
		store.Reset(root, TargetFormat::RGBA8, 1);
		Expect(!store.Get(TextureKey{ 8, 8, 0xdd, 0x1a }) && store.GetStats().failures == 1, "invalid file -> failure remembered, no replacement");
		Expect(!store.Get(TextureKey{ 8, 8, 0xdd, 0x1a }) && store.GetStats().failures == 1, "failed file is not retried");
		Expect(!store.Has(TextureKey{ 8, 8, 0xdd, 0x1a }), "failed key reported as not available");
		fs::remove_all(root, ec);

		printf(g_fail ? "SELFTEST FAIL (%d)\n" : "SELFTEST PASS\n", g_fail);
		return g_fail ? 1 : 0;
	}
}

int main(int argc, char** argv)
{
	const std::string cmd = argc > 1 ? argv[1] : "";
	if (cmd == "--csv-header")
	{
		printf("%s\n", kCsvHeader);
		return 0;
	}
	if (cmd == "check" && argc >= 3)
		return CmdCheck(argc, argv);
	if (cmd == "dump" && argc == 6)
		return CmdDump(argv);
	if (cmd == "scan" && argc == 3)
		return CmdScan(argv[2]);
	if (cmd == "selftest" && argc == 4)
		return CmdSelftest(argv[2], argv[3]);
	fprintf(stderr, "usage: texpack_ktx2_tool check [--ref <png>] [--ref-prefix <prefix>] [--levels <n>] [--full-chain] <file.ktx2> | --csv-header | dump <file> <level> <bc7|astc|rgba8> <out.raw> | scan <dir> | selftest <valid.ktx2> <tmpdir>\n");
	return 2;
}
