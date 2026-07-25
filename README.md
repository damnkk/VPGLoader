# VPGLoader

VPGLoader is a C++ resource-loading library. The first release establishes the
build and dependency foundation for image loading (OpenImageIO), model loading
(Assimp), and future KTX texture conversion (KTX-Software). The loading and
conversion APIs and implementations will follow in a later iteration.

## Prerequisites

- CMake 3.24 or newer
- A C++17 compiler
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set to its
  installation directory

Run the configure and build commands from a **Developer PowerShell for
Visual Studio 2026**. This supplies the MSVC compiler's `INCLUDE` and `LIB`
environment variables and the `nmake` build tool.

Dependencies are declared in `vcpkg.json`. CMake uses vcpkg manifest mode, so
the required ports are restored automatically during configuration.

## Build

Configure a static Debug build:

```powershell
$env:VCPKG_ROOT = "C:\\src\\vcpkg"
cmake -S . -B build/debug -G "NMake Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_SHARED_LIBS=OFF
cmake --build build/debug
```

To build the example executable, enable it at configure time:

```powershell
cmake -S . -B build/debug-example -G "NMake Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DVPGLOADER_BUILD_EXAMPLES=ON
cmake --build build/debug-example
```

For a shared Release library, use a separate build directory and set
`BUILD_SHARED_LIBS` to `ON`:

```powershell
cmake -S . -B build/release-shared -G "NMake Makefiles" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_SHARED_LIBS=ON
cmake --build build/release-shared
```

## Install and package

The install tree contains headers, the selected static/shared library, and a
CMake package target named `VPGLoader::VPGLoader`.

```powershell
cmake --install build/debug --prefix stage
cpack --config build/debug/CPackConfig.cmake
```

Set `VPGLOADER_ENABLE_PACKAGING=OFF` to omit CPack metadata. Packaging only
includes VPGLoader artifacts; consumers should resolve Assimp and OpenImageIO
through the same vcpkg toolchain.

## Repository layout

```text
include/VPGLoader/  Public library headers
src/image/          Future OpenImageIO-backed image loader
src/model/          Future Assimp-backed model loader
src/texture/        Future KTX-backed texture converter
examples/           Optional CMake example
cmake/              Installed-package configuration template
```

## Texture ownership and cache

Image loading returns a `TextureHandle` (`std::shared_ptr<const Texture>`).
The underlying `Texture` owns a contiguous `std::vector<uint8_t>` and its
metadata, including the source path, filename, size, pixel format, dimensions,
and mip layout. Copy the handle, not `imageData`; data remains valid while at
least one handle exists.

`TextureLoader::LoadCached()` uses a process-wide weak cache. It coalesces
concurrent requests and reuses an already-live texture, but does not retain
CPU pixel data after callers release their handles. This is the appropriate
default for a renderer that uploads a texture to the GPU and then releases the
CPU copy. Call `TextureCache::Default().Invalidate(path)` when a source file
changes.

For an editor, preview tool, or conversion batch that benefits from keeping a
bounded CPU cache, create a `TextureCache` with a byte budget:

```cpp
vpgloader::TextureCache previewCache({ 128 * 1024 * 1024 });
auto texture = previewCache.Load("assets/albedo.png");

// No pixel copy: the converter reads from the same immutable Texture.
vpgloader::texture::TextureConverter::SaveAsKtx(texture, "assets/albedo.ktx");
```

The initial KTX writer emits an uncompressed KTX 1.1 file for one- to
four-channel `UInt8` textures. KTX2, supercompression, and GPU block formats
are deliberately deferred until their output policy is specified.
