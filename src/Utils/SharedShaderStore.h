#pragma once

#include <memory>
#include <string_view>

namespace org::services
{
	class ShaderStore;
}

/// The plugin's one shader store. FXC and DXC builds share it, and a cache folder can only have one owner.
namespace Util
{
	/// Subdirectory of Data/ShaderCache holding the store.
	inline constexpr std::wstring_view kShaderStoreDirName = L"v2";

	/// The store, opened on first use. Settings must be loaded by then: the disk cache setting decides whether it saves anything.
	std::shared_ptr<org::services::ShaderStore> SharedShaderStore();

	/// Source files may have changed, so the store must read them again. Does nothing before the store is opened, so this
	/// is safe at any time: ShaderCache::Clear also runs at exit, when it is too late to open the store.
	void BeginShaderStoreGeneration();

	/// Call when a batch of builds ends: saves the index now and logs what the batch cost.
	void FinishShaderStoreBatch(std::string_view a_batch);
}
