# Skyrim Community Shaders

SKSE core plugin for community-driven advanced graphics modifications.

[Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/86492)

## Requirements

- [CMake](https://cmake.org/)
  - Add this to your `PATH`
- [PowerShell](https://github.com/PowerShell/PowerShell/releases/latest)
- [Vcpkg](https://github.com/microsoft/vcpkg)
  - Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
- [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
  - Desktop development with C++
- [CommonLibSSENG](https://github.com/alandtse/commonlibvr/tree/ng)
  - Add this as as an environment variable `CommonLibSSEPath`

## User Requirements

- [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
  - Needed for SSE/AE
- [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101)
  - Needed for VR

## Register Visual Studio as a Generator

- Open `x64 Native Tools Command Prompt`
- Run `cmake`
- Close the cmd window

## Building

```
git clone https://github.com/doodlum/skyrim-community-shaders.git
cd skyrim-community-shaders
# pull commonlib /extern to override the path settings
git submodule update --init --recursive
#configure cmake
cmake -S . --preset=ALL
#build dll
cmake --build build --config Release
```

## License

### Default

[GPL-3.0-or-later](COPYING) WITH [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS.md).  
Specifically, the Modded Code is Skyrim (and its variants) and Modding Libraries include [SKSE](https://skse.silverlock.org/) and Commonlib (and variants).

### Shaders

See LICENSE within each directory; if none, it's [Default](#default)

- [Features Shaders](features)
- [Package Shaders](package/Shaders/)
