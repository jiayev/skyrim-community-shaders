#pragma once

#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <variant>
#include <vector>

/// Value types that can be overridden by the PostProcessController.
/// Covers all common parameter types used in PostProcessFeature settings.
using PPOverrideValue = std::variant<bool, int, float, float2, float3, float4>;

/// @brief Describes a single controllable parameter within a PostProcessFeature.
///
/// Created by PostProcessFeature subclasses during RegisterControllableParams()
/// and stored in the controller's registry. Holds a direct pointer to the setting
/// field so the controller can read/write it without virtual dispatch per-param.
struct PPParamDesc
{
	/// The type of the underlying field (must match the variant alternative stored).
	enum class Type
	{
		Bool,
		Int,
		Float,
		Float2,
		Float3,
		Float4,
	};

	std::string featureType;   ///< Owning feature's GetType() string, e.g. "COD Bloom"
	std::string name;          ///< Internal name used as key, e.g. "Threshold"
	std::string displayName;   ///< UI-friendly name, e.g. "Bloom Threshold"
	std::string tooltip;       ///< Description for UI/docs
	Type type;                 ///< The data type of the parameter
	void* valuePtr = nullptr;  ///< Direct pointer to the setting field in the feature

	/// Default value (used for reset / as baseline when no override is active)
	PPOverrideValue defaultValue;

	/// Optional min/max range (meaningful for float types)
	std::optional<float> minValue;
	std::optional<float> maxValue;
};

/// @brief Priority-based controller for PostProcessFeature parameters.
///
/// Two responsibilities:
/// 1. **Registry**: PostProcessFeatures register their controllable parameters
///    (with type, pointer, name, range). External callers can enumerate them.
/// 2. **Override stack**: Multiple sources set overrides with priorities.
///    Before Draw(), the controller writes the winning override into the field.
///    After Draw(), it restores the original value.
///
/// Usage:
///   auto* ctrl = PostProcessController::GetSingleton();
///
///   // Enumerate all registered params for a feature
///   for (auto& desc : ctrl->GetRegisteredParams("COD Bloom"))
///       logger::info("Param: {} ({})", desc.name, desc.displayName);
///
///   // Set an override
///   ctrl->SetOverride("COD Bloom", "Threshold", "WeatherManager",
///       PostProcessController::kWeather, -4.0f);
///
///   // Higher priority wins
///   ctrl->SetOverride("COD Bloom", "Threshold", "IBL",
///       PostProcessController::kFeature, -2.0f);
///
///   // Apply before Draw, revert after
///   ctrl->ApplyOverrides("COD Bloom");
///   pipe->Draw(lastTexColor);
///   ctrl->RevertOverrides("COD Bloom");
struct PostProcessController
{
	static PostProcessController* GetSingleton()
	{
		static PostProcessController singleton;
		return &singleton;
	}

	/// Predefined priority levels. Users may use any integer value;
	/// these are convenient defaults. Higher value = higher priority.
	enum Priority : int
	{
		kWeather = 100,   ///< Weather/TOD system
		kFeature = 200,   ///< Other CS features controlling PP parameters
		kExternal = 300,  ///< External mods/APIs
		kDebug = 1000,    ///< Debug overrides (always win)
	};

	// =========================================================================
	// Parameter Registration
	// =========================================================================

	/// @brief Register a controllable parameter.
	/// Called by PostProcessFeature subclasses from RegisterControllableParams().
	void RegisterParam(const PPParamDesc& desc);

	/// @brief Remove all registered parameters for a feature (e.g. on feature teardown).
	void UnregisterFeature(const std::string& featureType);

	/// @brief Get all registered parameter descriptors for a feature.
	/// Returns empty vector if no params registered for this feature.
	std::vector<const PPParamDesc*> GetRegisteredParams(const std::string& featureType) const;

	/// @brief Get all registered parameter descriptors across all features.
	std::vector<const PPParamDesc*> GetAllRegisteredParams() const;

	/// @brief Find a specific registered parameter descriptor.
	/// @return Pointer to the descriptor, or nullptr if not found.
	const PPParamDesc* FindParam(const std::string& featureType, const std::string& paramName) const;

	// =========================================================================
	// Override Management
	// =========================================================================

	/// An individual override entry from a specific source.
	struct Override
	{
		std::string source;     ///< Identifier of the source, e.g. "WeatherManager", "IBL"
		int priority = 0;       ///< Higher priority wins
		PPOverrideValue value;  ///< The override value
	};

	/// @brief Set an override for a registered parameter.
	///
	/// The parameter must have been registered via RegisterParam() first.
	/// If the parameter is not registered, the override is silently ignored.
	/// If this source already has an override for this parameter, it is replaced.
	///
	/// @param featureType  The PostProcessFeature::GetType() string, e.g. "COD Bloom"
	/// @param paramName    The parameter name, e.g. "Threshold"
	/// @param source       Identifier of the control source, e.g. "WeatherManager"
	/// @param priority     Priority level (higher wins)
	/// @param value        The value to override with
	void SetOverride(const std::string& featureType,
		const std::string& paramName,
		const std::string& source,
		int priority,
		const PPOverrideValue& value);

	/// @brief Remove a specific source's override for a specific parameter.
	void RemoveOverride(const std::string& featureType,
		const std::string& paramName,
		const std::string& source);

	/// @brief Remove all overrides from a given source across all features and parameters.
	void RemoveAllFromSource(const std::string& source);

	/// @brief Get the winning (highest priority) override for a single parameter.
	std::optional<PPOverrideValue> GetActiveOverride(
		const std::string& featureType,
		const std::string& paramName) const;

	/// @brief Check if any overrides are active for a given feature type.
	bool HasOverridesForFeature(const std::string& featureType) const;

	// =========================================================================
	// Apply / Revert (called by PostProcessing around each feature's Draw)
	// =========================================================================

	/// @brief Apply all active overrides for a feature by writing winning values
	///        into the registered parameter pointers. Saves original values for revert.
	///        Call this before the feature's Draw().
	void ApplyOverrides(const std::string& featureType);

	/// @brief Revert all overridden parameters for a feature to their original values.
	///        Call this after the feature's Draw().
	void RevertOverrides(const std::string& featureType);

	/// @brief Clear all overrides across all features and sources.
	void ClearAll();

private:
	PostProcessController() = default;
	~PostProcessController() = default;
	PostProcessController(const PostProcessController&) = delete;
	PostProcessController& operator=(const PostProcessController&) = delete;

	/// Compound key identifying a specific parameter on a specific feature.
	struct ParamKey
	{
		std::string featureType;
		std::string paramName;

		bool operator<(const ParamKey& other) const
		{
			if (featureType != other.featureType)
				return featureType < other.featureType;
			return paramName < other.paramName;
		}

		bool operator==(const ParamKey& other) const
		{
			return featureType == other.featureType && paramName == other.paramName;
		}
	};

	/// Stack of overrides for a single parameter, from multiple sources.
	/// Kept sorted by priority descending so Top() is O(1).
	struct OverrideStack
	{
		std::vector<Override> entries;

		void Add(const Override& o);
		void Remove(const std::string& source);
		bool Empty() const { return entries.empty(); }
		const Override& Top() const { return entries.front(); }

	private:
		void Sort();
	};

	/// Saved original value for revert after Draw().
	struct SavedValue
	{
		void* ptr = nullptr;
		PPParamDesc::Type type;
		PPOverrideValue original;
	};

	// --- Data ---

	/// All registered parameter descriptors, keyed by (featureType, paramName).
	std::map<ParamKey, PPParamDesc> registry;

	/// Active override stacks, keyed by (featureType, paramName).
	std::map<ParamKey, OverrideStack> overrides;

	/// Saved values for the current apply/revert cycle, per feature.
	std::map<std::string, std::vector<SavedValue>> savedValues;

	mutable std::shared_mutex mutex;

	// --- Internal helpers ---

	/// Read the current value from a param's pointer into a PPOverrideValue.
	static PPOverrideValue ReadFromPtr(void* ptr, PPParamDesc::Type type);

	/// Write a PPOverrideValue into a param's pointer.
	static void WriteToPtr(void* ptr, PPParamDesc::Type type, const PPOverrideValue& value);
};
