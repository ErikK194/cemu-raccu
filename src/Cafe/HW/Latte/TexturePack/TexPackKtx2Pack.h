#pragma once
// texture-pack KTX2 pack index and replacement cache (emulator independent).
//
// Normative reference: docs/texpack/SPEC.md sections 3, 4, 8, 9.
//   - one folder per title, searched recursively; only exact texture-pack file names are indexed
//   - the key alone selects the file: no path, archive or context information, no alias CSV
//   - the same key in several files: byte-identical copies are one entry, different contents are a conflict and
//     the key is never replaced
//   - transcoded results are cached and reused; nothing is transcoded twice while it is cached

#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Loader.h"
#include "Cafe/HW/Latte/TexturePack/TexPackKey.h"

#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace texpack
{
	using LogFn = std::function<void(const std::string&)>;

	class Ktx2PackIndex
	{
	public:
		struct ScanStats
		{
			size_t filesSeen{};
			size_t indexed{};            // distinct keys that can be replaced
			size_t ignoredNames{};       // *.ktx2 files whose name is not an exact texture-pack key name
			size_t identicalDuplicates{};// extra byte-identical copies of an indexed key
			size_t conflicts{};          // keys with differing contents (never replaced)
		};

		// Replaces the index with the contents of `root` (recursive). A missing folder gives an empty index.
		ScanStats Scan(const std::filesystem::path& root, const LogFn& log = {});

		enum class Lookup { NotFound, Found, Conflict };
		Lookup Find(const TextureKey& key, std::filesystem::path* pathOut = nullptr) const;
		size_t Size() const { return m_entries.size(); }

	private:
		struct Entry
		{
			std::filesystem::path path;
			bool conflict{};
		};
		std::unordered_map<TextureKey, Entry, TextureKeyHasher> m_entries;
	};

	// Thread-safe replacement source: pack index + LRU cache of transcoded textures with a byte budget.
	class ReplacementStore
	{
	public:
		struct Stats
		{
			size_t loads{};        // files read and transcoded
			size_t cacheHits{};
			size_t failures{};     // invalid files (remembered, not retried until the next Reset)
			size_t evictions{};
			size_t cachedBytes{};
		};

		// (Re)initialise for one title folder. Clears the cache.
		Ktx2PackIndex::ScanStats Reset(const std::filesystem::path& root, TargetFormat target, size_t cacheBudgetBytes, const LogFn& log = {});
		void Clear();

		bool Has(const TextureKey& key) const;          // indexed and not a conflict or known failure
		TargetFormat Target() const { return m_target; }

		// Cached or freshly transcoded texture; nullptr when the key has no usable replacement.
		std::shared_ptr<const TranscodedTexture> Get(const TextureKey& key, const LogFn& log = {});
		// Cached texture only (never reads or transcodes); nullptr when not cached.
		std::shared_ptr<const TranscodedTexture> TryGetCached(const TextureKey& key);

		Stats GetStats() const;

	private:
		mutable std::mutex m_mutex;
		Ktx2PackIndex m_index;
		TargetFormat m_target{ TargetFormat::RGBA8 };
		size_t m_budget{};
		using LruList = std::list<TextureKey>;
		struct CacheEntry
		{
			std::shared_ptr<const TranscodedTexture> texture;
			LruList::iterator lru;
		};
		std::unordered_map<TextureKey, CacheEntry, TextureKeyHasher> m_cache;
		std::unordered_map<TextureKey, bool, TextureKeyHasher> m_failed;
		LruList m_lru; // front = most recently used
		Stats m_stats;
		uint64_t m_generation{}; // bumped by Reset/Clear: a Get() that started before must not fill the new cache
	};

	// Reads a whole file; false on any I/O error.
	bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out);

	inline std::string PathToUtf8(const std::filesystem::path& p)
	{
		const auto s = p.u8string();
		return std::string(s.begin(), s.end());
	}
	inline std::filesystem::path Utf8ToPath(const std::string& s)
	{
		return std::filesystem::path(std::u8string(s.begin(), s.end()));
	}
}
