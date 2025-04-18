// ----------------------------------------------------------------------------
//
//	@file		AntTweakBar.h
//
//	@brief		AntTweakBar is a light and intuitive graphical user interface
//				that can be readily integrated into OpenGL and DirectX
//				applications in order to interactively tweak parameters.
//
//	@author		Philippe Decaudin
//
//	@doc		http://anttweakbar.sourceforge.net/doc
//
//	@license	This file is part of the AntTweakBar library.
//				AntTweakBar is a free software released under the zlib license.
//				For conditions of distribution and use, see License.txt
//
// ----------------------------------------------------------------------------
#pragma once

#if !defined TW_INCLUDED
#	define TW_INCLUDED

#	include <stddef.h>

#	define TW_VERSION 117  // Version Mmm : M=Major mm=minor (e.g., 102 is version 1.02)

#	ifdef __cplusplus
#		if defined(_MSC_VER)
#			pragma warning(push)
#			pragma warning(disable: 4995 4530)
#			include <string>
#			pragma warning(pop)
#		else
#			include <string>
#		endif
extern "C" {
#	endif  // __cplusplus

	// ----------------------------------------------------------------------------
	//	OS specific definitions
	// ----------------------------------------------------------------------------

#	if (defined(_WIN32) || defined(_WIN64)) && !defined(TW_STATIC)
#		define TW_CALL __stdcall
#		define TW_CDECL_CALL __cdecl
#		define TW_EXPORT_API __declspec(dllexport)
#		define TW_IMPORT_API __declspec(dllimport)
#	else
#		define TW_CALL
#		define TW_CDECL_CALL
#		define TW_EXPORT_API
#		define TW_IMPORT_API
#	endif

#	define TW_API

	// ----------------------------------------------------------------------------
	//	Bar functions and definitions
	// ----------------------------------------------------------------------------

	typedef struct CTwBar TwBar;  // structure CTwBar is not exposed.

	//TW_API TwBar *		TW_CALL TwNewBar(const char *barName);
	//TW_API int			TW_CALL TwDeleteBar(TwBar *bar);
	//TW_API int			TW_CALL TwDeleteAllBars();
	//TW_API int			TW_CALL TwSetTopBar(const TwBar *bar);
	//TW_API TwBar *		TW_CALL TwGetTopBar();
	//TW_API int			TW_CALL TwSetBottomBar(const TwBar *bar);
	//TW_API TwBar *		TW_CALL TwGetBottomBar();
	//TW_API const char * TW_CALL TwGetBarName(const TwBar *bar);
	//TW_API int			TW_CALL TwGetBarCount();
	//TW_API TwBar *		TW_CALL TwGetBarByIndex(int barIndex);
	//TW_API TwBar *		TW_CALL TwGetBarByName(const char *barName);
	//TW_API int			TW_CALL TwRefreshBar(TwBar *bar);
	//TW_API TwBar *		TW_CALL TwGetActiveBar();

	// ----------------------------------------------------------------------------
	//	Var functions and definitions
	// ----------------------------------------------------------------------------

	typedef enum ETwType
	{
		TW_TYPE_UNDEF = 0,
#	ifdef __cplusplus
		TW_TYPE_BOOLCPP = 1,
#	endif  // __cplusplus
		TW_TYPE_BOOL8 = 2,
		TW_TYPE_BOOL16,
		TW_TYPE_BOOL32,
		TW_TYPE_CHAR,
		TW_TYPE_INT8,
		TW_TYPE_UINT8,
		TW_TYPE_INT16,
		TW_TYPE_UINT16,
		TW_TYPE_INT32,
		TW_TYPE_UINT32,
		TW_TYPE_FLOAT,
		TW_TYPE_DOUBLE,
		TW_TYPE_COLOR32,   // 32 bits color. Order is RGBA if API is OpenGL or Direct3D10, and inversed if API is Direct3D9 (can be modified by defining 'colorOrder=...', see doc)
		TW_TYPE_COLOR3F,   // 3 floats color. Order is RGB.
		TW_TYPE_COLOR4F,   // 4 floats color. Order is RGBA.
		TW_TYPE_CDSTRING,  // Null-terminated C Dynamic String (pointer to an array of char dynamically allocated with malloc/realloc/strdup)
#	ifdef __cplusplus
#		if defined(_MSC_VER) && (_MSC_VER == 1600)
		TW_TYPE_STDSTRING = (0x2ffe0000 + sizeof(std::string)),  // VS2010 C++ STL string (std::string)
#		else
		TW_TYPE_STDSTRING = (0x2fff0000 + sizeof(std::string)),  // C++ STL string (std::string)
#		endif
#	endif                                      // __cplusplus
		TW_TYPE_QUAT4F = TW_TYPE_CDSTRING + 2,  // 4 floats encoding a quaternion {qx,qy,qz,qs}
		TW_TYPE_QUAT4D,                         // 4 doubles encoding a quaternion {qx,qy,qz,qs}
		TW_TYPE_DIR3F,                          // direction vector represented by 3 floats
		TW_TYPE_DIR3D                           // direction vector represented by 3 doubles
	} TwType;
#	define TW_TYPE_CSSTRING(n) ((TwType)(0x30000000 + ((n)&0xfffffff)))  // Null-terminated C Static String of size n (defined as char[n], with n<2^28)

	typedef void(TW_CALL* TwSetVarCallback)(const void* value, void* clientData);
	typedef void(TW_CALL* TwGetVarCallback)(void* value, void* clientData);
	typedef void(TW_CALL* TwButtonCallback)(void* clientData);

	//TW_API int		TW_CALL TwAddVarRW(TwBar *bar, const char *name, TwType type, void *var, const char *def);
	//TW_API int		TW_CALL TwAddVarRO(TwBar *bar, const char *name, TwType type, const void *var, const char *def);
	//TW_API int		TW_CALL TwAddVarCB(TwBar *bar, const char *name, TwType type, TwSetVarCallback setCallback, TwGetVarCallback getCallback, void *clientData, const char *def);
	//TW_API int		TW_CALL TwAddButton(TwBar *bar, const char *name, TwButtonCallback callback, void *clientData, const char *def);
	//TW_API int		TW_CALL TwAddSeparator(TwBar *bar, const char *name, const char *def);
	//TW_API int		TW_CALL TwRemoveVar(TwBar *bar, const char *name);
	//TW_API int		TW_CALL TwRemoveAllVars(TwBar *bar);

	typedef struct CTwEnumVal
	{
		int Value;
		const char* Label;
	} TwEnumVal;
	typedef struct CTwStructMember
	{
		const char* Name;
		TwType Type;
		size_t Offset;
		const char* DefString;
	} TwStructMember;
	typedef void(TW_CALL* TwSummaryCallback)(char* summaryString, size_t summaryMaxLength, const void* value, void* clientData);

	//TW_API int		TW_CALL TwDefine(const char *def);
	//TW_API TwType	TW_CALL TwDefineEnum(const char *name, const TwEnumVal *enumValues, unsigned int nbValues);
	//TW_API TwType	TW_CALL TwDefineEnumFromString(const char *name, const char *enumString);
	//TW_API TwType	TW_CALL TwDefineStruct(const char *name, const TwStructMember *structMembers, unsigned int nbMembers, size_t structSize, TwSummaryCallback summaryCallback, void *summaryClientData);

	typedef void(TW_CALL* TwCopyCDStringToClient)(char** destinationClientStringPtr, const char* sourceString);
//TW_API void		TW_CALL TwCopyCDStringToClientFunc(TwCopyCDStringToClient copyCDStringFunc);
//TW_API void		TW_CALL TwCopyCDStringToLibrary(char **destinationLibraryStringPtr, const char *sourceClientString);
#	ifdef __cplusplus
	typedef void(TW_CALL* TwCopyStdStringToClient)(std::string& destinationClientString, const std::string& sourceString);
//TW_API void		TW_CALL TwCopyStdStringToClientFunc(TwCopyStdStringToClient copyStdStringToClientFunc);
//TW_API void		TW_CALL TwCopyStdStringToLibrary(std::string& destinationLibraryString, const std::string& sourceClientString);
#	endif  // __cplusplus

	typedef enum ETwParamValueType
	{
		TW_PARAM_INT32,
		TW_PARAM_FLOAT,
		TW_PARAM_DOUBLE,
		TW_PARAM_CSTRING  // Null-terminated array of char (ie, c-string)
	} TwParamValueType;
	//TW_API int		TW_CALL TwGetParam(TwBar *bar, const char *varName, const char *paramName, TwParamValueType paramValueType, unsigned int outValueMaxCount, void *outValues);
	//TW_API int		TW_CALL TwSetParam(TwBar *bar, const char *varName, const char *paramName, TwParamValueType paramValueType, unsigned int inValueCount, const void *inValues);

#	ifdef __cplusplus
}  // extern "C"
#	endif  // __cplusplus

#endif  // !defined TW_INCLUDED
