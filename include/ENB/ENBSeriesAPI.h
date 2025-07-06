#pragma once

#include <psapi.h>

#include "AntTweakBar.h"
#include "ENBSeriesSDK.h"

using namespace ENB_SDK;

namespace ENB_API
{
	// Available ENBSeries SDK versions
	enum class SDKVersion : long
	{
		V1000 = 1000,
		V1001 = 1001,
		V1002 = 1002,
	};

	enum class ENBWindowType : int
	{
		TW_HELP,
		EditorBarButtons,  // GeneralWindow
		EditorBar1,        // MainWindow
		EditorBar2,        // Weathers?
		EditorBarObjects,
		EditorBarEffects
	};

	// ENB Series' modder interface
	class ENBAPI
	{
	public:
		/// <summary>
		/// Returns the version of the SDK used by the ENBSeries.<br/>
		/// Guaranteed compatibility for all Xxxx versions only, for example 1025 will work with sdk version 1000-1025, 2025 will work with sdk version 2000-2025, etc.
		/// </summary>
		/// <returns>
		/// Version of SDK used by the ENBSeries, 1000 means 1.0, 1001 means 1.01, etc.
		/// </returns>
		long GetSDKVersion()
		{
			return reinterpret_cast<_ENBGetSDKVersion>(GetProcAddress(enbmodule, "ENBGetSDKVersion"))();
		}

		ENBAPI(HMODULE a_enbmodule)
		{
			this->enbmodule = a_enbmodule;
		}

	protected:
		HMODULE enbmodule = NULL;
	};

	class ENBSDK1000 : public ENBAPI
	{
	public:
		/// <summary>
		/// Returns the version of ENBSeries.
		/// </summary>
		/// <returns>
		/// Version of ENBSeries, 279 means 0.279 etc.
		/// </returns>
		long GetVersion()
		{
			return reinterpret_cast<_ENBGetVersion>(GetProcAddress(enbmodule, "ENBGetVersion"))();
		}

		/// <summary>
		/// Returns the unique ENBSeries game identifier.
		/// </summary>
		/// <returns>
		/// Unique game identifier, for example ENBGAMEID_SKYRIM.
		/// </returns>
		long GetGameIdentifier()
		{
			return reinterpret_cast<_ENBGetGameIdentifier>(GetProcAddress(enbmodule, "ENBGetGameIdentifier"))();
		}

		/// <summary>
		/// Adds a callback function which is executed by ENBSeries at certain moments. This helps to bypass potential bugs and may increase performance.
		/// </summary>
		void SetCallbackFunction(ENBCallbackFunction a_func)
		{
			reinterpret_cast<_ENBSetCallbackFunction>(GetProcAddress(enbmodule, "ENBSetCallbackFunction"))(a_func);
		}

		/// <summary>
		/// Get the value of a parameter.<br/>
		/// Parameters may spawn or be deleted when the user is modifying shaders, so it's highly recommended to call this inside a callback function.</summary>
		/// <param name="a_filename"> Use NULL to access shader variables instead of configuration files.</param>
		/// <param name="a_outparam"> Structure which contains output parameters.</param>
		/// <returns> False if failed because function arguments are invalid, parameter does not exist or is hidden.</returns>
		bool GetParameter(char* a_filename, char* a_category, char* a_keyname, ENBParameter* a_outparam)
		{
			return reinterpret_cast<_ENBGetParameter>(GetProcAddress(enbmodule, "ENBGetParameter"))(a_filename, a_category, a_keyname, a_outparam);
		}
		bool GetParameter(const char* a_filename, const char* a_category, const char* a_keyname, ENBParameter* a_outparam)
		{
			return reinterpret_cast<_ENBGetParameterA>(GetProcAddress(enbmodule, "ENBGetParameter"))(a_filename, a_category, a_keyname, a_outparam);
		}

		/// <summary>
		/// Set the value of a parameter.<br/>
		/// Any values forced by this parameter can be saved by user in editor window, which means corruption of presets,
		/// so it's highly recommended to warn users about that.
		/// </summary>
		/// <param name="a_filename"> Use NULL to access shader variables instead of configuration files.</param>
		/// <returns>
		/// FALSE if failed, because function arguments are invalid, parameter not exist, hidden or read only.<br/>
		/// FALSE if called outside of callback function to protect against graphical artifacts and crashes.
		/// </returns>
		bool SetParameter(char* a_filename, char* a_category, char* a_keyname, ENBParameter* a_inparam)
		{
			return reinterpret_cast<_ENBSetParameter>(GetProcAddress(enbmodule, "ENBSetParameter"))(a_filename, a_category, a_keyname, a_inparam);
		}
		bool SetParameter(const char* a_filename, const char* a_category, const char* a_keyname, ENBParameter* a_inparam)
		{
			return reinterpret_cast<_ENBSetParameterA>(GetProcAddress(enbmodule, "ENBSetParameter"))(a_filename, a_category, a_keyname, a_inparam);
		}
	};

	class ENBSDK1001 : public ENBSDK1000
	{
	public:
		/// <summary>
		/// Receive various objects for advanced programming.
		/// </summary>
		/// <returns>
		/// NULL if called too early and not everything is initialized yet.
		/// </returns>
		ENBRenderInfo* GetRenderInfo()
		{
			return reinterpret_cast<_ENBGetRenderInfo>(GetProcAddress(enbmodule, "ENBGetRenderInfo"))();
		}

		/// <summary>
		/// Receive various states of the mod.
		/// </summary>
		/// <returns>
		/// Returns boolean or indexed values, depending from which state it was requested.
		/// </returns>
		long GetState(ENBStateType state)
		{
			return reinterpret_cast<_ENBStateType>(GetProcAddress(enbmodule, "ENBGetState"))(state);
		}
	};

	class ENBSDKALT1001 : public ENBSDK1001
	{
	public:
		typedef TwBar* (*_TwNewBar)(const char* barName);
		typedef int (*_TwDeleteBar)(TwBar* bar);
		typedef const char* (*_TwGetBarName)(const TwBar* bar);
		typedef TwBar* (*_TwGetBarByIndex)(int barIndex);
		typedef TwBar* (*_TwGetBarByName)(const char* barName);
		typedef int (*_TwRefreshBar)(TwBar* bar);
		typedef const char* (*_TwGetBarName)(const TwBar* bar);

		TwBar* TwNewBar(const char* barName)
		{
			return reinterpret_cast<_TwNewBar>(GetProcAddress(enbmodule, "TwNewBar"))(barName);
		}

		int TwDeleteBar(TwBar* bar)
		{
			return reinterpret_cast<_TwDeleteBar>(GetProcAddress(enbmodule, "TwDeleteBar"))(bar);
		}

		TwBar* TwGetBarByIndex(int barIndex)
		{
			return reinterpret_cast<_TwGetBarByIndex>(GetProcAddress(enbmodule, "TwGetBarByIndex"))(barIndex);
		}

		TwBar* TwGetBarByEnum(ENBWindowType barIndex)
		{
			return reinterpret_cast<_TwGetBarByIndex>(GetProcAddress(enbmodule, "TwGetBarByIndex"))(static_cast<int>(barIndex));
		}

		TwBar* TwGetBarByName(const char* barName)
		{
			return reinterpret_cast<_TwGetBarByName>(GetProcAddress(enbmodule, "TwGetBarByName"))(barName);
		}

		int TwRefreshBar(TwBar* bar)
		{
			return reinterpret_cast<_TwRefreshBar>(GetProcAddress(enbmodule, "TwRefreshBar"))(bar);
		}

		typedef int (*_TwAddVarRW)(TwBar* bar, const char* name, TwType type, void* var, const char* def);
		typedef int (*_TwAddVarRO)(TwBar* bar, const char* name, TwType type, const void* var, const char* def);
		typedef int (*_TwAddVarCB)(TwBar* bar, const char* name, TwType type, TwSetVarCallback setCallback, TwGetVarCallback getCallback, void* clientData, const char* def);
		typedef int (*_TwAddButton)(TwBar* bar, const char* name, TwButtonCallback callback, void* clientData, const char* def);
		typedef int (*_TwAddSeparator)(TwBar* bar, const char* name, const char* def);
		typedef int (*_TwRemoveVar)(TwBar* bar, const char* name);
		typedef int (*_TwRemoveAllVars)(TwBar* bar);

		int TwAddVarRW(TwBar* bar, const char* name, TwType type, void* var, const char* def)
		{
			return reinterpret_cast<_TwAddVarRW>(GetProcAddress(enbmodule, "TwAddVarRW"))(bar, name, type, var, def);
		}

		int TwAddVarRO(TwBar* bar, const char* name, TwType type, const void* var, const char* def)
		{
			return reinterpret_cast<_TwAddVarRO>(GetProcAddress(enbmodule, "TwAddVarRO"))(bar, name, type, var, def);
		}

		int TwAddVarCB(TwBar* bar, const char* name, TwType type, TwSetVarCallback setCallback, TwGetVarCallback getCallback, void* clientData, const char* def)
		{
			return reinterpret_cast<_TwAddVarCB>(GetProcAddress(enbmodule, "TwAddVarCB"))(bar, name, type, setCallback, getCallback, clientData, def);
		}

		int TwAddButton(TwBar* bar, const char* name, TwButtonCallback callback, void* clientData, const char* def)
		{
			return reinterpret_cast<_TwAddButton>(GetProcAddress(enbmodule, "TwAddButton"))(bar, name, callback, clientData, def);
		}

		int TwAddSeparator(TwBar* bar, const char* name, const char* def)
		{
			return reinterpret_cast<_TwAddSeparator>(GetProcAddress(enbmodule, "TwAddVarRO"))(bar, name, def);
		}

		int TwRemoveVar(TwBar* bar, const char* name)
		{
			return reinterpret_cast<_TwRemoveVar>(GetProcAddress(enbmodule, "TwRemoveVar"))(bar, name);
		}

		int TwRemoveAllVars(TwBar* bar)
		{
			return reinterpret_cast<_TwRemoveAllVars>(GetProcAddress(enbmodule, "TwRemoveAllVars"))(bar);
		}

		typedef int (*_TwGetParam)(TwBar* bar, const char* varName, const char* paramName, TwParamValueType paramValueType, unsigned int outValueMaxCount, void* outValues);
		typedef int (*_TwSetParam)(TwBar* bar, const char* varName, const char* paramName, TwParamValueType paramValueType, unsigned int inValueCount, const void* inValues);

		int TwGetParam(TwBar* bar, const char* varName, const char* paramName, TwParamValueType paramValueType, unsigned int outValueMaxCount, void* outValues)
		{
			return reinterpret_cast<_TwGetParam>(GetProcAddress(enbmodule, "TwGetParam"))(bar, varName, paramName, paramValueType, outValueMaxCount, outValues);
		}

		int TwSetParam(TwBar* bar, const char* varName, const char* paramName, TwParamValueType paramValueType, unsigned int inValueCount, const void* inValues)
		{
			return reinterpret_cast<_TwSetParam>(GetProcAddress(enbmodule, "TwSetParam"))(bar, varName, paramName, paramValueType, inValueCount, inValues);
		}

		const char* TwGetBarName(const TwBar* bar)
		{
			return reinterpret_cast<_TwGetBarName>(GetProcAddress(enbmodule, "TwGetBarName"))(bar);
		}

		typedef int (*_TwDefine)(const char* def);

		int TwDefine(const char* def)
		{
			return reinterpret_cast<_TwDefine>(GetProcAddress(enbmodule, "TwDefine"))(def);
		}
	};

	class ENBSDKT1002 : public ENBSDK1001
	{
	};

	class ENBSDKALT1002 : public ENBSDKALT1001
	{
	public:
		struct ENBTimeOfDay
		{
			float dawn;
			float sunrise;
			float day;
			float sunset;
			float dusk;
			float night;
		};

		ENBTimeOfDay GetTimeOfDay()
		{
			return {
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorDawn)),
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorSunrise)),
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorDay)),
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorSunset)),
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorDusk)),
				std::bit_cast<float>(GetState(ENBStateType::ENBState_fTODFactorNight))
			};
		}
	};

	/// <summary>
	/// Request the ENB SDK interface.
	/// Recommended: Send your request during or after SKSEMessagingInterface::kMessage_PostLoad to make sure the dll has already been loaded
	/// </summary>
	/// <param name="a_ENBSDKVersion">The SDK version to request</param>
	/// <returns>The pointer to the API singleton, or nullptr if request failed</returns>
	[[nodiscard]] static inline void* RequestENBAPI(const SDKVersion a_ENBSDKVersion = SDKVersion::V1002)
	{
		DWORD cb = 1000 * sizeof(HMODULE);
		DWORD cbNeeded = 0;
		HMODULE enbmodule = NULL;
		HMODULE hmodules[1000];
		HANDLE hproc = GetCurrentProcess();
		for (long i = 0; i < 1000; i++) hmodules[i] = NULL;
		// Find the proper library using the existance of an exported function, because several with the same name may exist
		if (EnumProcessModules(hproc, hmodules, cb, &cbNeeded)) {
			long count = cbNeeded / sizeof(HMODULE);
			for (long i = 0; i < count; i++) {
				if (hmodules[i] == NULL)
					break;
				void* func = (void*)GetProcAddress(hmodules[i], "ENBGetSDKVersion");
				if (func) {
					enbmodule = hmodules[i];
					break;
				}
			}
		}

		if (!enbmodule)
			return nullptr;

		ENBAPI enbSDK = ENBAPI(enbmodule);

		if (((enbSDK.GetSDKVersion() / 1000) % 10) == (static_cast<long>(a_ENBSDKVersion) / 1000) % 10) {
			return new ENBAPI(enbmodule);
		}
		return nullptr;
	}
}
