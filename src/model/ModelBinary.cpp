#include "ModelBinary.hpp"

#include <VPGLoader/ModelExporter.hpp>
#include <VPGLoader/ModelLoader.hpp>
#include <VPGLoader/TextureConverter.hpp>
#include <VPGLoader/TextureLoader.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace vpgloader::detail {
namespace {

constexpr std::array<std::uint8_t, 8> FileMagic = {
    'V', 'P', 'G', 'M', 'O', 'D', 'E', 'L',
};
constexpr std::uint32_t FileVersion = 1;
constexpr std::uint32_t EndianMarker = 0x01020304U;
constexpr std::uint64_t MaximumStringBytes = 1024ULL * 1024ULL * 1024ULL;

static_assert(sizeof(float) == 4, ".vpgmodel requires IEEE-754 32-bit floats.");
static_assert(
    std::numeric_limits<float>::is_iec559,
    ".vpgmodel requires IEEE-754 floating-point representation.");
static_assert(sizeof(Float2) == 8, "Float2 must have a packed binary layout.");
static_assert(sizeof(Float3) == 12, "Float3 must have a packed binary layout.");
static_assert(sizeof(Float4) == 16, "Float4 must have a packed binary layout.");
static_assert(sizeof(Matrix4) == 64, "Matrix4 must have a packed binary layout.");
static_assert(std::is_trivially_copyable_v<Float2>);
static_assert(std::is_trivially_copyable_v<Float3>);
static_assert(std::is_trivially_copyable_v<Float4>);
static_assert(std::is_trivially_copyable_v<Matrix4>);

bool IsLittleEndian() noexcept
{
    const std::uint16_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

std::string LowerAscii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::filesystem::path ResolvePath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path result = std::filesystem::absolute(path, error);
    if (error) {
        return path.lexically_normal();
    }
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(result, error);
    return error ? result.lexically_normal() : canonical;
}

class BinaryWriter final {
public:
    explicit BinaryWriter(const std::filesystem::path& path)
        : path_(path)
        , stream_(path, std::ios::binary | std::ios::trunc)
    {
        if (!stream_) {
            throw ModelExportError(
                "Unable to open model output '" + path.u8string() + "'.");
        }
    }

    void WriteRaw(const void* data, std::size_t size)
    {
        if (size == 0) {
            return;
        }
        stream_.write(
            static_cast<const char*>(data),
            static_cast<std::streamsize>(size));
        if (!stream_) {
            throw ModelExportError(
                "Unable to write model output '" + path_.u8string() + "'.");
        }
    }

    void WriteU8(std::uint8_t value)
    {
        WriteRaw(&value, sizeof(value));
    }

    void WriteU32(std::uint32_t value)
    {
        const std::array<std::uint8_t, 4> bytes = {
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 24),
        };
        WriteRaw(bytes.data(), bytes.size());
    }

    void WriteU64(std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        WriteRaw(bytes.data(), bytes.size());
    }

    void WriteFloat(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteU32(bits);
    }

    void WriteString(const std::string& value)
    {
        WriteU64(static_cast<std::uint64_t>(value.size()));
        WriteRaw(value.data(), value.size());
    }

    template <typename T>
    void WriteRawVector(const std::vector<T>& values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        WriteU64(static_cast<std::uint64_t>(values.size()));
        if (values.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw ModelExportError("Model array byte size exceeds size_t.");
        }
        WriteRaw(values.data(), values.size() * sizeof(T));
    }

    void Finish()
    {
        stream_.flush();
        if (!stream_) {
            throw ModelExportError(
                "Unable to finish model output '" + path_.u8string() + "'.");
        }
    }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
};

class BinaryReader final {
public:
    explicit BinaryReader(const std::filesystem::path& path)
        : path_(path)
        , stream_(path, std::ios::binary)
    {
        if (!stream_) {
            throw ModelLoadError(
                "Unable to open VPG model '" + path.u8string() + "'.");
        }

        stream_.seekg(0, std::ios::end);
        const std::streamoff end = stream_.tellg();
        if (end < 0) {
            throw ModelLoadError(
                "Unable to determine VPG model size for '" + path.u8string() + "'.");
        }
        remaining_ = static_cast<std::uint64_t>(end);
        stream_.seekg(0, std::ios::beg);
    }

    void ReadRaw(void* destination, std::size_t size)
    {
        if (static_cast<std::uint64_t>(size) > remaining_) {
            throw ModelLoadError(
                "VPG model '" + path_.u8string() + "' is truncated.");
        }
        if (size != 0) {
            stream_.read(
                static_cast<char*>(destination),
                static_cast<std::streamsize>(size));
            if (!stream_) {
                throw ModelLoadError(
                    "Unable to read VPG model '" + path_.u8string() + "'.");
            }
        }
        remaining_ -= static_cast<std::uint64_t>(size);
    }

    std::uint8_t ReadU8()
    {
        std::uint8_t value = 0;
        ReadRaw(&value, sizeof(value));
        return value;
    }

    std::uint32_t ReadU32()
    {
        std::array<std::uint8_t, 4> bytes{};
        ReadRaw(bytes.data(), bytes.size());
        return static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8)
            | (static_cast<std::uint32_t>(bytes[2]) << 16)
            | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::uint64_t ReadU64()
    {
        std::array<std::uint8_t, 8> bytes{};
        ReadRaw(bytes.data(), bytes.size());
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
        }
        return value;
    }

    float ReadFloat()
    {
        const std::uint32_t bits = ReadU32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string ReadString()
    {
        const std::uint64_t size = ReadU64();
        if (size > remaining_ || size > MaximumStringBytes
            || size > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw ModelLoadError(
                "VPG model '" + path_.u8string() + "' contains an invalid string size.");
        }
        std::string value(static_cast<std::size_t>(size), '\0');
        ReadRaw(value.data(), value.size());
        return value;
    }

    template <typename T>
    std::vector<T> ReadRawVector()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint64_t count = ReadU64();
        if (count > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max() / sizeof(T))) {
            throw ModelLoadError(
                "VPG model '" + path_.u8string() + "' contains an oversized array.");
        }
        const std::size_t byteSize =
            static_cast<std::size_t>(count) * sizeof(T);
        if (static_cast<std::uint64_t>(byteSize) > remaining_) {
            throw ModelLoadError(
                "VPG model '" + path_.u8string() + "' contains a truncated array.");
        }
        std::vector<T> values(static_cast<std::size_t>(count));
        ReadRaw(values.data(), byteSize);
        return values;
    }

    std::size_t ReadCount(std::size_t minimumBytesPerElement = 1)
    {
        const std::uint64_t count = ReadU64();
        if (count > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())
            || (minimumBytesPerElement != 0
                && count > remaining_ / minimumBytesPerElement)) {
            throw ModelLoadError(
                "VPG model '" + path_.u8string() + "' contains an invalid object count.");
        }
        return static_cast<std::size_t>(count);
    }

    std::uint64_t remaining() const noexcept
    {
        return remaining_;
    }

private:
    std::filesystem::path path_;
    std::ifstream stream_;
    std::uint64_t remaining_ = 0;
};

void WriteFloat2(BinaryWriter& writer, const Float2& value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
}

Float2 ReadFloat2(BinaryReader& reader)
{
    return {reader.ReadFloat(), reader.ReadFloat()};
}

void WriteFloat3(BinaryWriter& writer, const Float3& value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
}

Float3 ReadFloat3(BinaryReader& reader)
{
    return {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
}

void WriteFloat4(BinaryWriter& writer, const Float4& value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
    writer.WriteFloat(value.w);
}

Float4 ReadFloat4(BinaryReader& reader)
{
    return {
        reader.ReadFloat(),
        reader.ReadFloat(),
        reader.ReadFloat(),
        reader.ReadFloat(),
    };
}

void WriteQuaternion(BinaryWriter& writer, const Quaternion& value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
    writer.WriteFloat(value.w);
}

Quaternion ReadQuaternion(BinaryReader& reader)
{
    Quaternion value;
    value.x = reader.ReadFloat();
    value.y = reader.ReadFloat();
    value.z = reader.ReadFloat();
    value.w = reader.ReadFloat();
    return value;
}

void WriteAabb(BinaryWriter& writer, const AABB& value)
{
    WriteFloat3(writer, value.min);
    WriteFloat3(writer, value.max);
    writer.WriteU8(value.valid ? 1U : 0U);
}

AABB ReadAabb(BinaryReader& reader)
{
    AABB value;
    value.min = ReadFloat3(reader);
    value.max = ReadFloat3(reader);
    const std::uint8_t valid = reader.ReadU8();
    if (valid > 1) {
        throw ModelLoadError("VPG model contains an invalid AABB flag.");
    }
    value.valid = valid != 0;
    return value;
}

void WriteIndexVector(BinaryWriter& writer,
                      const std::vector<std::uint32_t>& values)
{
    writer.WriteRawVector(values);
}

std::vector<std::uint32_t> ReadIndexVector(BinaryReader& reader)
{
    return reader.ReadRawVector<std::uint32_t>();
}

void RequireIndex(std::uint32_t index,
                  std::size_t count,
                  bool allowInvalid,
                  const char* description,
                  bool exporting)
{
    if ((allowInvalid && index == InvalidModelIndex) || index < count) {
        return;
    }
    const std::string message =
        std::string(description) + " is outside its referenced array.";
    if (exporting) {
        throw ModelExportError(message);
    }
    throw ModelLoadError("Invalid VPG model: " + message);
}

void ValidateModel(const LoadedModel& model, bool exporting)
{
    const auto fail = [exporting](const std::string& message) {
        if (exporting) {
            throw ModelExportError(message);
        }
        throw ModelLoadError("Invalid VPG model: " + message);
    };

    const std::size_t vertexCount = model.geometry.positions.size();
    if (model.geometry.normals.size() != vertexCount
        || model.geometry.tangents.size() != vertexCount
        || model.geometry.texCoords0.size() != vertexCount
        || model.geometry.texCoords1.size() != vertexCount
        || model.geometry.colors.size() != vertexCount) {
        fail("All model vertex streams must have the same element count.");
    }

    for (const ModelMeshAsset& mesh : model.meshes) {
        const std::uint64_t vertexEnd =
            static_cast<std::uint64_t>(mesh.firstVertex) + mesh.vertexCount;
        const std::uint64_t indexEnd =
            static_cast<std::uint64_t>(mesh.firstIndex) + mesh.indexCount;
        if (vertexEnd > vertexCount
            || indexEnd > model.geometry.indices.size()) {
            fail("A mesh geometry range is outside the model-wide arrays.");
        }
        RequireIndex(
            mesh.materialIndex,
            model.materials.size(),
            false,
            "Mesh material index",
            exporting);
        for (std::uint64_t index = mesh.firstIndex; index < indexEnd; ++index) {
            if (model.geometry.indices[static_cast<std::size_t>(index)]
                >= mesh.vertexCount) {
                fail("A mesh-local index is outside its vertex range.");
            }
        }
    }

    for (const ModelSubmeshAsset& submesh : model.asset.submeshes) {
        RequireIndex(
            submesh.meshIndex,
            model.meshes.size(),
            false,
            "Submesh mesh index",
            exporting);
    }
    for (const ModelNodeAsset& node : model.asset.nodes) {
        RequireIndex(
            node.parent,
            model.asset.nodes.size(),
            true,
            "Node parent index",
            exporting);
        RequireIndex(
            node.firstChild,
            model.asset.nodes.size(),
            true,
            "Node first-child index",
            exporting);
        RequireIndex(
            node.nextSibling,
            model.asset.nodes.size(),
            true,
            "Node next-sibling index",
            exporting);
        RequireIndex(
            node.transformIndex,
            model.asset.transforms.size(),
            false,
            "Node transform index",
            exporting);
        for (const std::uint32_t submeshIndex : node.submeshIndices) {
            RequireIndex(
                submeshIndex,
                model.asset.submeshes.size(),
                false,
                "Node submesh index",
                exporting);
        }
    }
    RequireIndex(
        model.asset.rootNode,
        model.asset.nodes.size(),
        model.asset.nodes.empty(),
        "Root node index",
        exporting);

    for (const ModelTextureInfo& info : model.textureInfos) {
        RequireIndex(
            info.textureIndex,
            model.textures.size(),
            true,
            "Texture-info texture index",
            exporting);
    }

    for (const ModelMaterial& material : model.materials) {
        if (static_cast<std::uint8_t>(material.alphaMode)
            > static_cast<std::uint8_t>(AlphaMode::Blend)) {
            fail("A material contains an invalid alpha mode.");
        }
        const std::array<std::uint32_t, 6> textureInfoIndices = {
            material.baseColorTexture,
            material.normalTexture,
            material.metallicRoughnessTexture,
            material.emissiveTexture,
            material.occlusionTexture,
            material.specularTexture,
        };
        for (const std::uint32_t index : textureInfoIndices) {
            RequireIndex(
                index,
                model.textureInfos.size(),
                true,
                "Material texture-info index",
                exporting);
        }
    }
}

std::string SanitizeFileStem(const std::string& source)
{
    std::string result;
    result.reserve(std::min<std::size_t>(source.size(), 48));
    for (const unsigned char character : source) {
        if (result.size() == 48) {
            break;
        }
        if (std::isalnum(character) != 0 || character == '-' || character == '_') {
            result.push_back(static_cast<char>(character));
        } else if (!result.empty() && result.back() != '_') {
            result.push_back('_');
        }
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    return result.empty() ? std::string("texture") : result;
}

struct SerializedTexture {
    std::string fileName;
    TextureComponentType componentType = TextureComponentType::UInt8;
};

std::vector<SerializedTexture> WriteTextures(
    const LoadedModel& model,
    const std::filesystem::path& destination)
{
    std::vector<SerializedTexture> serialized(model.textures.size());
    const std::string modelStem =
        SanitizeFileStem(destination.stem().u8string());

    for (const ModelTextureAsset& asset : model.textures) {
        if (!asset.texture && asset.loadError.empty()) {
            throw ModelExportError(
                "Texture '" + asset.name
                + "' has no loaded CPU data to export as KTX.");
        }
    }

    for (std::size_t index = 0; index < model.textures.size(); ++index) {
        const ModelTextureAsset& asset = model.textures[index];
        if (!asset.texture) {
            continue;
        }

        const std::string preferredStem =
            !asset.sourcePath.empty()
                ? asset.sourcePath.stem().u8string()
                : asset.name;
        std::ostringstream fileName;
        fileName << modelStem << "_texture_" << std::setw(4)
                 << std::setfill('0') << index << '_'
                 << SanitizeFileStem(preferredStem) << ".ktx2";

        SerializedTexture& record = serialized[index];
        record.fileName = fileName.str();
        record.componentType = asset.texture->info().format.componentType;
        texture::TextureConverter::SaveAsKtx2(
            asset.texture,
            destination.parent_path() / std::filesystem::u8path(record.fileName));
    }
    return serialized;
}

void WriteModel(BinaryWriter& writer,
                const LoadedModel& model,
                const std::vector<SerializedTexture>& textures)
{
    writer.WriteRaw(FileMagic.data(), FileMagic.size());
    writer.WriteU32(FileVersion);
    writer.WriteU32(EndianMarker);
    writer.WriteU32(0);
    writer.WriteU32(0);

    writer.WriteString(model.asset.name);

    writer.WriteU64(static_cast<std::uint64_t>(model.asset.submeshes.size()));
    for (const ModelSubmeshAsset& submesh : model.asset.submeshes) {
        writer.WriteU32(submesh.meshIndex);
        WriteAabb(writer, submesh.bounds);
    }

    writer.WriteU64(static_cast<std::uint64_t>(model.asset.nodes.size()));
    for (const ModelNodeAsset& node : model.asset.nodes) {
        writer.WriteString(node.name);
        writer.WriteU32(node.parent);
        writer.WriteU32(node.firstChild);
        writer.WriteU32(node.nextSibling);
        writer.WriteU32(node.transformIndex);
        WriteFloat3(writer, node.translation);
        WriteQuaternion(writer, node.rotation);
        WriteFloat3(writer, node.scale);
        WriteIndexVector(writer, node.submeshIndices);
    }

    writer.WriteRawVector(model.asset.transforms);
    WriteAabb(writer, model.asset.bounds);
    writer.WriteU32(model.asset.rootNode);

    writer.WriteRawVector(model.geometry.positions);
    writer.WriteRawVector(model.geometry.normals);
    writer.WriteRawVector(model.geometry.tangents);
    writer.WriteRawVector(model.geometry.texCoords0);
    writer.WriteRawVector(model.geometry.texCoords1);
    writer.WriteRawVector(model.geometry.colors);
    writer.WriteRawVector(model.geometry.indices);

    writer.WriteU64(static_cast<std::uint64_t>(model.meshes.size()));
    for (const ModelMeshAsset& mesh : model.meshes) {
        writer.WriteU32(mesh.firstVertex);
        writer.WriteU32(mesh.vertexCount);
        writer.WriteU32(mesh.firstIndex);
        writer.WriteU32(mesh.indexCount);
        writer.WriteU32(mesh.materialIndex);
        WriteAabb(writer, mesh.bounds);
    }

    writer.WriteU64(static_cast<std::uint64_t>(model.materials.size()));
    for (const ModelMaterial& material : model.materials) {
        writer.WriteString(material.name);
        WriteFloat4(writer, material.baseColorFactor);
        WriteFloat3(writer, material.emissiveFactor);
        writer.WriteFloat(material.metallicFactor);
        writer.WriteFloat(material.roughnessFactor);
        writer.WriteFloat(material.normalScale);
        writer.WriteFloat(material.occlusionStrength);
        writer.WriteU8(static_cast<std::uint8_t>(material.alphaMode));
        writer.WriteFloat(material.alphaCutoff);
        writer.WriteU8(material.doubleSided ? 1U : 0U);
        writer.WriteU8(material.unlit ? 1U : 0U);
        writer.WriteU32(material.baseColorTexture);
        writer.WriteU32(material.normalTexture);
        writer.WriteU32(material.metallicRoughnessTexture);
        writer.WriteU32(material.emissiveTexture);
        writer.WriteU32(material.occlusionTexture);
        writer.WriteU32(material.specularTexture);
    }

    writer.WriteU64(static_cast<std::uint64_t>(model.textureInfos.size()));
    for (const ModelTextureInfo& info : model.textureInfos) {
        writer.WriteU32(info.textureIndex);
        writer.WriteU32(info.texCoord);
        WriteFloat2(writer, info.offset);
        WriteFloat2(writer, info.scale);
        writer.WriteFloat(info.rotation);
    }

    writer.WriteU64(static_cast<std::uint64_t>(model.textures.size()));
    for (std::size_t index = 0; index < model.textures.size(); ++index) {
        const ModelTextureAsset& texture = model.textures[index];
        writer.WriteString(texture.name);
        writer.WriteString(textures[index].fileName);
        writer.WriteU8(
            static_cast<std::uint8_t>(textures[index].componentType));
        writer.WriteU8(texture.isSrgb ? 1U : 0U);
        writer.WriteString(texture.loadError);
    }

    writer.WriteU64(static_cast<std::uint64_t>(model.warnings.size()));
    for (const std::string& warning : model.warnings) {
        writer.WriteString(warning);
    }
}

void CheckBoolByte(std::uint8_t value, const char* description)
{
    if (value > 1) {
        throw ModelLoadError(
            std::string("VPG model contains an invalid ") + description + " flag.");
    }
}

std::shared_ptr<LoadedModel> ReadModel(
    BinaryReader& reader,
    const std::filesystem::path& sourcePath,
    std::vector<TextureComponentType>& textureTypes)
{
    std::array<std::uint8_t, FileMagic.size()> magic{};
    reader.ReadRaw(magic.data(), magic.size());
    if (magic != FileMagic) {
        throw ModelLoadError(
            "File '" + sourcePath.u8string() + "' is not a VPG model.");
    }
    const std::uint32_t version = reader.ReadU32();
    if (version != FileVersion) {
        throw ModelLoadError(
            "Unsupported VPG model version " + std::to_string(version)
            + " in '" + sourcePath.u8string() + "'.");
    }
    if (reader.ReadU32() != EndianMarker) {
        throw ModelLoadError(
            "VPG model '" + sourcePath.u8string() + "' has an invalid byte order marker.");
    }
    const std::uint32_t flags = reader.ReadU32();
    const std::uint32_t reserved = reader.ReadU32();
    if (flags != 0 || reserved != 0) {
        throw ModelLoadError(
            "VPG model '" + sourcePath.u8string() + "' uses unsupported format flags.");
    }

    auto model = std::make_shared<LoadedModel>();
    model->asset.name = reader.ReadString();
    model->asset.sourcePath = sourcePath;

    model->asset.submeshes.resize(reader.ReadCount(29));
    for (ModelSubmeshAsset& submesh : model->asset.submeshes) {
        submesh.meshIndex = reader.ReadU32();
        submesh.bounds = ReadAabb(reader);
    }

    model->asset.nodes.resize(reader.ReadCount(60));
    for (ModelNodeAsset& node : model->asset.nodes) {
        node.name = reader.ReadString();
        node.parent = reader.ReadU32();
        node.firstChild = reader.ReadU32();
        node.nextSibling = reader.ReadU32();
        node.transformIndex = reader.ReadU32();
        node.translation = ReadFloat3(reader);
        node.rotation = ReadQuaternion(reader);
        node.scale = ReadFloat3(reader);
        node.submeshIndices = ReadIndexVector(reader);
    }

    model->asset.transforms = reader.ReadRawVector<Matrix4>();
    model->asset.bounds = ReadAabb(reader);
    model->asset.rootNode = reader.ReadU32();

    model->geometry.positions = reader.ReadRawVector<Float3>();
    model->geometry.normals = reader.ReadRawVector<Float3>();
    model->geometry.tangents = reader.ReadRawVector<Float4>();
    model->geometry.texCoords0 = reader.ReadRawVector<Float2>();
    model->geometry.texCoords1 = reader.ReadRawVector<Float2>();
    model->geometry.colors = reader.ReadRawVector<std::uint32_t>();
    model->geometry.indices = reader.ReadRawVector<std::uint32_t>();

    model->meshes.resize(reader.ReadCount(45));
    for (ModelMeshAsset& mesh : model->meshes) {
        mesh.firstVertex = reader.ReadU32();
        mesh.vertexCount = reader.ReadU32();
        mesh.firstIndex = reader.ReadU32();
        mesh.indexCount = reader.ReadU32();
        mesh.materialIndex = reader.ReadU32();
        mesh.bounds = ReadAabb(reader);
    }

    model->materials.resize(reader.ReadCount(78));
    for (ModelMaterial& material : model->materials) {
        material.name = reader.ReadString();
        material.baseColorFactor = ReadFloat4(reader);
        material.emissiveFactor = ReadFloat3(reader);
        material.metallicFactor = reader.ReadFloat();
        material.roughnessFactor = reader.ReadFloat();
        material.normalScale = reader.ReadFloat();
        material.occlusionStrength = reader.ReadFloat();
        const std::uint8_t alphaMode = reader.ReadU8();
        if (alphaMode > static_cast<std::uint8_t>(AlphaMode::Blend)) {
            throw ModelLoadError("VPG model contains an invalid alpha mode.");
        }
        material.alphaMode = static_cast<AlphaMode>(alphaMode);
        material.alphaCutoff = reader.ReadFloat();
        const std::uint8_t doubleSided = reader.ReadU8();
        const std::uint8_t unlit = reader.ReadU8();
        CheckBoolByte(doubleSided, "double-sided");
        CheckBoolByte(unlit, "unlit");
        material.doubleSided = doubleSided != 0;
        material.unlit = unlit != 0;
        material.baseColorTexture = reader.ReadU32();
        material.normalTexture = reader.ReadU32();
        material.metallicRoughnessTexture = reader.ReadU32();
        material.emissiveTexture = reader.ReadU32();
        material.occlusionTexture = reader.ReadU32();
        material.specularTexture = reader.ReadU32();
    }

    model->textureInfos.resize(reader.ReadCount(28));
    for (ModelTextureInfo& info : model->textureInfos) {
        info.textureIndex = reader.ReadU32();
        info.texCoord = reader.ReadU32();
        info.offset = ReadFloat2(reader);
        info.scale = ReadFloat2(reader);
        info.rotation = reader.ReadFloat();
    }

    const std::size_t textureCount = reader.ReadCount(20);
    model->textures.resize(textureCount);
    textureTypes.resize(textureCount, TextureComponentType::UInt8);
    for (std::size_t index = 0; index < textureCount; ++index) {
        ModelTextureAsset& texture = model->textures[index];
        texture.name = reader.ReadString();
        const std::string fileName = reader.ReadString();
        const std::uint8_t componentType = reader.ReadU8();
        if (componentType
            > static_cast<std::uint8_t>(TextureComponentType::Float32)) {
            throw ModelLoadError(
                "VPG model contains an invalid texture component type.");
        }
        textureTypes[index] =
            static_cast<TextureComponentType>(componentType);
        const std::uint8_t isSrgb = reader.ReadU8();
        CheckBoolByte(isSrgb, "texture sRGB");
        texture.isSrgb = isSrgb != 0;
        texture.loadError = reader.ReadString();
        texture.embedded = false;
        texture.embeddedIndex = InvalidModelIndex;

        if (!fileName.empty()) {
            const std::filesystem::path relative =
                std::filesystem::u8path(fileName);
            if (relative.is_absolute() || relative.has_parent_path()
                || relative.filename() != relative) {
                throw ModelLoadError(
                    "VPG model texture references must be sibling filenames.");
            }
            texture.sourcePath = ResolvePath(sourcePath.parent_path() / relative);
        }
    }

    model->warnings.resize(reader.ReadCount(8));
    for (std::string& warning : model->warnings) {
        warning = reader.ReadString();
    }

    if (reader.remaining() != 0) {
        throw ModelLoadError(
            "VPG model '" + sourcePath.u8string() + "' contains unexpected trailing data.");
    }
    ValidateModel(*model, false);
    return model;
}

void LoadTextures(LoadedModel& model,
                  const std::vector<TextureComponentType>& textureTypes,
                  const ModelLoadOptions& options)
{
    if (!options.loadTextures) {
        return;
    }

    struct TextureResult {
        TextureHandle texture;
        std::string error;
    };

    std::vector<std::size_t> pending;
    std::string firstFailure;
    for (std::size_t index = 0; index < model.textures.size(); ++index) {
        ModelTextureAsset& asset = model.textures[index];
        if (!asset.sourcePath.empty()) {
            pending.push_back(index);
        } else if (!asset.loadError.empty() && firstFailure.empty()) {
            firstFailure = asset.loadError;
        }
    }

    std::vector<TextureResult> results(pending.size());
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        for (;;) {
            const std::size_t pendingIndex =
                next.fetch_add(1, std::memory_order_relaxed);
            if (pendingIndex >= pending.size()) {
                return;
            }
            const std::size_t textureIndex = pending[pendingIndex];
            ModelTextureAsset& asset = model.textures[textureIndex];
            TextureResult& result = results[pendingIndex];
            try {
                TextureLoadOptions textureOptions = options.textureLoadOptions;
                textureOptions.outputComponentType = textureTypes[textureIndex];
                result.texture =
                    options.cacheExternalTextures
                        ? TextureLoader::LoadCached(
                              asset.sourcePath, textureOptions)
                        : TextureLoader::Load(
                              asset.sourcePath, textureOptions);
            } catch (const std::exception& error) {
                result.error =
                    "Failed to load VPG model texture '" + asset.name
                    + "' from '" + asset.sourcePath.u8string() + "': "
                    + error.what();
            } catch (...) {
                result.error =
                    "Failed to load VPG model texture '" + asset.name
                    + "': unknown error";
            }
        }
    };

    std::size_t workerCount = options.maxTextureLoadConcurrency;
    if (workerCount == 0) {
        workerCount = std::thread::hardware_concurrency();
        if (workerCount == 0) {
            workerCount = 1;
        }
    }
    workerCount = std::min(workerCount, pending.size());
    std::vector<std::thread> workers;
    workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
    for (std::size_t index = 1; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    if (workerCount != 0) {
        worker();
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    for (std::size_t index = 0; index < pending.size(); ++index) {
        ModelTextureAsset& asset = model.textures[pending[index]];
        TextureResult& result = results[index];
        if (result.texture) {
            asset.texture = std::move(result.texture);
            asset.loadError.clear();
        } else {
            asset.loadError = std::move(result.error);
            model.warnings.push_back(asset.loadError);
            if (firstFailure.empty()) {
                firstFailure = asset.loadError;
            }
        }
    }

    if (options.failOnMissingTextures && !firstFailure.empty()) {
        throw ModelLoadError(firstFailure);
    }
}

} // namespace

bool IsVpgModelPath(const std::filesystem::path& path) noexcept
{
    try {
        return LowerAscii(path.extension().u8string()) == ".vpgmodel";
    } catch (...) {
        return false;
    }
}

void SaveVpgModel(const LoadedModel& model,
                  const std::filesystem::path& destination)
{
    if (!IsLittleEndian()) {
        throw ModelExportError(
            "VPG model export is currently supported only on little-endian CPUs.");
    }
    if (destination.empty()) {
        throw ModelExportError("Model export destination is empty.");
    }
    if (!IsVpgModelPath(destination)) {
        throw ModelExportError(
            "Model export destination must use the .vpgmodel extension.");
    }

    ValidateModel(model, true);
    if (!destination.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error) {
            throw ModelExportError(
                "Unable to create model output directory '"
                + destination.parent_path().u8string() + "': "
                + error.message());
        }
    }

    const std::vector<SerializedTexture> textures =
        WriteTextures(model, destination);
    BinaryWriter writer(destination);
    WriteModel(writer, model, textures);
    writer.Finish();
}

ModelHandle LoadVpgModel(const std::filesystem::path& path,
                         const ModelLoadOptions& options)
{
    if (!IsLittleEndian()) {
        throw ModelLoadError(
            "VPG model loading is currently supported only on little-endian CPUs.");
    }

    const std::filesystem::path sourcePath = ResolvePath(path);
    try {
        BinaryReader reader(sourcePath);
        std::vector<TextureComponentType> textureTypes;
        std::shared_ptr<LoadedModel> model =
            ReadModel(reader, sourcePath, textureTypes);
        LoadTextures(*model, textureTypes, options);
        return model;
    } catch (const ModelLoadError&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelLoadError(
            "Failed to load VPG model '" + sourcePath.u8string() + "': "
            + error.what());
    }
}

} // namespace vpgloader::detail
