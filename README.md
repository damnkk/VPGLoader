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

On Windows, the build deploys the viewer's runtime DLLs beside the executable,
including `VPGLoader.dll` in shared builds and third-party dependencies. Keep
these DLLs with the executable; launching it does not require a developer
shell or adding the library's build directory to `PATH`.

Run the viewer with any model format supported by the model loader:

```powershell
.\build\debug-example\examples\vpgloader-vulkan-viewer.exe `
  D:\assets\models\viking_room.obj
```

The viewer loads the complete CPU model through `ModelLoader`, iterates its
flat drawable list, uploads geometry and base-color textures to Vulkan, and
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

Model textures use the same public texture APIs as standalone callers:
external files and native KTX caches go through `Load`/`LoadCached`, embedded
encoded images through `LoadFromMemory`, and embedded raw pixels through
`LoadFromPixels`. Model export likewise uses `TextureConverter::SaveAsKtx2`.
The model module only resolves sources and material bindings; the texture
module owns decoding, channel conversion, pixel storage, and mip layout.

`LoadFromPixels(name, TexturePixelView)` accepts borrowed, tightly packed
RGBA8 or BGRA8 2D pixels and returns an owned RGBA8 texture with one mip level.
It preserves row order and color-space metadata without gamma conversion.
The input bytes can be released immediately after the call returns.

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
const glm::mat4 worldFromModel(1.0f); // Engine-owned object transform.

for (const auto& drawable : model->drawables) {
    const auto& mesh = model->meshes[drawable.meshIndex];
    const auto& material = model->materials[mesh.materialIndex];
    const glm::mat4 worldFromMesh = worldFromModel * drawable.modelFromMesh;
    // Draw mesh.indexCount indices at mesh.firstIndex, with mesh.firstVertex
    // as base vertex, using worldFromMesh and material.
}

const auto worldBounds = model->CalculateBounds(worldFromModel);
```

`LoadedModel` is a static rendering asset, not a scene graph. Its `drawables`
are mesh instances with a name, mesh index, and mesh-to-model matrix. Import
resolves the source hierarchy once (including empty parent nodes); the result
contains no parents, children, TRS copies, or transform-index indirection.
Multiple nodes referencing the same mesh share geometry. A node with several
material surfaces becomes several drawables. Source vertices stay in mesh
space, preserving normals and tangents without baking transforms into geometry.
Skinning, animation, cameras, and lights are outside this static model API.

Each `ModelMeshAsset` is one material surface: a range in the model-wide
structure-of-arrays geometry, a material index, and a mesh-local AABB. Indices
are local to the mesh's vertex range. `bounds` is the union of mesh bounds
transformed by the drawables, in model space. `CalculateBounds(worldFromModel)`
calculates world bounds in one linear pass; these are conservative transformed
mesh boxes, not a scan of every vertex. Empty models have invalid bounds.
For mutable `LoadedModel` builders, refresh `bounds = CalculateBounds()` after
editing drawable transforms; recompute mesh bounds when editing vertex data.
An engine can keep its own mutable instance transforms alongside the shared
immutable asset and use `TransformAABB`/`ExpandAABB` to update their bounds.

All math types are GLM vectors and matrices. Matrices are column-major, use
`matrix[3].xyz` for translation, and are affine (rotation, translation, scale,
or shear). Normal matrices and mirrored winding remain the renderer's concern.
Material texture members index `textureInfos`; each entry identifies a texture
plus its UV set and transform.

Material imports recognize the case-insensitive, dot-separated `DoubleSided`
name flag used by Falcor assets such as ORCA Bistro. FBX materials with no
explicit alpha mode and a base-color opacity of one use `AlphaMode::Mask`
when their loaded base-color texture has four channels. This follows Falcor's
base-color alpha convention without scanning pixels; RGB textures normalized
to RGBA have alpha filled with one, so all of their texels survive the cutoff.
Explicit alpha modes are preserved, and scalar opacity below one retains the
existing `AlphaMode::Blend` behavior. The texture-based default requires decoded
textures, so it is not applied with `loadTextures=false`. Resolved flags are
saved in `.vpgmodel`; re-import source FBX files to update older native assets.

External model textures use the same process-wide weak texture cache as
`TextureLoader::LoadCached()`. Embedded compressed images in GLB/FBX files are
extracted by Assimp as encoded bytes and then decoded by OpenImageIO through
`TextureLoader::LoadFromMemory()`; Assimp is not used as an image decoder.
Material parsing only registers texture requests. Once every material has
been parsed, VPGLoader loads the registered textures as one bounded parallel
batch. `ModelLoadOptions::maxTextureLoadConcurrency` controls the worker count;
zero selects the machine's reported hardware concurrency.
Set `loadTextures=false` to retain texture references and UV metadata without
decoding pixels. Set `loadEmbeddedTextures=false` to skip embedded image
decoding specifically. Embedded-only images have no filesystem path.
`loadMaterials=false` installs a single default material and omits texture
references. `globalScale` applies to both imported and native models: it scales
the drawable matrices without rewriting geometry or decomposing transforms.
A missing texture records a message in both
`ModelTextureAsset::loadError` and `LoadedModel::warnings` while leaving the
remaining model usable. Set `ModelLoadOptions::failOnMissingTextures` when a
missing texture should instead fail the whole load.

The handle is immutable to consumers. Releasing the final `ModelHandle`
releases geometry and its texture handles; texture pixel memory is freed once
no other texture handle or bounded cache retains it.

## Native model export

Export an already loaded CPU model with `ModelExporter`. Geometry, mesh
ranges, drawables, materials, texture-use metadata, mesh bounds, and
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
before returning the immutable model handle. Version 3 files target little-endian platforms.
Older `.vpgmodel` files must be regenerated from their original source files.
The format stores flat drawables; model bounds are derived again on load.

### Migration from the node API

- Iterate `model->drawables` instead of walking `asset.nodes` and `asset.submeshes`.
- Use `drawable.meshIndex` and `drawable.modelFromMesh` directly.
- `asset.name`, `asset.sourcePath`, and `asset.bounds` moved onto `LoadedModel`.
- Use GLM matrix multiplication directly; the redundant `Multiply` and
  `TransformPoint` wrappers were removed. `TransformAABB` accepts affine transforms.

## Maintenance and validation

The model pipeline is split by responsibility: `ModelLoader.cpp` handles
geometry and hierarchy flattening, `AssimpMaterials.cpp` handles material
translation and embedded images, `TextureLoading.cpp` owns bounded texture
workers and ordered error reporting, and `ModelBinary.cpp` owns native I/O.
File-boundary checks for truncated data, buffer ranges, and invalid references
remain in place. Internally, consumers can rely on valid mesh/material indices
instead of repeatedly skipping invalid records or substituting identity matrices.

Build and run the CPU regression tests (no Vulkan device needed):

```powershell
cmake -S . -B build/debug -DVPGLOADER_BUILD_TESTS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Tests cover hierarchy flattening, shared mesh instances, affine bounds,
material/texture load options, parallel texture errors, and native round trips.
