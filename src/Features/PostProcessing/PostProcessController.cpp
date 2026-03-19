#include "PostProcessController.h"

// =============================================================================
// OverrideStack
// =============================================================================

void PostProcessController::OverrideStack::Sort()
{
	std::sort(entries.begin(), entries.end(),
		[](const Override& a, const Override& b) {
			return a.priority > b.priority;
		});
}

void PostProcessController::OverrideStack::Add(const Override& o)
{
	Remove(o.source);
	entries.push_back(o);
	Sort();
}

void PostProcessController::OverrideStack::Remove(const std::string& source)
{
	entries.erase(
		std::remove_if(entries.begin(), entries.end(),
			[&source](const Override& entry) { return entry.source == source; }),
		entries.end());
}

// =============================================================================
// ReadFromPtr / WriteToPtr
// =============================================================================

PPOverrideValue PostProcessController::ReadFromPtr(void* ptr, PPParamDesc::Type type)
{
	switch (type) {
	case PPParamDesc::Type::Bool:
		return *static_cast<bool*>(ptr);
	case PPParamDesc::Type::Int:
		return *static_cast<int*>(ptr);
	case PPParamDesc::Type::Float:
		return *static_cast<float*>(ptr);
	case PPParamDesc::Type::Float2:
		return *static_cast<float2*>(ptr);
	case PPParamDesc::Type::Float3:
		return *static_cast<float3*>(ptr);
	case PPParamDesc::Type::Float4:
		return *static_cast<float4*>(ptr);
	default:
		return false;
	}
}

void PostProcessController::WriteToPtr(void* ptr, PPParamDesc::Type type, const PPOverrideValue& value)
{
	switch (type) {
	case PPParamDesc::Type::Bool:
		if (auto* v = std::get_if<bool>(&value))
			*static_cast<bool*>(ptr) = *v;
		break;
	case PPParamDesc::Type::Int:
		if (auto* v = std::get_if<int>(&value))
			*static_cast<int*>(ptr) = *v;
		break;
	case PPParamDesc::Type::Float:
		if (auto* v = std::get_if<float>(&value))
			*static_cast<float*>(ptr) = *v;
		break;
	case PPParamDesc::Type::Float2:
		if (auto* v = std::get_if<float2>(&value))
			*static_cast<float2*>(ptr) = *v;
		break;
	case PPParamDesc::Type::Float3:
		if (auto* v = std::get_if<float3>(&value))
			*static_cast<float3*>(ptr) = *v;
		break;
	case PPParamDesc::Type::Float4:
		if (auto* v = std::get_if<float4>(&value))
			*static_cast<float4*>(ptr) = *v;
		break;
	}
}

// =============================================================================
// Parameter Registration
// =============================================================================

void PostProcessController::RegisterParam(const PPParamDesc& desc)
{
	std::unique_lock lock(mutex);
	ParamKey key{ desc.featureType, desc.name };
	registry[key] = desc;
}

void PostProcessController::UnregisterFeature(const std::string& featureType)
{
	std::unique_lock lock(mutex);

	for (auto it = registry.begin(); it != registry.end();) {
		if (it->first.featureType == featureType)
			it = registry.erase(it);
		else
			++it;
	}

	// Also remove any overrides for this feature
	for (auto it = overrides.begin(); it != overrides.end();) {
		if (it->first.featureType == featureType)
			it = overrides.erase(it);
		else
			++it;
	}

	savedValues.erase(featureType);
}

std::vector<const PPParamDesc*> PostProcessController::GetRegisteredParams(const std::string& featureType) const
{
	std::shared_lock lock(mutex);

	std::vector<const PPParamDesc*> result;
	for (const auto& [key, desc] : registry) {
		if (key.featureType == featureType)
			result.push_back(&desc);
	}
	return result;
}

std::vector<const PPParamDesc*> PostProcessController::GetAllRegisteredParams() const
{
	std::shared_lock lock(mutex);

	std::vector<const PPParamDesc*> result;
	result.reserve(registry.size());
	for (const auto& [key, desc] : registry)
		result.push_back(&desc);
	return result;
}

const PPParamDesc* PostProcessController::FindParam(const std::string& featureType, const std::string& paramName) const
{
	std::shared_lock lock(mutex);

	ParamKey key{ featureType, paramName };
	auto it = registry.find(key);
	return it != registry.end() ? &it->second : nullptr;
}

// =============================================================================
// Override Management
// =============================================================================

void PostProcessController::SetOverride(
	const std::string& featureType,
	const std::string& paramName,
	const std::string& source,
	int priority,
	const PPOverrideValue& value)
{
	std::unique_lock lock(mutex);

	ParamKey key{ featureType, paramName };

	// Only allow overrides on registered parameters
	if (registry.find(key) == registry.end()) {
		logger::warn("PostProcessController::SetOverride - parameter '{}.{}' is not registered, ignoring",
			featureType, paramName);
		return;
	}

	Override o{ source, priority, value };
	overrides[key].Add(o);
}

void PostProcessController::RemoveOverride(
	const std::string& featureType,
	const std::string& paramName,
	const std::string& source)
{
	std::unique_lock lock(mutex);

	ParamKey key{ featureType, paramName };
	auto it = overrides.find(key);
	if (it != overrides.end()) {
		it->second.Remove(source);
		if (it->second.Empty())
			overrides.erase(it);
	}
}

void PostProcessController::RemoveAllFromSource(const std::string& source)
{
	std::unique_lock lock(mutex);

	for (auto it = overrides.begin(); it != overrides.end();) {
		it->second.Remove(source);
		if (it->second.Empty())
			it = overrides.erase(it);
		else
			++it;
	}
}

std::optional<PPOverrideValue> PostProcessController::GetActiveOverride(
	const std::string& featureType,
	const std::string& paramName) const
{
	std::shared_lock lock(mutex);

	ParamKey key{ featureType, paramName };
	auto it = overrides.find(key);
	if (it != overrides.end() && !it->second.Empty())
		return it->second.Top().value;
	return std::nullopt;
}

bool PostProcessController::HasOverridesForFeature(const std::string& featureType) const
{
	std::shared_lock lock(mutex);

	for (const auto& [key, stack] : overrides) {
		if (key.featureType == featureType && !stack.Empty())
			return true;
	}
	return false;
}

// =============================================================================
// Apply / Revert
// =============================================================================

void PostProcessController::ApplyOverrides(const std::string& featureType)
{
	std::unique_lock lock(mutex);

	auto& saved = savedValues[featureType];
	saved.clear();

	for (const auto& [key, stack] : overrides) {
		if (key.featureType != featureType || stack.Empty())
			continue;

		// Find the registered descriptor to get the pointer and type
		auto regIt = registry.find(key);
		if (regIt == registry.end() || !regIt->second.valuePtr)
			continue;

		const auto& desc = regIt->second;

		// Save original value
		SavedValue sv;
		sv.ptr = desc.valuePtr;
		sv.type = desc.type;
		sv.original = ReadFromPtr(desc.valuePtr, desc.type);
		saved.push_back(sv);

		// Write the winning override
		WriteToPtr(desc.valuePtr, desc.type, stack.Top().value);
	}
}

void PostProcessController::RevertOverrides(const std::string& featureType)
{
	std::unique_lock lock(mutex);

	auto it = savedValues.find(featureType);
	if (it == savedValues.end())
		return;

	for (const auto& sv : it->second) {
		if (sv.ptr)
			WriteToPtr(sv.ptr, sv.type, sv.original);
	}

	it->second.clear();
}

void PostProcessController::ClearAll()
{
	std::unique_lock lock(mutex);
	overrides.clear();
	savedValues.clear();
}
