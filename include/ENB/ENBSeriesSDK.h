// ENBSeries SDK http://enbdev.com/
// Original author: Boris Vorontsov, 2017
// Modified by @doodlum
// Main header file with parameters and functions definitions
#pragma once

namespace ENB_SDK
{
	enum class ENBParameterType : long
	{
		ENBParam_NONE = 0,                //invalid
		ENBParam_FLOAT = 1,               //1 float
		ENBParam_INT = 2,                 //1 int
		ENBParam_HEX = 3,                 //1 DWORD
		ENBParam_BOOL = 4,                //1 BOOL
		ENBParam_COLOR3 = 5,              //3 float
		ENBParam_COLOR4 = 6,              //4 float
		ENBParam_VECTOR3 = 7,             //3 float
		ENBParam_FORCEDWORD = 0x7fffffff  //unused
	};

	inline long ENBParameterTypeToSize(ENBParameterType type)
	{
		long size = 0;
		if (type == ENBParameterType::ENBParam_FLOAT)
			size = sizeof(float);
		if (type == ENBParameterType::ENBParam_INT)
			size = sizeof(int);
		if (type == ENBParameterType::ENBParam_HEX)
			size = sizeof(DWORD);
		if (type == ENBParameterType::ENBParam_BOOL)
			size = sizeof(BOOL);
		if (type == ENBParameterType::ENBParam_COLOR3)
			size = sizeof(float) * 3;
		if (type == ENBParameterType::ENBParam_COLOR4)
			size = sizeof(float) * 4;
		if (type == ENBParameterType::ENBParam_VECTOR3)
			size = sizeof(float) * 3;
		return size;
	}

	struct ENBParameter
	{
		unsigned char Data[16];  //array of variables BOOL, INT, FLOAT, max vector of 4 elements
		unsigned long Size;      //4*4 max
		ENBParameterType Type;

		ENBParameter()
		{
			for (DWORD k = 0; k < 16; k++) Data[k] = 0;
			Size = 0;
			Type = ENBParameterType::ENBParam_NONE;
		}
	};

	enum class ENBCallbackType : long
	{
		ENBCallback_EndFrame = 1,    //called at the end of frame, before displaying result on the screen
		ENBCallback_BeginFrame = 2,  //called after frame was displayed, time between end and begin frame may be big enough to execute something heavy in separate thread
		ENBCallback_PreSave = 3,     //called before user trying to save config, useful for restoring original parameters
		ENBCallback_PostLoad = 4,    //called when parameters are created and loaded, useful for saving original parameters
		//v1001:
		ENBCallback_OnInit = 5,     //called when mod is initialized completely, but nothing yet processed, all new resources must be created by plugin
		ENBCallback_OnExit = 6,     //called when game or mod are about to close, all new resources must be deleted by plugin
		ENBCallback_PreReset = 7,   //called when game or mod destroy internal objects (display mode changes for example), need to destroy all objects in plugin. may not be called ever, but must be handled in some similar way to OnExit
		ENBCallback_PostReset = 8,  //called when game or mod re-create internal objects (after display mode changes for example), need to create all objects in plugin again, including pointers to interfaces of d3d. may not be called ever, but must be handled in some similar way to OnInit

		ENBCallback_FORCEDWORD = 0x7fffffff  //unused
	};

	//v1001:
	enum ENBStateType : long
	{
		ENBState_IsEditorActive = 1,      //is mod editor windows are opened
		ENBState_IsEffectsWndActive = 2,  //is shader effects window of mod editor opened
		ENBState_CursorPosX = 3,          //position of editor cursor (may not be the same as game cursor)
		ENBState_CursorPosY = 4,          //position of editor cursor (may not be the same as game cursor)
		ENBState_MouseLeft = 5,           //mouse key state boolean pressed or not
		ENBState_MouseRight = 6,          //mouse key state boolean pressed or not
		ENBState_MouseMiddle = 7,         //mouse key state boolean pressed or not

		//v1002:
		ENBState_ulWeatherCurrent = 8,          //current weather index as unsigned long
		ENBState_ulWeatherOutgoing = 9,         //outgoing weather index as unsigned long
		ENBState_fWeatherTransition = 10,       //weather transition as float
		ENBState_fTimeOfDay = 11,               //time of the day 24 hours format as float
		ENBState_fTODFactorDawn = 12,           //time weight 0..1 range
		ENBState_fTODFactorSunrise = 13,        //time weight 0..1 range
		ENBState_fTODFactorDay = 14,            //time weight 0..1 range
		ENBState_fTODFactorSunset = 15,         //time weight 0..1 range
		ENBState_fTODFactorDusk = 16,           //time weight 0..1 range
		ENBState_fTODFactorNight = 17,          //time weight 0..1 range
		ENBState_fTODFactorInteriorDay = 18,    //time weight 0..1 range
		ENBState_fTODFactorInteriorNight = 19,  //time weight 0..1 range
		ENBState_fNightDayFactor = 20,          //time weight 0..1 range
		ENBState_fInteriorFactor = 21,          //time weight 0..1 range
		ENBState_ulWorldSpaceID = 22,           //as unsigned long
		ENBState_ulLocationID = 23,             //as unsigned long

		ENBState_FORCEDWORD = 0x7fffffff  //unused
	};

	//v1001:
	struct ENBRenderInfo
	{
		//these objects actually pointers to ENBSeries wrapped classes to make sure every change is tracked by mod and restored later
		void* d3d11device;         //ID3D11Device
		void* d3d11devicecontext;  //ID3D11DeviceContext
		void* dxgiswapchain;       //IDXGISwapChain

		DWORD ScreenSizeX = NULL;
		DWORD ScreenSizeY = NULL;

		ENBRenderInfo()
		{
			d3d11device = NULL;
			d3d11devicecontext = NULL;
			dxgiswapchain = NULL;
			ScreenSizeX = 0;
			ScreenSizeX = 0;
		}
	};

	//Returns version of SDK used by the ENBSeries, 1000 means 1.0, 1001 means 1.01, etc
	//Guaranteed compatibility for all Xxxx versions only, for example 1025 will work with sdk version 1000-1025,
	//2025 will work with sdk version 2000-2025, etc. In best cases it's equal to ENBSDKVERSION
	typedef long (*_ENBGetSDKVersion)();

	//Returns version of the ENBSeries, 279 means 0.279
	typedef long (*_ENBGetVersion)();

	//Returns unique game identifier, for example ENBGAMEID_SKYRIM
	typedef long (*_ENBGetGameIdentifier)();

	//Assign callback function which is executed by ENBSeries at certain moments. This helps to bypass potential bugs
	//and increase performance. Argument calltype must be used to select proper action.
	//void WINAPI	CallbackFunction(ENBCallbackType calltype); //example function
	typedef void(WINAPI* ENBCallbackFunction)(ENBCallbackType calltype);  //declaration of callback function
	typedef void (*_ENBSetCallbackFunction)(ENBCallbackFunction func);

	//Receive value of parameter
	//Input "filename" could be NULL to access shader variables instead of configuration files.
	//Return FALSE if failed, because function arguments are invalid, parameter not exist or hidden. Also parameters
	//may spawn or to be deleted when user modifying shaders, so highly recommended to call it inside callback function.
	//For shader variables set filename=NULL
	typedef bool (*_ENBGetParameter)(char* filename, char* category, char* keyname, ENBParameter* outparam);
	typedef bool (*_ENBGetParameterA)(const char* filename, const char* category, const char* keyname, ENBParameter* outparam);

	//Set value of parameter
	//Input "filename" could be NULL to access shader variables instead of configuration files
	//Returns FALSE if failed, because function arguments are invalid, parameter not exist, hidden or read only. Also parameters
	//may spawn or to be deleted when user modifying shaders.
	//Return FALSE if called outside of callback function to protect against graphical artifacts and crashes.
	//WARNING! Any values forced by this parameter can be saved by user in editor window, which means corruption of presets,
	//so it's highly recommended to warn users about that.
	//For shader variables set filename=NULL
	typedef bool (*_ENBSetParameter)(char* filename, char* category, char* keyname, ENBParameter* inparam);
	typedef bool (*_ENBSetParameterA)(const char* filename, const char* category, const char* keyname, ENBParameter* inparam);

	//v1001:
	//Receive various objects for advanced programming
	//Return NULL if called too early and not everything initialized yet
	typedef ENBRenderInfo* (*_ENBGetRenderInfo)();

	//Receive various states of the mod
	//Return boolean or indexed values, depending from which state requested
	typedef long (*_ENBStateType)(ENBStateType state);

	//Returns version of SDK used by the ENBSeries, 1000 means 1.0, 1001 means 1.01, etc
	//Guaranteed compatibility for all Xxxx versions only, for example 1025 will work with sdk version 1000-1025,
	//2025 will work with sdk version 2000-2025, etc. In best cases it's equal to ENBSDKVERSION
	typedef long (*_ENBGetSDKVersion)();
}
