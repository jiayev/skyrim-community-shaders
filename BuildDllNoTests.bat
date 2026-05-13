@echo off
setlocal

set "PRESET=ALL"
if not "%~1"=="" (
    set "PRESET=%~1"
)

set "CONFIG=Release"
if not "%~2"=="" (
    set "CONFIG=%~2"
)

set "BUILD_DIR=build\DllOnly-%PRESET%"

echo Configuring %PRESET% in %BUILD_DIR% for DLL-only build...
cmake -S . --preset="%PRESET%" -B "%BUILD_DIR%" ^
    -DBUILD_SHADER_TESTS=OFF ^
    -DZIP_TO_DIST=OFF ^
    -DAIO_ZIP_TO_DIST=OFF ^
    -DAUTO_PLUGIN_DEPLOYMENT=OFF
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building CommunityShaders DLL only (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target CommunityShaders --parallel
if errorlevel 1 exit /b %ERRORLEVEL%

endlocal
