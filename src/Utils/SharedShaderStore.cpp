#include "Utils/SharedShaderStore.h"

#include <ORGModuleServices/ShaderStore.h>

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

namespace Util
{
	namespace
	{
		std::atomic<org::services::ShaderStore*> g_openedStore{ nullptr };

		/**
		 * @brief Deletes what the old cache left in Data/ShaderCache: everything except the store's own folder.
		 * This can be thousands of files, so it runs on its own thread, and a later start finishes what an exit interrupted.
		 */
		void RemoveOldCacheFiles()
		{
			std::thread([] {
				std::error_code ec;
				uint64_t removed = 0;
				for (const auto& entry : std::filesystem::directory_iterator(L"Data/ShaderCache", ec)) {
					if (entry.path().filename().wstring() == kShaderStoreDirName)
						continue;
					std::error_code removeError;
					removed += std::filesystem::remove_all(entry.path(), removeError);
					if (removeError)
						logger::warn("Could not delete old shader cache entry {}: {}", WStringToString(entry.path().wstring()), removeError.message());
				}
				if (removed)
					logger::info("Deleted {} files of the old shader cache", removed);
			}).detach();
		}
	}

	std::shared_ptr<org::services::ShaderStore> SharedShaderStore()
	{
		// Never destroyed. ~ShaderCache runs among the static destructors and still uses the store, and the
		// index is saved as it changes, so nothing is lost at exit. Windows releases the store's mutex.
		static const auto* store = new std::shared_ptr<org::services::ShaderStore>([] {
			org::services::ShaderStoreOptions options;
			options.sourceRoot = L"Data";
			options.cacheDirectory = std::filesystem::path(L"Data/ShaderCache") / kShaderStoreDirName;
			options.persistent = SIE::ShaderCache::Instance().IsDiskCache();
			options.maxBytes = static_cast<uint64_t>(globals::state->shaderStoreMaxMB.load(std::memory_order_relaxed)) << 20;
			const bool persistent = options.persistent;
			auto created = std::make_shared<org::services::ShaderStore>(std::move(options));
			if (!created->OwnsIndex())
				logger::warn("Another game instance owns the shader cache: this one uses it but will not save its index");
			if (persistent) {
				const auto info = created->IndexInfo();
				logger::info("Shader store opened: {} variants, {} artifacts, {} MB, {} evicted", info.variants, info.artifactsAtStart, info.artifactBytesAtStart >> 20, info.artifactsEvicted);
				if (info.overCapBytes)
					logger::warn("Shader store is {} MB over its size limit, because recent starts used all of it. Raise the limit in Advanced settings if you switch between builds or feature sets.", info.overCapBytes >> 20);
				RemoveOldCacheFiles();
			}
			g_openedStore = created.get();
			return created;
		}());
		return *store;
	}

	void BeginShaderStoreGeneration()
	{
		if (auto* store = g_openedStore.load())
			store->BeginGeneration();
	}

	void FinishShaderStoreBatch(std::string_view a_batch)
	{
		const auto store = SharedShaderStore();
		store->Flush();
		const auto info = store->IndexInfo();
		logger::info("Shader store after {}: {} Index: {} variants, {} dependency sets, {} entries, {} files, {} KB saved.", a_batch, store->Summary(), info.variants, info.dependencySets,
			info.dependencyEntries, info.files, info.savedBytes >> 10);
	}
}
