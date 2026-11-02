@echo off
setlocal

rem Usage: BuildAIONoShaderTests.bat [CONFIGURE_PRESET] [CONFIG]
cd /d "%~dp0"
if errorlevel 1 exit /b %ERRORLEVEL%

set "PRESET=Release"
if not "%~1"=="" (
    set "PRESET=%~1"
)

set "CONFIG=Release"
if not "%~2"=="" (
    set "CONFIG=%~2"
)

rem Use Ninja as the working Vulkan builds do; keep old VS build caches separate.
set "BUILD_DIR=build\AIONoShaderTests-%PRESET%-Ninja"

where cl >nul 2>&1
if not errorlevel 1 goto :vsready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found; run from a VS x64 developer prompt.
    exit /b 1
)
set "VSINSTALL="
set "VSINSTALL_FILE=%TEMP%\cs_aio_vsinstall_%RANDOM%.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSINSTALL_FILE%"
set /p VSINSTALL=<"%VSINSTALL_FILE%"
del "%VSINSTALL_FILE%" >nul 2>&1
if not defined VSINSTALL (
    echo ERROR: No Visual Studio C++ toolchain found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b %ERRORLEVEL%
:vsready

rem Complete AIO packages need the current Vulkan runtime dependencies.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-dxvk.ps1" -Required
if errorlevel 1 exit /b %ERRORLEVEL%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-streamline.ps1" -Required
if errorlevel 1 exit /b %ERRORLEVEL%

rem Shader validation is separate from the package target on this branch.
echo Configuring %PRESET% in %BUILD_DIR% for AIO package without shader validation...
cmake -S . --preset="%PRESET%" -G Ninja -A "" -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE="%CONFIG%" ^
    -DCMAKE_CXX_FLAGS="/EHsc %CXXFLAGS%" ^
    -DVulkan_INCLUDE_DIR="%~dp0extern/dxvk/include/vulkan/include" ^
    -DZIP_TO_DIST=OFF ^
    -DAIO_ZIP_TO_DIST=ON ^
    -DAUTO_PLUGIN_DEPLOYMENT=OFF
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building Package-AIO-Manual (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target Package-AIO-Manual --parallel
if errorlevel 1 exit /b %ERRORLEVEL%

endlocal
