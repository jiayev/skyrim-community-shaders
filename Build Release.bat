@echo off

RMDIR dist /S /Q

cmake -S . --preset=ALL --check-stamp-file "build\CMakeFiles\generate.stamp"
if %ERRORLEVEL% NEQ 0 exit 1
cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 exit 1

xcopy "build\release\*.dll" "dist\SKSE\Plugins\" /I /Y
xcopy "build\release\*.pdb" "dist\SKSE\Plugins\" /I /Y

xcopy "package" "dist" /I /Y /E

pause