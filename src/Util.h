#pragma once

/**
 @def GET_INSTANCE_MEMBER
 @brief Set variable in current namespace based on instance member from GetRuntimeData or GetVRRuntimeData.
  
 @warning The class must have both a GetRuntimeData() and GetVRRuntimeData() function.
  
 @param a_value The instance member value to access (e.g., renderTargets).
 @param a_source The instance of the class (e.g., state).
 @result The a_value will be set as a variable in the current namespace. (e.g., auto& renderTargets = state->renderTargets;)
 */
#define GET_INSTANCE_MEMBER(a_value, a_source) \
	auto& a_value = !REL::Module::IsVR() ? a_source->GetRuntimeData().a_value : a_source->GetVRRuntimeData().a_value;

namespace Util
{
	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source);
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);
	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");
	std::string DefinesToString(const std::vector<std::pair<const char*, const char*>>& defines);
	std::string DefinesToString(const std::vector<D3D_SHADER_MACRO>& defines);
	/**
	 * @brief Normalizes a file path by replacing backslashes with forward slashes, 
	 *        removing redundant slashes, and converting all characters to lowercase.
	 * 
	 * This function ensures that the file path uses consistent forward slashes
	 * (`/`), eliminates consecutive slashes (`//`), and converts all characters 
	 * in the path to lowercase for case-insensitive comparisons.
	 * 
	 * @param a_path The original file path to be normalized.
	 * @return A normalized file path as a lowercase string with single forward slashes.
	 */
	std::string FixFilePath(const std::string& a_path);
	std::string WStringToString(const std::wstring& wideString);

	float4 TryGetWaterData(float offsetX, float offsetY);
	/**
	 * @brief Dumps the names of all settings from the specified setting collections and the game setting collection.
	 * 
	 * This function retrieves settings from two INI setting collections (`INISettingCollection` and `INIPrefSettingCollection`)
	 * and logs their names. It also retrieves and logs settings from the game setting collection (`GameSettingCollection`).
	 * 
	 * The output is logged using the `logger::info` method, and it includes the collection name for better clarity on where
	 * each setting belongs.
	 */
	void DumpSettingsOptions();
	float4 GetCameraData();
	bool GetTemporal();
	float GetVerticalFOVRad();

	inline RE::NiPoint3 GetAverageEyePosition()
	{
		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (!REL::Module::IsVR())
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return (shadowState->GetVRRuntimeData().posAdjust.getEye(0) + shadowState->GetVRRuntimeData().posAdjust.getEye(1)) * 0.5f;
	}

	inline RE::NiPoint3 GetEyePosition(int eyeIndex)
	{
		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (!REL::Module::IsVR())
			return shadowState->GetRuntimeData().posAdjust.getEye();
		return shadowState->GetVRRuntimeData().posAdjust.getEye(eyeIndex);
	}

	inline RE::BSGraphics::ViewData GetCameraData(int eyeIndex)
	{
		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (!REL::Module::IsVR()) {
			return shadowState->GetRuntimeData().cameraData.getEye();
		}
		return shadowState->GetVRRuntimeData().cameraData.getEye(eyeIndex);
	}

	float2 ConvertToDynamic(float2 a_size);

	struct DispatchCount
	{
		uint x;
		uint y;
	};

	DispatchCount GetScreenDispatchCount(bool a_dynamic = true);

	/**
	 * Usage:
	 * if (auto _tt = Util::HoverTooltipWrapper()){
	 *     ImGui::Text("What the tooltip says.");
	 * }
	*/
	class HoverTooltipWrapper
	{
	private:
		bool hovered;

	public:
		HoverTooltipWrapper();
		~HoverTooltipWrapper();
		inline operator bool() { return hovered; }
	};

	/**
	 * Usage:
	 * static FrameChecker frame_checker;
	 * if(frame_checker.isNewFrame())
	 *     ...
	*/
	class FrameChecker
	{
	private:
		uint32_t last_frame = UINT32_MAX;

	public:
		inline bool isNewFrame(uint32_t frame)
		{
			bool retval = last_frame != frame;
			last_frame = frame;
			return retval;
		}
		inline bool isNewFrame() { return isNewFrame(RE::BSGraphics::State::GetSingleton()->frameCount); }
	};

	/**
	 * @brief Checks if dynamic resolution is currently enabled.
	 * 
	 * @return true if dynamic resolution is enabled, false otherwise.
	 */
	bool IsDynamicResolution();

}

namespace nlohmann
{
	void to_json(json& j, const float2& v);
	void from_json(const json& j, float2& v);
	void to_json(json& j, const float3& v);
	void from_json(const json& j, float3& v);
	void to_json(json& j, const float4& v);
	void from_json(const json& j, float4& v);
};