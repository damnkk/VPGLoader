# VPGLoader

VPGLoader is a C++17 CPU resource-loading library. OpenImageIO decodes texture
files, Assimp produces complete model data, and KTX-Software writes loaded
textures as KTX. The public objects contain no Vulkan or renderer state.

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
  -DVPGLOADER_BUILD_SAMPLE=ON
cmake --build build/debug-example
```

The Vulkan viewer example additionally requires a Vulkan SDK with `glslc`.
GLFW is restored through `vcpkg.json`; Vulkan remains an example-only system
dependency and is not linked into VPGLoader itself.

Run the viewer with any model format supported by the model loader:

```powershell
.\build\debug-example\examples\vpgloader-vulkan-viewer.exe `
  D:\assets\models\viking_room.obj
```

The viewer loads the complete CPU model through `ModelLoader`, traverses its
node hierarchy, uploads geometry and base-color textures to Vulkan, and
automatically orbits the model-space bounds. Press Escape or close the window
to exit. The optional `--frames N` argument limits the render loop for smoke
tests.

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
includes VPGLoader artifacts; consumers should resolve Assimp, OpenImageIO,
and KTX-Software through the same vcpkg toolchain.

## Repository layout

```text
include/VPGLoader/  Public library headers
src/model/          Assimp-backed CPU model loader
src/texture/        OpenImageIO loading, cache, and KTX conversion
examples/           Optional Vulkan model-viewer example and GLSL shaders
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

## Model loading

`ModelLoader::Load()` returns a `ModelHandle`
(`std::shared_ptr<const LoadedModel>`), so passing a loaded model between
systems never copies its geometry or texture bytes:

```cpp
auto model = vpgloader::ModelLoader::Load("assets/scene.glb");

for (const auto& node : model->asset.nodes) {
    for (const auto submeshIndex : node.submeshIndices) {
        const auto& submesh = model->asset.submeshes[submeshIndex];
        const auto& mesh = model->meshes[submesh.meshIndex];
        // Interpret the node and mesh in the consuming scene or renderer.
    }
}
```

The result contains model-wide structure-of-arrays geometry, per-mesh ranges,
materials, material texture-use metadata, decoded `TextureHandle` objects,
the original node hierarchy, local transforms, submesh associations, and
model-space bounds. Material texture members index `textureInfos`; each
texture-info entry then identifies a texture plus its UV set and transform.

External model textures use the same process-wide weak texture cache as
`TextureLoader::LoadCached()`. Embedded compressed images in GLB/FBX files are
extracted by Assimp as encoded bytes and then decoded by OpenImageIO through
`TextureLoader::LoadFromMemory()`; Assimp is not used as an image decoder.
Material parsing only registers texture requests. Once every material has
been parsed, VPGLoader loads the registered textures as one bounded parallel
batch. `ModelLoadOptions::maxTextureLoadConcurrency` controls the worker count;
zero selects the machine's reported hardware concurrency.
Set `ModelLoadOptions::loadEmbeddedTextures` to `false` for a path-only
workflow. Embedded-only images then have no filesystem path and cannot be
loaded. A missing texture records a message in both
`ModelTextureAsset::loadError` and `LoadedModel::warnings` while leaving the
remaining model usable. Set `ModelLoadOptions::failOnMissingTextures` when a
missing texture should instead fail the whole load.

The handle is immutable to consumers. Releasing the final `ModelHandle`
releases geometry and its texture handles; texture pixel memory is freed once
no other texture handle or bounded cache retains it.
