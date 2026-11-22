#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Pack.h"

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;

namespace texpack
{
	bool ReadWholeFile(const fs::path& path, std::vector<uint8_t>& out)
	{
		out.clear();
		std::ifstream f(path, std::ios::binary | std::ios::ate);
		if (!f)
			return false;
		const std::streamoff size = f.tellg();
		if (size < 0)
			return false;
		out.resize((size_t)size);
		f.seekg(0);
		return size == 0 || (bool)f.read((char*)out.data(), size);
	}

	namespace
	{
		bool SameContents(const fs::path& a, const fs::path& b)
		{
			std::error_code ec;
			if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec)
				return false;
			std::vector<uint8_t> da, db;
			return ReadWholeFile(a, da) && ReadWholeFile(b, db) && da == db;
		}
	}

	Ktx2PackIndex::ScanStats Ktx2PackIndex::Scan(const fs::path& root, const LogFn& log)
	{
		m_entries.clear();
		ScanStats stats;
		std::error_code ec;
		if (!fs::is_directory(root, ec))
			return stats;
		const auto options = fs::directory_options::skip_permission_denied;
		for (auto it = fs::recursive_directory_iterator(root, options, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec))
				continue;
			stats.filesSeen++;
			const fs::path& path = it->path();
			const std::string name = PathToUtf8(path.filename());
			if (name.size() < 5 || name.compare(name.size() - 5, 5, ".ktx2") != 0)
				continue; // other files are not part of the pack contract
			const auto key = ParseKtx2FileName(name);
			if (!key)
			{
				stats.ignoredNames++;
				if (log)
					log("ignored (not an exact texture-pack name): " + PathToUtf8(path));
				continue;
			}
			auto existing = m_entries.find(*key);
			if (existing == m_entries.end())
			{
				m_entries.emplace(*key, Entry{ path, false });
				continue;
			}
			if (existing->second.conflict)
				continue;
			if (SameContents(existing->second.path, path))
			{
				stats.identicalDuplicates++;
				continue;
			}
			existing->second.conflict = true;
			stats.conflicts++;
			if (log)
				log("conflict, key not replaced: " + PathToUtf8(existing->second.path) + " <> " + PathToUtf8(path));
		}
		for (const auto& [key, entry] : m_entries)
			if (!entry.conflict)
				stats.indexed++;
		return stats;
	}

	Ktx2PackIndex::Lookup Ktx2PackIndex::Find(const TextureKey& key, fs::path* pathOut) const
	{
		auto it = m_entries.find(key);
		if (it == m_entries.end())
			return Lookup::NotFound;
		if (it->second.conflict)
			return Lookup::Conflict;
		if (pathOut)
			*pathOut = it->second.path;
		return Lookup::Found;
	}

	Ktx2PackIndex::ScanStats ReplacementStore::Reset(const fs::path& root, TargetFormat target, size_t cacheBudgetBytes, const LogFn& log)
	{
		std::lock_guard lock(m_mutex);
		m_generation++;
		m_cache.clear();
		m_lru.clear();
		m_failed.clear();
		m_stats = {};
		m_target = target;
		m_budget = cacheBudgetBytes;
		return m_index.Scan(root, log);
	}

	void ReplacementStore::Clear()
	{
		std::lock_guard lock(m_mutex);
		m_generation++;
		m_index = {};
		m_cache.clear();
		m_lru.clear();
		m_failed.clear();
		m_stats = {};
	}

	bool ReplacementStore::Has(const TextureKey& key) const
	{
		std::lock_guard lock(m_mutex);
		return m_index.Find(key) == Ktx2PackIndex::Lookup::Found && m_failed.find(key) == m_failed.end();
	}

	ReplacementStore::Stats ReplacementStore::GetStats() const
	{
		std::lock_guard lock(m_mutex);
		return m_stats;
	}

	std::shared_ptr<const TranscodedTexture> ReplacementStore::TryGetCached(const TextureKey& key)
	{
		std::lock_guard lock(m_mutex);
		auto it = m_cache.find(key);
		if (it == m_cache.end())
			return nullptr;
		m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
		m_stats.cacheHits++;
		return it->second.texture;
	}

	std::shared_ptr<const TranscodedTexture> ReplacementStore::Get(const TextureKey& key, const LogFn& log)
	{
		fs::path path;
		TargetFormat target;
		uint64_t generation;
		{
			std::lock_guard lock(m_mutex);
			generation = m_generation;
			if (auto it = m_cache.find(key); it != m_cache.end())
			{
				m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
				m_stats.cacheHits++;
				return it->second.texture;
			}
			if (m_failed.find(key) != m_failed.end())
				return nullptr;
			if (m_index.Find(key, &path) != Ktx2PackIndex::Lookup::Found)
				return nullptr;
			target = m_target;
		}

		// read + transcode outside the lock
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<uint8_t> file;
		auto texture = std::make_shared<TranscodedTexture>();
		std::vector<std::string> messages;
		Ktx2Status status = Ktx2Status::Truncated;
		if (!ReadWholeFile(path, file))
			messages.emplace_back("file could not be read");
		else
			status = TranscodeKtx2(file.data(), file.size(), target, *texture, messages);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		if (log)
		{
			for (const auto& m : messages)
				log(PathToUtf8(path.filename()) + ": " + m);
			if (status == Ktx2Status::Ok)
			{
				char line[256];
				snprintf(line, sizeof(line), "%s: %ux%u, %u levels -> %s, %.1f KiB, %.2f ms", PathToUtf8(path.filename()).c_str(), texture->width, texture->height,
					(unsigned)texture->levels.size(), TargetFormatName(target), texture->ByteSize() / 1024.0, ms);
				log(line);
			}
			else
				log(PathToUtf8(path.filename()) + ": not used (" + Ktx2StatusName(status) + "), original texture kept");
		}

		std::lock_guard lock(m_mutex);
		if (generation != m_generation)
			return status == Ktx2Status::Ok ? texture : nullptr; // store was reset or cleared meanwhile: result is not cached
		if (status != Ktx2Status::Ok)
		{
			m_failed[key] = true;
			m_stats.failures++;
			return nullptr;
		}
		if (auto it = m_cache.find(key); it != m_cache.end()) // another thread was faster
			return it->second.texture;
		m_stats.loads++;
		m_lru.push_front(key);
		m_cache.emplace(key, CacheEntry{ texture, m_lru.begin() });
		m_stats.cachedBytes += texture->ByteSize();
		// evict least recently used entries (never the one just added); textures in use keep their shared_ptr
		while (m_stats.cachedBytes > m_budget && m_lru.size() > 1)
		{
			const TextureKey victim = m_lru.back();
			m_lru.pop_back();
			auto vit = m_cache.find(victim);
			m_stats.cachedBytes -= vit->second.texture->ByteSize();
			m_cache.erase(vit);
			m_stats.evictions++;
		}
		return texture;
	}
}
