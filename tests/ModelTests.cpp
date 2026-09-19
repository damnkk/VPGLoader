#include <VPGLoader/VPGLoader.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace vpgloader;
namespace fs = std::filesystem;

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Error, class Function>
void RequireError(Function&& function, const char* message)
{
    try {
        function();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

bool Near(const glm::vec3& a, const glm::vec3& b)
{
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(0.0001f)));
}

bool Near(const glm::mat4& a, const glm::mat4& b)
{
    for (int column = 0; column < 4; ++column) {
        if (!glm::all(glm::lessThan(glm::abs(a[column] - b[column]), glm::vec4(0.0001f)))) {
            return false;
        }
    }
    return true;
}

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    Require(stream.good(), "Cannot write fixture");
}

void CheckRawPixels(const fs::path& directory)
{
    Texture::ByteBuffer source = {20, 40, 80, 255, 3, 2, 1, 128};
    const Texture::ByteBuffer rgba = {80, 40, 20, 255, 1, 2, 3, 128};
    TexturePixelView pixels;
    pixels.data = source.data();
    pixels.size = source.size();
    pixels.width = 1;
    pixels.height = 2;
    pixels.layout = TexturePixelLayout::Bgra8;
    pixels.colorSpace = TextureColorSpace::SRGB;
    const auto texture = TextureLoader::LoadFromPixels("raw-image", pixels);
    Require(texture->imageData() == rgba, "BGRA conversion changed pixel or row order");
    Require(source[0] == 20, "Raw loading modified borrowed input");
    source[0] = 0;
    Require(texture->imageData() == rgba, "Raw texture still aliases borrowed input");
    const auto& info = texture->info();
    Require(info.width == 1 && info.height == 2 && info.depth == 1
            && info.colorSpace == TextureColorSpace::SRGB
            && info.format.componentType == TextureComponentType::UInt8
            && info.format.channels == 4 && info.mipLevels.size() == 1
            && info.mipLevels[0].byteOffset == 0 && info.mipLevels[0].byteSize == rgba.size(),
            "Raw texture metadata is inconsistent");

    pixels.data = rgba.data();
    pixels.layout = TexturePixelLayout::Rgba8;
    Require(TextureLoader::LoadFromPixels("rgba", pixels)->imageData() == rgba,
            "RGBA source should retain its channels");
    texture::TextureConverter::SaveAsKtx2(texture, directory / "raw.ktx2");
    Require(TextureLoader::Load(directory / "raw.ktx2")->imageData() == rgba,
            "Raw texture did not round-trip through the public KTX interfaces");

    --pixels.size;
    RequireError<TextureLoadError>([&] { TextureLoader::LoadFromPixels("short", pixels); },
                                   "Truncated raw pixels were accepted");
    RequireError<TextureLoadError>([] { TextureLoader::LoadFromPixels("empty", {}); },
                                   "Empty raw pixels were accepted");
}

// Two surfaces, each instanced twice. An empty parent contributes reflection,
// rotation, and nonuniform scale; the other branch contains a shear matrix.
std::string WriteFixture(const fs::path& directory)
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::uint16_t indices[] = {0, 1, 2};
    std::ofstream geometry(directory / "triangle.bin", std::ios::binary);
    geometry.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    geometry.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    geometry.close();
    Require(geometry.good(), "Cannot write geometry fixture");

    // A one-pixel, uncompressed TGA, decoded by the normal OIIO image path.
    const unsigned char tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0, 20, 40, 80,
    };
    std::ofstream image(directory / "color.tga", std::ios::binary);
    image.write(reinterpret_cast<const char*>(tga), sizeof(tga));
    image.close();
    Require(image.good(), "Cannot write image fixture");

    const std::string document = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name":"root", "translation":[10,0,0], "children":[1,3]},
    {"name":"empty-parent", "rotation":[0,0,0.7071067811865476,0.7071067811865476],
     "scale":[-2,3,1], "children":[2]},
    {"name":"instance-a", "translation":[1,2,0], "mesh":0},
    {"name":"instance-b", "matrix":[1,0,0,0,0.5,1,0,0,0,0,1,0,-5,0,0,1], "mesh":0}
  ],
  "buffers": [{"uri":"triangle.bin", "byteLength":42}],
  "bufferViews": [
    {"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
    {"buffer":0,"byteOffset":36,"byteLength":6,"target":34963}
  ],
  "accessors": [
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
    {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}
  ],
  "images": [{"uri":"color.tga"}],
  "textures": [{"source":0}],
  "materials": [
    {"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},
    {"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,1]},"normalTexture":{"index":0}}
  ],
  "meshes": [{"primitives":[
    {"attributes":{"POSITION":0},"indices":1,"material":0},
    {"attributes":{"POSITION":0},"indices":1,"material":1}
  ]}]
})";
    WriteText(directory / "scene.gltf", document);
    return document;
}

void CheckFlattening(const LoadedModel& model)
{
    Require(model.drawables.size() == 4, "Expected one drawable per node/surface use");
    Require(model.meshes.size() == 2, "Instances must share two meshes");
    Require(model.geometry.positions.size() == 6, "Instancing duplicated geometry");
    Require(model.bounds.valid && Near(model.bounds.min, {1, -4, 0})
            && Near(model.bounds.max, {6.5f, 1, 0}), "Wrong model bounds");

    glm::mat4 expectedA(1.0f);
    expectedA[0] = {0, -2, 0, 0};
    expectedA[1] = {-3, 0, 0, 0};
    expectedA[3] = {4, -2, 0, 1};
    glm::mat4 expectedB(1.0f);
    expectedB[1][0] = 0.5f;
    expectedB[3][0] = 5;
    unsigned int uses[2] = {};
    for (const auto& drawable : model.drawables) {
        Require(drawable.meshIndex < 2, "Bad drawable mesh reference");
        ++uses[drawable.meshIndex];
        Require(drawable.name == "instance-a" || drawable.name == "instance-b",
                "Empty nodes must not survive flattening");
        Require(Near(drawable.modelFromMesh,
                     drawable.name == "instance-a" ? expectedA : expectedB),
                "Hierarchy multiplication order or matrix conversion is wrong");
    }
    Require(uses[0] == 2 && uses[1] == 2, "Lost mesh instance");

    const auto world = glm::translate(glm::mat4(1), glm::vec3(3, 4, 5));
    const AABB worldBounds = model.CalculateBounds(world);
    Require(Near(worldBounds.min, {4, 0, 5}) && Near(worldBounds.max, {9.5f, 5, 5}),
            "World bounds did not compose the object transform");
}

void CheckAabb()
{
    const AABB box{{-2, -1, 3}, {4, 5, 9}, true};
    glm::mat4 matrix = glm::rotate(glm::mat4(1), 0.7f, glm::vec3(0, 0, 1));
    matrix = glm::scale(matrix, glm::vec3(-2, 3, 0.5f));
    matrix[2][0] += 0.8f;
    matrix[3] = {3, 4, -5, 1};
    AABB reference;
    for (unsigned int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{
            corner & 1 ? box.max.x : box.min.x,
            corner & 2 ? box.max.y : box.min.y,
            corner & 4 ? box.max.z : box.min.z};
        ExpandAABB(reference, glm::vec3(matrix * glm::vec4(point, 1)));
    }
    const auto actual = TransformAABB(box, matrix);
    Require(Near(reference.min, actual.min) && Near(reference.max, actual.max),
            "Affine AABB transform differs from corner reference");
    Require(!TransformAABB({}, matrix).valid, "Empty AABB became valid");
    Require(!LoadedModel{}.CalculateBounds().valid, "Empty model has bounds");
}

void CheckEmbedded(const fs::path& directory, std::string fixture)
{
    fixture.replace(fixture.find("color.tga"), 9,
                    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGMI0BD5DwAC5AGMgM9tMwAAAABJRU5ErkJggg==");
    WriteText(directory / "embedded.gltf", fixture);
    const auto model = ModelLoader::Load(directory / "embedded.gltf");
    Require(model->textures.size() == 2 && model->textures[0].embedded
            && model->textures[0].texture && model->textures[1].texture,
            "Embedded PNG import failed");
    Require(model->textures[0].texture->imageData() == Texture::ByteBuffer({80, 40, 20, 255}),
            "Embedded image pixels changed");

    ModelLoadOptions skipEmbedded;
    skipEmbedded.loadEmbeddedTextures = false;
    skipEmbedded.failOnMissingTextures = true;
    const auto skipped = ModelLoader::Load(directory / "embedded.gltf", skipEmbedded);
    Require(skipped->textures.size() == 2 && skipped->textures[0].embedded
            && !skipped->textures[0].texture && skipped->textures[0].loadError.empty(),
            "Intentionally skipped embedded texture lost metadata or reported failure");
    skipEmbedded.loadTextures = false;
    const auto metadata = ModelLoader::Load(directory / "embedded.gltf", skipEmbedded);
    Require(metadata->textures[0].embedded && !metadata->textures[0].texture,
            "Metadata-only import lost embedded texture identity");
}

void CheckRoundTrip(const fs::path& directory, const ModelHandle& source)
{
    const auto path = directory / "scene.vpgmodel";
    ModelExporter::Save(source, path);
    const auto restored = ModelLoader::Load(path);
    CheckFlattening(*restored);
    Require(restored->geometry.positions == source->geometry.positions
            && restored->geometry.normals == source->geometry.normals
            && restored->geometry.tangents == source->geometry.tangents
            && restored->geometry.texCoords0 == source->geometry.texCoords0
            && restored->geometry.texCoords1 == source->geometry.texCoords1
            && restored->geometry.colors == source->geometry.colors
            && restored->geometry.indices == source->geometry.indices,
            "Native geometry round-trip changed data");
    Require(restored->textures.size() == 2, "Native texture references lost");
    Require(restored->materials[0].pbrBaseColorTexture == source->materials[0].pbrBaseColorTexture
            && restored->materials[1].pbrBaseColorFactor == source->materials[1].pbrBaseColorFactor,
            "Native material round-trip changed data");
    Require(restored->textureInfos.size() == source->textureInfos.size()
            && restored->textureInfos[1].textureIndex == source->textureInfos[1].textureIndex,
            "Native texture binding round-trip changed data");
    for (std::size_t index = 0; index < restored->textures.size(); ++index) {
        Require(restored->textures[index].texture->imageData()
                    == source->textures[index].texture->imageData(),
                "KTX pixel round-trip changed data");
    }

    ModelLoadOptions metadataOnly;
    metadataOnly.loadTextures = false;
    const auto metadata = ModelLoader::Load(path, metadataOnly);
    Require(metadata->textures.size() == 2 && !metadata->textures[0].texture,
            "Native metadata-only load decoded pixels");
    ModelLoadOptions noMaterials;
    noMaterials.loadMaterials = false;
    const auto geometryOnly = ModelLoader::Load(path, noMaterials);
    Require(geometryOnly->materials.size() == 1 && geometryOnly->textures.empty(),
            "Native loadMaterials ignored");
    for (const auto& mesh : geometryOnly->meshes) {
        Require(mesh.materialIndex == 0, "Default material was not applied");
    }

    ModelLoadOptions scaled;
    scaled.globalScale = 2;
    const auto scaledModel = ModelLoader::Load(path, scaled);
    Require(Near(scaledModel->bounds.min, source->bounds.min * 2.0f)
            && Near(scaledModel->bounds.max, source->bounds.max * 2.0f),
            "Native globalScale ignored");

    // A failed metadata check must leave no successful texture result behind.
    TextureInfo info;
    info.width = info.height = info.depth = 1;
    info.format = {TextureComponentType::UInt16, 4};
    info.mipLevels.push_back({1, 1, 1, 0, 8});
    auto wrongPrecision = Texture::Create(info, Texture::ByteBuffer(8, 0));
    texture::TextureConverter::SaveAsKtx2(wrongPrecision, restored->textures[0].sourcePath);
    ModelLoadOptions uncached;
    uncached.cacheExternalTextures = false;
    uncached.maxTextureLoadConcurrency = 2;
    const auto mismatch = ModelLoader::Load(path, uncached);
    Require(!mismatch->textures[0].texture && !mismatch->textures[0].loadError.empty(),
            "Rejected texture was committed as a successful load");
    uncached.failOnMissingTextures = true;
    RequireError<ModelLoadError>([&] { ModelLoader::Load(path, uncached); },
                                "Strict native texture loading did not fail");

    fs::copy_file(path, directory / "old.vpgmodel", fs::copy_options::overwrite_existing);
    {
        std::fstream old(directory / "old.vpgmodel", std::ios::binary | std::ios::in | std::ios::out);
        old.seekp(8);
        const char version[] = {2, 0, 0, 0};
        old.write(version, sizeof(version));
    }
    RequireError<ModelLoadError>([&] { ModelLoader::Load(directory / "old.vpgmodel"); },
                                "Old native format was silently accepted");
    WriteText(directory / "truncated.vpgmodel", "VPGMODEL");
    RequireError<ModelLoadError>([&] { ModelLoader::Load(directory / "truncated.vpgmodel"); },
                                "Truncated native file was accepted");

    LoadedModel invalid;
    invalid.drawables.push_back({"bad", 7, glm::mat4(1)});
    RequireError<ModelExportError>([&] { ModelExporter::Save(invalid, directory / "invalid.vpgmodel"); },
                                  "Invalid drawable reference was exported");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "Pass a fixture output directory");
        const fs::path directory = fs::u8path(argv[1]);
        fs::create_directories(directory);
        CheckRawPixels(directory);
        const auto fixture = WriteFixture(directory);
        CheckAabb();
        ModelLoadOptions options;
        options.maxTextureLoadConcurrency = 2;
        const auto model = ModelLoader::Load(directory / "scene.gltf", options);
        CheckFlattening(*model);
        Require(model->textures.size() == 2 && model->textures[0].texture
                && model->textures[1].texture, "Material texture decoding failed");
        CheckRoundTrip(directory, model);
        CheckEmbedded(directory, fixture);

        options.loadTextures = false;
        const auto metadata = ModelLoader::Load(directory / "scene.gltf", options);
        Require(metadata->textures.size() == 2 && !metadata->textures[0].texture
                && metadata->materials[0].pbrBaseColorTexture != InvalidMaterialTexture,
                "Assimp metadata-only load lost texture bindings");
        options.loadMaterials = false;
        const auto geometryOnly = ModelLoader::Load(directory / "scene.gltf", options);
        Require(geometryOnly->materials.size() == 1 && geometryOnly->textures.empty(),
                "Assimp loadMaterials ignored");
        options.globalScale = 2;
        const auto scaled = ModelLoader::Load(directory / "scene.gltf", options);
        Require(Near(scaled->bounds.min, model->bounds.min * 2.0f)
                && Near(scaled->bounds.max, model->bounds.max * 2.0f), "Assimp globalScale is wrong");

        std::string missingFixture = fixture;
        missingFixture.replace(missingFixture.find("color.tga"), 9, "missing.png");
        WriteText(directory / "missing.gltf", missingFixture);
        options = {};
        options.maxTextureLoadConcurrency = 2;
        const auto missing = ModelLoader::Load(directory / "missing.gltf", options);
        Require(missing->warnings.size() >= 2 && !missing->textures[0].texture,
                "Missing textures should preserve the model with diagnostics");
        options.failOnMissingTextures = true;
        RequireError<ModelLoadError>([&] { ModelLoader::Load(directory / "missing.gltf", options); },
                                    "Strict Assimp texture loading did not fail");
        std::cout << "Model import, bounds, texture, and native round-trip tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
