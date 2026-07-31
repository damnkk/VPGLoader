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
automatically orbits the model-space bounds. Press `E`, switch to the console,
and enter a `.vpgmodel` path (or an existing directory) to export the current
CPU model and its sibling KTX textures. Use the mouse wheel to move the orbit
camera closer or farther away. Press Escape or close the window to exit. The
optional `--frames N` argument limits the render loop for smoke tests.

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
includes VPGLoader artifacts; consumers should resolve Assimp, GLM,
OpenImageIO, and KTX-Software through the same vcpkg toolchain.

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

`TextureLoader` preserves supported source component precision (`UInt8`,
`UInt16`, `Float16`, or `Float32`) instead of converting every image to
`UInt8`. Loaded textures always use four RGBA channels: missing color channels
are filled with zero and a missing alpha channel is filled with one, without
changing component precision.

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
vpgloader::texture::TextureConverter::SaveAsKtx2(
    texture, "assets/albedo.ktx2");
```

The primary writer emits an uncompressed KTX 2.0 file with an explicit
`VkFormat` for one- to four-channel `UInt8`, `UInt16`, `Float16`, and
`Float32` textures. The legacy `SaveAsKtx()` KTX 1.1 writer and loader remain
available for existing assets. KTX2 supercompression and GPU block formats
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

Model math data is exposed directly as GLM types: geometry uses
`glm::vec2`/`glm::vec3`/`glm::vec4`, nodes use `glm::quat`, and transforms use
`glm::mat4`. Geometry and transforms can therefore be passed to GLM- or
Vulkan-facing code without conversion. Matrices are column-major and use
`matrix[3].xyz` for translation.

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

## Native model export

Export an already loaded CPU model with `ModelExporter`. Geometry, mesh
ranges, nodes, transforms, materials, texture-use metadata, bounds, and
warnings are written to a versioned little-endian binary file. Each loaded
texture is written as an uncompressed KTX2 file beside it:

```cpp
#include <VPGLoader/VPGLoader.hpp>

auto model = vpgloader::ModelLoader::Load("assets/scene.glb");
vpgloader::ModelExporter::Save(model, "cache/scene.vpgmodel");
```

The generated texture filenames include the model name and texture index, so
textures with identical source filenames do not collide. A `.vpgmodel` stores
only sibling texture filenames; moving a native model therefore means moving
its generated KTX2 files with it.

`ModelLoader::Load()` auto-detects the extension and reads the native binary
without invoking Assimp or OpenImageIO. Geometry arrays are restored with
contiguous block reads, while the KTX2 files are decoded directly by
KTX-Software. The existing texture cache, parallel loading limit,
`loadTextures`, and `failOnMissingTextures` options continue to apply:

```cpp
auto cachedModel = vpgloader::ModelLoader::Load("cache/scene.vpgmodel");
```

The file header contains a magic value, format version, and byte-order marker.
The reader validates all sizes, enum values, ranges, and cross-references
before returning the immutable model handle. Version 1 files intentionally
target little-endian platforms.
