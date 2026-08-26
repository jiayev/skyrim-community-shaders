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

set "BUILD_DIR=build\AIONoShaderTests-%PRESET%"

echo Configuring %PRESET% in %BUILD_DIR% for AIO package without ShaderTest...
cmake -S . --preset="%PRESET%" -B "%BUILD_DIR%" ^
    -DBUILD_SHADER_TESTS=OFF ^
    -DZIP_TO_DIST=OFF ^
    -DAIO_ZIP_TO_DIST=ON ^
    -DAUTO_PLUGIN_DEPLOYMENT=OFF
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building CommunityShaders DLL and Package-AIO-Manual (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target CommunityShaders Package-AIO-Manual --parallel
if errorlevel 1 exit /b %ERRORLEVEL%

endlocal
