#include "Cafe/HW/Latte/TexturePack/TexPackCemu.h"
#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Pack.h"

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/helpers/helpers.h"

#include <png.h>

#if !BOOST_OS_WINDOWS
#include <unistd.h> // sysconf: physical RAM for the cache budget (Linux, Android, macOS)
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace TexPackCemu
{
	namespace
	{
		fs::path GetTitleDumpPath()
		{
			return ActiveSettings::GetUserDataPath("dump/texture_pack/{:016x}", CafeSystem::GetForegroundTitleId());
		}

		// ---------------- key log ----------------
		std::mutex s_keyLogMutex;
		std::unordered_set<std::string> s_keyLogSeen;

		// ---------------- PNG dump worker ----------------
		struct DumpJob
		{
			fs::path path;
			uint32 width;
			uint32 height;
			std::vector<uint8> rgba;
		};

		std::mutex s_dumpMutex;
		std::condition_variable s_dumpCv;
		std::condition_variable s_dumpIdleCv;
		std::deque<DumpJob> s_dumpQueue;
		std::unordered_set<std::string> s_dumpSeen; // "<base>_mipNN" per title
		bool s_dumpThreadStarted = false;
		bool s_dumpBusy = false;

		bool WritePng(const DumpJob& job)
		{
			png_image image{};
			image.version = PNG_IMAGE_VERSION;
			image.width = job.width;
			image.height = job.height;
			image.format = PNG_FORMAT_RGBA;
			png_alloc_size_t size = 0;
			if (!png_image_write_get_memory_size(image, size, 0, job.rgba.data(), 0, nullptr))
				return false;
			std::vector<uint8> encoded(size);
			if (!png_image_write_to_memory(&image, encoded.data(), &size, 0, job.rgba.data(), 0, nullptr))
				return false;
			// write to a temporary name first so an interrupted session never leaves a truncated PNG with a valid name
			fs::path tmp = job.path;
			tmp += ".part";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				if (!f)
					return false;
				f.write((const char*)encoded.data(), (std::streamsize)size);
				if (!f)
					return false;
			}
			std::error_code ec;
			fs::rename(tmp, job.path, ec);
			return !ec;
		}

		void DumpWorker()
		{
			SetThreadName("TexPackDump");
			while (true)
			{
				DumpJob job;
				{
					std::unique_lock lock(s_dumpMutex);
					s_dumpBusy = false;
					s_dumpIdleCv.notify_all();
					s_dumpCv.wait(lock, [] { return !s_dumpQueue.empty(); });
					job = std::move(s_dumpQueue.front());
					s_dumpQueue.pop_front();
					s_dumpBusy = true;
				}
				std::error_code ec;
				if (fs::exists(job.path, ec))
					continue;
				if (!WritePng(job))
					cemuLog_log(LogType::Force, "TexturePack: failed to write texture dump {}", _pathToUtf8(job.path));
			}
		}
	}

	bool IsDumpActive()
	{
		return ActiveSettings::DumpTexPackTexturesEnabled();
	}

	bool IsKeyingActive()
	{
		return IsDumpActive() || IsReplacementActive();
	}

	texpack::KeyStatus ComputeKey(const LatteTexture* tex, MPTR physImagePtr, texpack::TextureKey& keyOut)
	{
		const texpack::ProfileFormat* profileFormat = texpack::FindProfileFormat((uint32)tex->format);
		if (!profileFormat)
			return texpack::KeyStatus::UnsupportedFormat;
		if (tex->dim != Latte::E_DIM::DIM_2D || tex->depth != 1 || tex->isDepth)
			return texpack::KeyStatus::UnsupportedSurface;
		if (tex->width <= 0 || tex->height <= 0 || tex->width > 8191 || tex->height > 8191)
			return texpack::KeyStatus::InvalidDimensions;
		const uint32 width = (uint32)tex->width;
		const uint32 height = (uint32)tex->height;
		const texpack::ProfileLayout profile = texpack::ComputeProfileLayout(width, height, *profileFormat, 0);

		// the layout Cemu itself uses to read level 0 of this texture
		LatteAddrLib::AddrSurfaceInfo_OUT surfaceInfo{};
		LatteAddrLib::GX2CalculateSurfaceInfo(tex->format, width, height, 1, Latte::E_DIM::DIM_2D, Latte::MakeGX2TileMode(tex->tileMode), 0, 0, &surfaceInfo);
		if ((uint32)surfaceInfo.hwTileMode != profile.hwTileMode)
			return texpack::KeyStatus::TileModeMismatch;
		if ((uint64)surfaceInfo.surfSize != profile.lengthBytes)
			return texpack::KeyStatus::SpanMismatch;

		if (physImagePtr == MPTR_NULL || profile.lengthBytes > 0xFFFFFFFFull)
			return texpack::KeyStatus::DataUnavailable;
		const MPTR virtualAddr = memory_physicalToVirtual(physImagePtr);
		if (!memory_isAddressRangeAccessible(virtualAddr, (uint32)profile.lengthBytes))
			return texpack::KeyStatus::DataUnavailable;
		const uint8* data = memory_getPointerFromPhysicalOffset(physImagePtr);

		keyOut.width = width;
		keyOut.height = height;
		keyOut.gx2Format = (uint32)tex->format;
		keyOut.hash = texpack::HashLevel0(data, (size_t)profile.lengthBytes);
		return texpack::KeyStatus::Ok;
	}

	void LogKey(const LatteTexture* tex, MPTR physImagePtr, texpack::KeyStatus status, const texpack::TextureKey& key)
	{
		// Ok entries are distinct per key; missing keys per surface description (the address is informational only)
		std::string id;
		if (status == texpack::KeyStatus::Ok)
			id = texpack::FormatKeyBaseName(key);
		else
			id = fmt::format("{}:{}x{}:{:x}:{}:{}", texpack::KeyStatusName(status), tex->width, tex->height, (uint32)tex->format, (uint32)tex->tileMode, (uint32)tex->dim);

		std::unique_lock lock(s_keyLogMutex);
		if (!s_keyLogSeen.emplace(id).second)
			return;
		const fs::path dir = GetTitleDumpPath();
		std::error_code ec;
		fs::create_directories(dir, ec);
		const fs::path csvPath = dir / "keys.csv";
		const bool writeHeader = !fs::exists(csvPath, ec);
		std::ofstream csv(csvPath, std::ios::app);
		if (!csv)
			return;
		if (writeHeader)
			csv << "status,key,width,height,gx2_format,dim,hw_tilemode,mip_levels,phys_addr\n";
		csv << fmt::format("{},{},{},{},0x{:x},{},{},{},0x{:08x}\n",
			texpack::KeyStatusName(status), status == texpack::KeyStatus::Ok ? texpack::FormatKeyBaseName(key) : std::string(),
			tex->width, tex->height, (uint32)tex->format, (uint32)tex->dim, (uint32)tex->tileMode, tex->mipLevels, physImagePtr);
	}

	void QueueDump(const texpack::TextureKey& key, uint32 mipIndex, uint32 width, uint32 height, std::vector<uint8>&& rgba)
	{
		if (rgba.size() != (size_t)width * height * 4)
			return;
		const std::string fileName = texpack::FormatDumpFileName(key, mipIndex);
		std::unique_lock lock(s_dumpMutex);
		if (!s_dumpSeen.emplace(fileName).second)
			return;
		const fs::path dir = GetTitleDumpPath();
		std::error_code ec;
		fs::create_directories(dir, ec);
		if (!s_dumpThreadStarted)
		{
			s_dumpThreadStarted = true;
			std::thread(DumpWorker).detach(); // lives for the whole process, idles on the condition variable
		}
		s_dumpQueue.push_back(DumpJob{ dir / fileName, width, height, std::move(rgba) });
		s_dumpCv.notify_one();
	}

	void FlushDumps()
	{
		std::unique_lock lock(s_dumpMutex);
		s_dumpIdleCv.wait_for(lock, std::chrono::seconds(30), [] { return s_dumpQueue.empty() && !s_dumpBusy; });
	}

	// ---------------- KTX2 replacement ----------------
	namespace
	{
		// transcoded data kept for re-uploads: 1/8 of the physical RAM, 256 MiB .. 2 GiB (MH3U measured ~0.9 GiB per
		// session; phones/handhelds get a smaller cache, evicted entries are transcoded again when needed)
		size_t CacheBudgetBytes()
		{
			uint64 ram = 0;
#if BOOST_OS_WINDOWS
			MEMORYSTATUSEX ms{ sizeof(ms) };
			if (GlobalMemoryStatusEx(&ms))
				ram = ms.ullTotalPhys;
#elif defined(_SC_PHYS_PAGES)
			const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
			if (pages > 0 && page > 0)
				ram = (uint64)pages * (uint64)page;
#endif
			const uint64 lo = 256ull << 20, hi = 2048ull << 20;
			return (size_t)(ram ? std::clamp<uint64>(ram / 8, lo, hi) : hi);
		}
		const size_t kCacheBudgetBytes = CacheBudgetBytes();
		// largest replacement edge used: the device's 2D image limit (set by the renderer); the uncompressed RGBA8
		// fallback (no BC7/ASTC on the device) is additionally capped, because 16384x8192 RGBA8 alone needs 512 MiB
		constexpr uint32 kRgba8MaxEdge = 4096;
		std::atomic<uint32> s_deviceMaxEdge{ 16384 };
		constexpr uint8 kMaxRecreates = 8;                         // per surface, guards against flip-flopping guest data
		constexpr int kTranscodeWorkers = 2;                       // background KTX2 transcode threads
		constexpr auto kStatsInterval = std::chrono::seconds(30);  // statistics log line (only when something changed)

		std::mutex s_replMutex;
		std::optional<texpack::TargetFormat> s_rendererTarget;
		// never destroyed: transcode workers may still use it while the process exits
		texpack::ReplacementStore& s_store = *new texpack::ReplacementStore();
		uint64 s_storeTitle = ~0ull;
		std::optional<texpack::TargetFormat> s_storeTarget;
		std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher> s_replLogged;
		thread_local bool s_eligibleScope = false;
		uint32 s_pendingRecreates = 0; // GPU thread only

		void Log(const std::string& line)
		{
			cemuLog_log(LogType::Force, "TexturePack: {}", line);
		}

		// ---------------- statistics ----------------
		struct Counters
		{
			std::atomic<uint64> keyed{};           // sampler textures created with a valid texture-pack key
			std::atomic<uint64> noKey{};           // sampler textures outside the verified TEX profile
			std::atomic<uint64> notInPack{};       // keyed, but the pack has no file for the key
			std::atomic<uint64> replaced{};        // texture objects created with their replacement
			std::atomic<uint64> deferred{};        // created with the original while the replacement is transcoded
			std::atomic<uint64> transcoded{};      // background transcodes finished
			std::atomic<uint64> transcodeFailed{}; // invalid KTX2 (original kept, see the file's log line)
			std::atomic<uint64> packRuleWins{};    // graphic pack size/format rule kept instead of the replacement
			std::atomic<uint64> packOverrides{};   // replacement used instead of a graphic pack size/format rule
			std::atomic<uint64> droppedRenderTarget{};
			std::atomic<uint64> droppedCopy{};
			std::atomic<uint64> recreatedData{};   // recreated because the guest data changed
			std::atomic<uint64> recreatedSwap{};   // recreated because a transcode finished or the option was switched

			uint64 Signature() const
			{
				return keyed + noKey + notInPack + replaced + deferred + transcoded + transcodeFailed + packRuleWins + packOverrides + droppedRenderTarget +
					   droppedCopy + recreatedData + recreatedSwap;
			}
		};
		Counters s_counters;
		std::mutex s_distinctMutex;
		std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher> s_distinctMissing;
		std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher> s_distinctReplaced;

		// true when the key is new in the set
		bool NoteDistinct(std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher>& set, const texpack::TextureKey& key)
		{
			std::unique_lock lock(s_distinctMutex);
			return set.emplace(key).second;
		}

		// ---------------- background transcoding ----------------
		// The GPU thread never transcodes a new replacement itself: a texture whose replacement is not cached yet is
		// created with the original data and recreated with the replacement once a worker has transcoded it.
		struct AsyncState
		{
			std::mutex mutex;
			std::condition_variable cv;
			std::deque<std::pair<texpack::TextureKey, uint32>> queue; // key, generation
			std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher> inFlight;
			std::vector<texpack::TextureKey> ready;
			uint32 generation = 0;
			bool started = false;
		};
		AsyncState& s_async = *new AsyncState(); // never destroyed (detached workers)

		void TranscodeWorker()
		{
			SetThreadName("TexPackTranscode");
			while (true)
			{
				texpack::TextureKey key;
				uint32 generation;
				{
					std::unique_lock lock(s_async.mutex);
					s_async.cv.wait(lock, [] { return !s_async.queue.empty(); });
					key = s_async.queue.front().first;
					generation = s_async.queue.front().second;
					s_async.queue.pop_front();
				}
				auto texture = s_store.Get(key, Log); // logs size and time, or why the file is not used
				std::unique_lock lock(s_async.mutex);
				if (generation != s_async.generation)
					continue; // the store was reset or cleared meanwhile
				s_async.inFlight.erase(key);
				if (texture)
				{
					s_counters.transcoded++;
					s_async.ready.push_back(key);
				}
				else
					s_counters.transcodeFailed++;
			}
		}

		void RequestTranscode(const texpack::TextureKey& key)
		{
			std::unique_lock lock(s_async.mutex);
			if (!s_async.inFlight.emplace(key).second)
				return;
			if (!s_async.started)
			{
				s_async.started = true;
				for (int i = 0; i < kTranscodeWorkers; i++)
					std::thread(TranscodeWorker).detach(); // live for the whole process, idle on the condition variable
			}
			s_async.queue.emplace_back(key, s_async.generation);
			s_async.cv.notify_one();
		}

		// store reset or cleared: queued work and finished results belong to the old state
		void ResetAsync()
		{
			std::unique_lock lock(s_async.mutex);
			s_async.generation++;
			s_async.queue.clear();
			s_async.inFlight.clear();
			s_async.ready.clear();
		}

		size_t PendingTranscodes()
		{
			std::unique_lock lock(s_async.mutex);
			return s_async.inFlight.size();
		}

		// (re)index load/textures/<titleId>/ when the title or the renderer target changes
		void EnsureStore(texpack::TargetFormat target)
		{
			std::unique_lock lock(s_replMutex);
			const uint64 titleId = CafeSystem::GetForegroundTitleId();
			if (titleId == s_storeTitle && s_storeTarget == target)
				return;
			s_storeTitle = titleId;
			s_storeTarget = target;
			s_replLogged.clear();
			const fs::path root = ActiveSettings::GetUserDataPath("load/textures/{:016x}", titleId);
			int ignoredLogged = 0;
			const auto stats = s_store.Reset(root, target, kCacheBudgetBytes, [&](const std::string& m) {
				if (ignoredLogged++ < 50)
					Log(m);
			});
			ResetAsync();
			Log(fmt::format("KTX2 pack {}: {} files, {} keys, {} ignored names, {} identical duplicates, {} conflicts, GPU target {}",
				_pathToUtf8(root), stats.filesSeen, stats.indexed, stats.ignoredNames, stats.identicalDuplicates, stats.conflicts,
				texpack::TargetFormatName(target)));
		}

		std::optional<texpack::TargetFormat> ActiveTarget()
		{
			if (!GetConfig().ktx2_texture_packs)
				return std::nullopt;
			std::unique_lock lock(s_replMutex);
			return s_rendererTarget;
		}

		// a graphic pack texture rule changes size or format of this (not replaced) texture
		bool HasGraphicPackRule(const LatteTexture* tex)
		{
			return tex->overwriteInfo.hasResolutionOverwrite || tex->overwriteInfo.hasFormatOverwrite;
		}

		bool PackWinsOverRules()
		{
			return GetConfig().ktx2_pack_overrides_graphic_packs;
		}

		// the user's priority setting keeps the graphic pack rule for this texture
		bool RuleBlocksReplacement(const LatteTexture* tex)
		{
			return HasGraphicPackRule(tex) && !PackWinsOverRules();
		}

		void ReportStats(bool force)
		{
			static auto s_lastTime = std::chrono::steady_clock::now();
			static uint64 s_lastSignature = 0;
			const auto now = std::chrono::steady_clock::now();
			if (!force && now - s_lastTime < kStatsInterval)
				return;
			s_lastTime = now;
			const uint64 signature = s_counters.Signature();
			if (!force && signature == s_lastSignature)
				return;
			s_lastSignature = signature;
			size_t distinctMissing, distinctReplaced;
			{
				std::unique_lock lock(s_distinctMutex);
				distinctMissing = s_distinctMissing.size();
				distinctReplaced = s_distinctReplaced.size();
			}
			const Counters& c = s_counters;
			Log(fmt::format("stats: sampler textures with key {}, without key {} | not in pack {} ({} distinct) | replaced {} ({} distinct), "
							"waited for transcode {}, transcoded {}, transcode failures {}, pending {} | graphic pack rule kept {}, overridden {} | "
							"dropped: render target {}, GPU copy {} | recreated: data changed {}, swap {}",
				c.keyed.load(), c.noKey.load(), c.notInPack.load(), distinctMissing, c.replaced.load(), distinctReplaced, c.deferred.load(),
				c.transcoded.load(), c.transcodeFailed.load(), PendingTranscodes(), c.packRuleWins.load(), c.packOverrides.load(), c.droppedRenderTarget.load(),
				c.droppedCopy.load(), c.recreatedData.load(), c.recreatedSwap.load()));
		}
	}

	void SetRendererTargetFormat(std::optional<texpack::TargetFormat> target)
	{
		std::unique_lock lock(s_replMutex);
		s_rendererTarget = target;
		if (target)
			cemuLog_log(LogType::Force, "TexturePack: KTX2 replacement GPU target {}", texpack::TargetFormatName(*target));
	}

	void SetRendererMaxImageEdge(uint32 maxEdge)
	{
		s_deviceMaxEdge = std::max<uint32>(maxEdge, 256);
		cemuLog_log(LogType::Force, "TexturePack: device 2D image limit {}, cache budget {} MiB", maxEdge, kCacheBudgetBytes >> 20);
	}

	bool IsReplacementActive()
	{
		return ActiveTarget().has_value();
	}

	ScopedReplacementEligible::ScopedReplacementEligible(bool eligible) : m_prev(s_eligibleScope)
	{
		s_eligibleScope = eligible;
	}

	ScopedReplacementEligible::~ScopedReplacementEligible()
	{
		s_eligibleScope = m_prev;
	}

	void OnTextureCreated(LatteTexture* tex)
	{
		tex->texPackEligible = s_eligibleScope;
		if (!tex->texPackEligible)
			return;
		const auto target = ActiveTarget();
		if (!target)
			return;
		texpack::TextureKey key;
		if (ComputeKey(tex, tex->physAddress, key) != texpack::KeyStatus::Ok)
		{
			s_counters.noKey++;
			return;
		}
		s_counters.keyed++;
		tex->texPackKey = key;
		tex->texPackKeyValid = true;
		EnsureStore(*target);
		if (!s_store.Has(key))
		{
			s_counters.notInPack++;
			// once per key and session: the base name is the pack file name the texture would need (+ ".ktx2")
			if (NoteDistinct(s_distinctMissing, key))
				Log(fmt::format("not in pack: {} ({}x{}, mips {}, tile mode {}, phys 0x{:08x})", texpack::FormatKeyBaseName(key), tex->width, tex->height,
					tex->mipLevels, (uint32)tex->tileMode, tex->physAddress));
			return;
		}
		const bool packRule = HasGraphicPackRule(tex);
		if (packRule && !PackWinsOverRules())
		{
			// the graphic pack rule (overwriteWidth/Height/Format) is kept (user priority); report once per key
			s_counters.packRuleWins++;
			std::unique_lock lock(s_replMutex);
			if (s_replLogged.emplace(key).second)
				Log(fmt::format("not replacing {}: a graphic pack texture rule overwrites its size/format", texpack::FormatKeyBaseName(key)));
			return;
		}
		auto replacement = s_store.TryGetCached(key);
		if (!replacement || replacement->levels.empty())
		{
			// original for now; ApplyFinishedTranscodes recreates the texture once the replacement is ready
			s_counters.deferred++;
			RequestTranscode(key);
			return;
		}
		texpack::ReplacementDesc desc;
		desc.key = key;
		desc.format = replacement->format;
		const uint32 fileLevels = (uint32)replacement->levels.size();
		// device/memory limit: skip top levels until the first used level fits, never below the original size
		const uint32 maxEdge = replacement->format == texpack::TargetFormat::RGBA8 ? std::min(kRgba8MaxEdge, s_deviceMaxEdge.load()) : s_deviceMaxEdge.load();
		uint32 first = 0;
		while (first + 1 < fileLevels && std::max(replacement->levels[first].width, replacement->levels[first].height) > maxEdge &&
			   replacement->levels[first + 1].width >= (uint32)tex->width && replacement->levels[first + 1].height >= (uint32)tex->height)
			first++;
		if (std::max(replacement->levels[first].width, replacement->levels[first].height) > maxEdge)
		{
			std::unique_lock lock(s_replMutex);
			if (s_replLogged.emplace(key).second)
				Log(fmt::format("not replacing {}: {}x{} exceeds the device limit {} even at original size", texpack::FormatKeyBaseName(key),
					replacement->width, replacement->height, maxEdge));
			return;
		}
		desc.firstLevel = first;
		desc.width = replacement->levels[first].width;
		desc.height = replacement->levels[first].height;
		// SPEC 6: at most the level count the game declares for this texture (texture descriptor, LAST_LEVEL + 1).
		// This can be lower than the TEX file's count (MH3U: the 1x1 level is never part of the view), so further KTX2
		// levels are normal and simply not uploaded; a KTX2 with fewer levels is used as it is (loader tolerance, SPEC 6)
		desc.levelCount = std::min<uint32>(fileLevels - first, (uint32)std::max(1, tex->mipLevels));
		desc.srgb = (key.gx2Format & 0x400) != 0;
		tex->texPackReplacement = desc;
		tex->texPackOverrodeRule = packRule; // the replacement's size/format replace the graphic pack rule's
		tex->overwriteInfo.hasResolutionOverwrite = true;
		tex->overwriteInfo.hasFormatOverwrite = false;
		tex->overwriteInfo.width = (sint32)desc.width;
		tex->overwriteInfo.height = (sint32)desc.height;
		tex->overwriteInfo.depth = 1;
		s_counters.replaced++;
		if (packRule)
			s_counters.packOverrides++;
		NoteDistinct(s_distinctReplaced, key);
		std::unique_lock lock(s_replMutex);
		if (s_replLogged.emplace(key).second)
			Log(fmt::format("replacing {} ({}x{}, game uses {} levels) with {}x{}, {} levels, {}{}{}{}{}", texpack::FormatKeyBaseName(key), key.width, key.height,
				tex->mipLevels, desc.width, desc.height, desc.levelCount, texpack::TargetFormatName(desc.format), desc.srgb ? " sRGB" : "",
				packRule ? " (instead of a graphic pack texture rule)" : "",
				fileLevels > desc.levelCount + first ? fmt::format(" (KTX2 has {}, levels beyond the game's count not uploaded)", fileLevels) : "",
				first ? fmt::format(" (top {} level(s) skipped: device limit {})", first, maxEdge) : ""));
	}

	bool UploadReplacement(LatteTexture* tex)
	{
		if (!tex->texPackReplacement)
			return false;
		const texpack::ReplacementDesc& desc = *tex->texPackReplacement;
		// normally cached (the texture was created from the cache); only an evicted entry is transcoded again here
		auto replacement = s_store.Get(desc.key, Log);
		if (!replacement || replacement->format != desc.format || replacement->levels.size() < desc.firstLevel + desc.levelCount ||
			replacement->levels[desc.firstLevel].width != desc.width || replacement->levels[desc.firstLevel].height != desc.height)
		{
			// keep the host image valid (defined layout, no stale data) until the texture is recreated
			Log(fmt::format("replacement for {} no longer available, texture is recreated", texpack::FormatKeyBaseName(desc.key)));
			texpack::TranscodedTexture zero;
			zero.format = desc.format;
			zero.width = desc.width;
			zero.height = desc.height;
			for (uint32 level = 0; level < desc.levelCount; level++)
			{
				texpack::TranscodedLevel lv;
				lv.width = std::max(1u, desc.width >> level);
				lv.height = std::max(1u, desc.height >> level);
				lv.zeroFilled = true;
				lv.data.assign(texpack::TargetLevelSize(desc.format, lv.width, lv.height), 0);
				zero.levels.push_back(std::move(lv));
			}
			g_renderer->texture_loadTexPackReplacement(tex, zero, 0);
			return false;
		}
		return g_renderer->texture_loadTexPackReplacement(tex, *replacement, desc.firstLevel);
	}

	bool NeedsRecreate(const LatteTexture* tex, bool keyValid, const texpack::TextureKey& key)
	{
		if (tex->isUpdatedOnGPU)
			return false;
		if (tex->texPackReplacement)
		{
			const bool changed = !keyValid || key != tex->texPackReplacement->key; // memory reused for other data
			if (changed)
				s_counters.recreatedData++;
			return changed;
		}
		if (!tex->texPackEligible || !keyValid || RuleBlocksReplacement(tex))
			return false;
		const auto target = ActiveTarget();
		if (!target)
			return false;
		EnsureStore(*target);
		if (!s_store.Has(key))
			return false;
		// new data in this texture has a replacement: swap now if it is ready, otherwise once it is transcoded
		if (!s_store.TryGetCached(key))
		{
			RequestTranscode(key);
			return false;
		}
		s_counters.recreatedData++;
		return true;
	}

	void FlagRecreate(LatteTexture* tex)
	{
		if (tex->texPackRecreatePending)
			return;
		if (tex->texPackRecreateCount >= kMaxRecreates)
		{
			if (tex->texPackReplacement && tex->texPackEligible)
			{
				// a surface whose guest data keeps changing (dynamically written atlas, streaming) must never keep a stale
				// replacement: rebuild it once more as a plain, non-replaceable texture (original data path for good)
				tex->texPackEligible = false;
				Log(fmt::format("texture at 0x{:08x} ({}x{}) keeps changing, replacement disabled for this surface (original data)",
					tex->physAddress, tex->width, tex->height));
				tex->texPackRecreatePending = true;
				s_pendingRecreates++;
				return;
			}
			if (tex->texPackRecreateCount++ == kMaxRecreates)
				Log(fmt::format("texture at 0x{:08x} ({}x{}) keeps changing, replacement state no longer updated", tex->physAddress, tex->width, tex->height));
			tex->texPackRecreateCount = std::min<uint32>(tex->texPackRecreateCount, 250);
			return;
		}
		tex->texPackRecreatePending = true;
		s_pendingRecreates++;
	}

	void OnReplacementDropped(bool renderTarget)
	{
		if (renderTarget)
			s_counters.droppedRenderTarget++;
		else
			s_counters.droppedCopy++;
	}

	namespace
	{
		void FlagSwap(LatteTexture* tex, uint32& swapped)
		{
			tex->texPackRecreateCount = 0; // a user switch or a finished transcode is not a flip-flopping surface
			FlagRecreate(tex);
			s_counters.recreatedSwap++;
			swapped++;
		}

		// existing texture without replacement: flag it when its replacement is cached, otherwise queue the transcode
		// (ApplyFinishedTranscodes swaps it later). Same rules as OnTextureCreated, key from current guest memory.
		void SwapInIfAvailable(LatteTexture* tex, texpack::TargetFormat target, uint32& swapped, uint32& queued)
		{
			texpack::TextureKey key;
			if (ComputeKey(tex, tex->physAddress, key) != texpack::KeyStatus::Ok)
				return;
			EnsureStore(target);
			if (!s_store.Has(key))
				return;
			tex->texPackKey = key;
			tex->texPackKeyValid = true;
			if (!s_store.TryGetCached(key))
			{
				RequestTranscode(key);
				queued++;
				return;
			}
			FlagSwap(tex, swapped);
		}

		// priority setting switched while replacement is active
		void ApplyPriorityChange(texpack::TargetFormat target, bool packWins)
		{
			uint32 swapped = 0, queued = 0;
			for (LatteTexture* tex : LatteTexture::GetAllTextures())
			{
				if (!tex)
					continue;
				if (packWins)
				{
					if (!tex->texPackReplacement && tex->texPackEligible && !tex->isUpdatedOnGPU && HasGraphicPackRule(tex))
						SwapInIfAvailable(tex, target, swapped, queued);
				}
				else if (tex->texPackReplacement && tex->texPackOverrodeRule)
					FlagSwap(tex, swapped); // recreated with the graphic pack rule
			}
			const std::string msg = packWins ? fmt::format("KTX2 texture packs take priority over graphic pack rules: replacing {} textures ({} still being transcoded)",
												   swapped + queued, queued)
											 : fmt::format("Graphic pack rules take priority over KTX2 texture packs: {} textures restored", swapped);
			Log(msg);
			LatteOverlay_pushNotification(msg, 4000);
		}

		// a switch of the user options, applied to textures that already exist
		void SyncReplacementOption()
		{
			static std::optional<bool> s_lastActive; // GPU thread only
			static bool s_lastPackWins = false;
			const auto target = ActiveTarget();
			const bool active = target.has_value();
			const bool packWins = PackWinsOverRules();
			if (!s_lastActive)
			{
				// state at the first command batch of the session: nothing exists yet to switch
				s_lastActive = active;
				s_lastPackWins = packWins;
				return;
			}
			const bool priorityChanged = s_lastPackWins != packWins;
			s_lastPackWins = packWins;
			if (*s_lastActive == active)
			{
				if (active && priorityChanged)
					ApplyPriorityChange(*target, packWins);
				return;
			}
			s_lastActive = active;
			uint32 swapped = 0, queued = 0;
			for (LatteTexture* tex : LatteTexture::GetAllTextures())
			{
				if (!tex)
					continue;
				if (active)
				{
					if (!tex->texPackReplacement && tex->texPackEligible && !tex->isUpdatedOnGPU && !RuleBlocksReplacement(tex))
						SwapInIfAvailable(tex, *target, swapped, queued);
				}
				else if (tex->texPackReplacement)
					FlagSwap(tex, swapped);
			}
			if (!active)
			{
				// free the transcoded cache; switching on again re-indexes the pack
				std::unique_lock lock(s_replMutex);
				s_store.Clear();
				s_storeTitle = ~0ull;
				s_storeTarget.reset();
				s_replLogged.clear();
				lock.unlock();
				ResetAsync();
			}
			const std::string msg = active ? fmt::format("KTX2 texture packs on ({}): replacing {} textures ({} still being transcoded)",
												 texpack::TargetFormatName(*target), swapped + queued, queued)
										   : fmt::format("KTX2 texture packs off: {} textures restored", swapped);
			Log(msg);
			LatteOverlay_pushNotification(msg, 4000);
			ReportStats(true);
		}

		// swap textures created with the original data to their replacement once a worker has transcoded it
		void ApplyFinishedTranscodes()
		{
			std::vector<texpack::TextureKey> ready;
			{
				std::unique_lock lock(s_async.mutex);
				if (s_async.ready.empty())
					return;
				ready.swap(s_async.ready);
			}
			if (!ActiveTarget())
				return;
			const std::unordered_set<texpack::TextureKey, texpack::TextureKeyHasher> readySet(ready.begin(), ready.end());
			for (LatteTexture* tex : LatteTexture::GetAllTextures())
			{
				if (!tex || tex->texPackReplacement || !tex->texPackEligible || !tex->texPackKeyValid || tex->isUpdatedOnGPU || RuleBlocksReplacement(tex))
					continue;
				if (readySet.find(tex->texPackKey) == readySet.end())
					continue;
				tex->texPackRecreateCount = 0;
				FlagRecreate(tex);
				s_counters.recreatedSwap++;
			}
		}
	}

	void OnGpuCommandBoundary()
	{
		SyncReplacementOption();
		ApplyFinishedTranscodes();
		ReportStats(false); // only logs when a counter changed
	}

	bool TakePendingRecreates()
	{
		if (s_pendingRecreates == 0)
			return false;
		s_pendingRecreates = 0;
		return true;
	}

	void RearmPendingRecreates()
	{
		s_pendingRecreates++;
	}

	namespace
	{
		constexpr double kFrameWindowSeconds = 1.0 / 60.0;
		constexpr double kSwapBudgetSeconds = 0.5 / 60.0;
		std::chrono::steady_clock::time_point s_swapWindowStart; // GPU thread only
		double s_swapSpent = 0.0;
	}

	bool SwapBudgetAvailable()
	{
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - s_swapWindowStart).count() >= kFrameWindowSeconds)
		{
			s_swapWindowStart = now;
			s_swapSpent = 0.0;
		}
		return s_swapSpent < kSwapBudgetSeconds;
	}

	void ChargeSwapTime(double seconds)
	{
		s_swapSpent += seconds;
	}
}
