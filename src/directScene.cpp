#include "directScene.h"

#include "commandProtocol.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/animMapper.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShapeQuery.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/topology.h>
#include <pxr/usd/usdSkel/utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace babylon::usd_importer {
namespace {

constexpr size_t kMaxInfluences = 8;

class BufferWriter
{
public:
    uint32_t size() const { return static_cast<uint32_t>(bytes.size()); }

    void align(uint32_t alignment = 4)
    {
        while (bytes.size() % alignment != 0) {
            bytes.push_back(0);
        }
    }

    void u16(uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }

    void f32(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void patchU32(uint32_t offset, uint32_t value)
    {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
    }

    uint32_t appendBytes(const void* source, size_t byteCount, uint32_t alignment = 4)
    {
        align(alignment);
        const uint32_t offset = size();
        if (byteCount == 0) {
            return offset;
        }
        const auto* first = static_cast<const uint8_t*>(source);
        bytes.insert(bytes.end(), first, first + byteCount);
        return offset;
    }

    uint32_t appendString(const std::string& value)
    {
        return appendBytes(value.data(), value.size(), 1);
    }

    std::vector<uint8_t> bytes;
};

class CommandWriter
{
public:
    CommandWriter()
    {
        buffer.u32(kCommandMagic);
        buffer.u16(kProtocolVersion);
        buffer.u16(0);
        buffer.u32(0);
        buffer.u32(0);
    }

    uint32_t begin(Command command, uint16_t flags = 0)
    {
        buffer.u16(static_cast<uint16_t>(command));
        buffer.u16(flags);
        const uint32_t lengthOffset = buffer.size();
        buffer.u32(0);
        return lengthOffset;
    }

    void end(uint32_t lengthOffset)
    {
        const uint32_t payloadStart = lengthOffset + 4;
        buffer.patchU32(lengthOffset, buffer.size() - payloadStart);
        ++commandCount;
    }

    std::vector<uint8_t> finish()
    {
        buffer.patchU32(8, commandCount);
        return std::move(buffer.bytes);
    }

    BufferWriter buffer;
    uint32_t commandCount = 0;
};

struct TextureData
{
    SdfAssetPath asset;
    SdfPath sourceShader;
    std::string name;
    TfToken channel;
    TfToken sourceColorSpace = TfToken("auto");
    TfToken wrapS = TfToken("repeat");
    TfToken wrapT = TfToken("repeat");
    GfVec2f uvScale = GfVec2f(1.0f);
    GfVec2f uvTranslation = GfVec2f(0.0f);
    float rotation = 0.0f;
    GfVec4f valueScale = GfVec4f(1.0f);
    GfVec4f valueBias = GfVec4f(0.0f);
    int uvIndex = 0;
    TfToken uvName = TfToken("st");

    explicit operator bool() const { return !asset.GetAssetPath().empty(); }
};

struct MaterialData
{
    std::string name = "Default USD material";
    GfVec3f baseColor = GfVec3f(1.0f);
    GfVec3f emissive = GfVec3f(0.0f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;
    float normalScale = 1.0f;
    float alphaCutoff = 0.0f;
    bool unlit = false;
    bool doubleSided = false;
    TextureData baseTexture;
    TextureData opacityTexture;
    TextureData normalTexture;
    TextureData metallicTexture;
    TextureData roughnessTexture;
    TextureData occlusionTexture;
    TextureData emissiveTexture;
    uint32_t baseChannel = kMissingOffset;
    uint32_t opacityChannel = kMissingOffset;
    uint32_t normalChannel = kMissingOffset;
    uint32_t metallicChannel = kMissingOffset;
    uint32_t roughnessChannel = kMissingOffset;
    uint32_t occlusionChannel = kMissingOffset;
    uint32_t emissiveChannel = kMissingOffset;
};

struct NodeAnimation
{
    std::vector<float> times;
    std::vector<GfMatrix4f> matrices;
};

struct NodeData
{
    SdfPath path;
    std::string name;
    uint32_t parentId = kMissingOffset;
    GfMatrix4d localTransform = GfMatrix4d(1.0);
    NodeAnimation animation;
};

struct SubmeshData
{
    uint32_t materialId = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

struct MorphTargetAnimation
{
    std::vector<float> times;
    std::vector<float> influences;
};

struct MorphTargetData
{
    std::string name;
    float influence = 0.0f;
    std::vector<GfVec3f> pointOffsets;
    std::vector<GfVec3f> normalOffsets;
    std::vector<GfVec3f> positions;
    std::vector<GfVec3f> normals;
    MorphTargetAnimation animation;
};

struct SourceMorphTarget
{
    MorphTargetData target;
    size_t subShapeIndex = 0;
    std::vector<GfVec3f> pointOffsets;
    std::vector<GfVec3f> normalOffsets;
};

using JointSet = std::array<uint16_t, kMaxInfluences>;
using WeightSet = std::array<float, kMaxInfluences>;

struct MeshData
{
    std::string name;
    bool doubleSided = false;
    bool leftHanded = false;
    uint32_t skeletonId = kMissingOffset;
    uint32_t influenceCount = 0;
    std::vector<GfVec3f> positions;
    std::vector<GfVec3f> normals;
    std::vector<GfVec2f> uvs;
    std::vector<GfVec4f> colors;
    std::vector<JointSet> joints;
    std::vector<WeightSet> weights;
    std::vector<uint32_t> sourcePointIndices;
    std::vector<uint32_t> indices;
    std::vector<SubmeshData> submeshes;
    std::vector<MorphTargetData> morphTargets;
    std::vector<uint32_t> nodeIds;
};

struct AnalyticPrimitiveData
{
    std::string name;
    std::vector<uint32_t> nodeIds;
    uint32_t materialId = 0;
    uint32_t flags = 0;
    AnalyticPrimitiveType type = AnalyticPrimitiveType::Cube;
    PrimitiveAxis axis = PrimitiveAxis::Y;
    float sizeOrRadius = 1.0f;
    float height = 0.0f;
    uint32_t tessellation = 32;
};

struct SkeletonAnimation
{
    std::vector<float> times;
    std::vector<VtMatrix4dArray> localTransforms;
};

struct SkeletonData
{
    std::string name;
    VtTokenArray joints;
    VtIntArray parents;
    VtMatrix4dArray restTransforms;
    VtMatrix4dArray bindTransforms;
    SkeletonAnimation animation;
};

struct ThinInstanceData
{
    size_t sourceIndex = 0;
    bool analyticSource = false;
    std::vector<GfMatrix4d> transforms;
};

struct SkinBinding
{
    uint32_t skeletonId = kMissingOffset;
    SdfPath skeletonPath;
    UsdSkelSkinningQuery query;
    std::vector<uint16_t> jointMap;
};

struct SceneData
{
    TfToken upAxis = UsdGeomTokens->y;
    double metersPerUnit = 1.0;
    double timeCodesPerSecond = 24.0;
    std::vector<MaterialData> materials{ MaterialData{} };
    std::unordered_map<std::string, uint32_t> materialIds;
    std::vector<NodeData> nodes;
    std::unordered_map<std::string, uint32_t> nodeIds;
    std::vector<MeshData> meshes;
    std::unordered_map<std::string, size_t> meshIds;
    std::unordered_map<std::string, bool> meshWinding;
    std::vector<ThinInstanceData> thinInstances;
    std::vector<AnalyticPrimitiveData> analyticPrimitives;
    std::unordered_map<std::string, size_t> analyticPrimitiveIds;
    std::unordered_map<std::string, uint32_t> analyticMaterialIds;
    std::vector<SkeletonData> skeletons;
    std::unordered_map<std::string, uint32_t> skeletonIds;
    std::unordered_map<std::string, SkinBinding> skinBindings;
    UsdShadeMaterialBindingAPI::BindingsCache bindingsCache;
    UsdShadeMaterialBindingAPI::CollectionQueryCache collectionQueryCache;
    UsdSkelCache skelCache;
};

template<typename T>
struct PrimvarData
{
    VtArray<T> values;
    TfToken interpolation;

    explicit operator bool() const { return !values.empty(); }

    const T* value(size_t faceIndex, size_t cornerIndex, size_t pointIndex) const
    {
        size_t index = 0;
        if (interpolation == UsdGeomTokens->constant) {
            index = 0;
        } else if (interpolation == UsdGeomTokens->uniform) {
            index = faceIndex;
        } else if (interpolation == UsdGeomTokens->faceVarying) {
            index = cornerIndex;
        } else {
            index = pointIndex;
        }
        return index < values.size() ? &values[index] : nullptr;
    }
};

std::string
displayName(const UsdPrim& prim, const char* fallback)
{
    const std::string authored = prim.GetDisplayName();
    if (!authored.empty()) {
        return authored;
    }
    const std::string name = prim.GetName().GetString();
    return name.empty() ? std::string(fallback) : name;
}

uint32_t
appendString(BufferWriter& data, const std::string& value, uint32_t& length)
{
    length = static_cast<uint32_t>(value.size());
    return data.appendString(value);
}

uint32_t
appendMatrix(BufferWriter& data, const GfMatrix4d& matrix)
{
    data.align();
    const uint32_t offset = data.size();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            data.f32(static_cast<float>(matrix[row][column]));
        }
    }
    return offset;
}

template<typename T>
bool
getInputValue(const UsdShadeShader& shader, const char* name, T& value)
{
    const UsdShadeInput input = shader.GetInput(TfToken(name));
    return input && input.Get(&value);
}

bool
getTokenInput(const UsdShadeShader& shader, const char* name, TfToken& value)
{
    const UsdShadeInput input = shader.GetInput(TfToken(name));
    if (!input) {
        return false;
    }
    if (input.Get(&value)) {
        return true;
    }
    std::string text;
    if (input.Get(&text)) {
        value = TfToken(text);
        return true;
    }
    return false;
}

UsdShadeShader
connectedShader(const UsdShadeInput& input, TfToken* outputName = nullptr)
{
    if (!input) {
        return UsdShadeShader();
    }
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType = UsdShadeAttributeType::Invalid;
    if (!input.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return UsdShadeShader();
    }
    if (outputName) {
        *outputName = sourceName;
    }
    return UsdShadeShader(source);
}

void
readUvConnection(const UsdShadeShader& textureShader, TextureData& texture)
{
    UsdShadeShader source = connectedShader(textureShader.GetInput(TfToken("st")));
    if (!source) {
        return;
    }
    TfToken sourceId;
    source.GetShaderId(&sourceId);
    if (sourceId == TfToken("UsdTransform2d")) {
        getInputValue(source, "scale", texture.uvScale);
        getInputValue(source, "translation", texture.uvTranslation);
        float rotationDegrees = 0.0f;
        if (getInputValue(source, "rotation", rotationDegrees)) {
            texture.rotation =
              rotationDegrees * static_cast<float>(3.14159265358979323846 / 180.0);
        }
        source = connectedShader(source.GetInput(TfToken("in")));
        if (!source) {
            return;
        }
        source.GetShaderId(&sourceId);
    }
    if (sourceId == TfToken("UsdPrimvarReader_float2")) {
        TfToken varname;
        if (!getTokenInput(source, "varname", varname)) {
            return;
        }
        texture.uvName = varname;
        texture.uvIndex = 0;
    } else {
        TF_WARN("Ignoring unsupported UV source shader '%s' at <%s>.",
                sourceId.GetText(),
                source.GetPath().GetText());
    }
}

TfToken
normalizedSourceColorSpace(const TfToken& authored)
{
    std::string normalized = TfStringToLower(authored.GetString());
    normalized.erase(std::remove_if(normalized.begin(),
                                    normalized.end(),
                                    [](char value) {
                                        return value == '-' || value == '_';
                                    }),
                     normalized.end());
    if (normalized == "raw") {
        return TfToken("raw");
    }
    if (normalized == "srgb") {
        return TfToken("sRGB");
    }
    return TfToken("auto");
}

TextureData
readTexture(const UsdShadeInput& materialInput)
{
    TextureData result;
    TfToken outputName;
    const UsdShadeShader shader = connectedShader(materialInput, &outputName);
    if (!shader) {
        return result;
    }
    TfToken shaderId;
    shader.GetShaderId(&shaderId);
    if (shaderId != TfToken("UsdUVTexture")) {
        TF_WARN("Ignoring texture connection from unsupported shader '%s' at <%s>.",
                shaderId.GetText(),
                shader.GetPath().GetText());
        return result;
    }
    if (!getInputValue(shader, "file", result.asset) || result.asset.GetAssetPath().empty()) {
        return {};
    }
    result.sourceShader = shader.GetPath();
    result.name = TfGetBaseName(result.asset.GetAssetPath());
    result.channel = outputName;
    TfToken sourceColorSpace;
    if (getTokenInput(shader, "sourceColorSpace", sourceColorSpace)) {
        result.sourceColorSpace = normalizedSourceColorSpace(sourceColorSpace);
    }
    getInputValue(shader, "scale", result.valueScale);
    getInputValue(shader, "bias", result.valueBias);
    getTokenInput(shader, "wrapS", result.wrapS);
    getTokenInput(shader, "wrapT", result.wrapT);
    readUvConnection(shader, result);
    return result;
}

uint32_t
textureChannel(const TfToken& channel)
{
    if (channel == TfToken("r")) {
        return 0;
    }
    if (channel == TfToken("g")) {
        return 1;
    }
    if (channel == TfToken("b")) {
        return 2;
    }
    if (channel == TfToken("a")) {
        return 3;
    }
    if (channel == TfToken("rgb")) {
        return 4;
    }
    return kMissingOffset;
}

std::optional<MaterialData>
readMaterial(const UsdShadeMaterial& material)
{
    MaterialData result;
    result.name = displayName(material.GetPrim(), "Material");
    const UsdShadeShader shader = material.ComputeSurfaceSource();
    if (!shader) {
        return std::nullopt;
    }
    TfToken shaderId;
    shader.GetShaderId(&shaderId);
    if (shaderId != TfToken("UsdPreviewSurface")) {
        TF_WARN("Using the default material for unsupported surface shader '%s' at <%s>.",
                shaderId.GetText(),
                material.GetPath().GetText());
        return std::nullopt;
    }

    getInputValue(shader, "diffuseColor", result.baseColor);
    getInputValue(shader, "emissiveColor", result.emissive);
    getInputValue(shader, "metallic", result.metallic);
    getInputValue(shader, "roughness", result.roughness);
    getInputValue(shader, "opacity", result.opacity);
    getInputValue(shader, "opacityThreshold", result.alphaCutoff);

    const UsdShadeInput baseInput = shader.GetInput(TfToken("diffuseColor"));
    const UsdShadeInput opacityInput = shader.GetInput(TfToken("opacity"));
    const UsdShadeInput metallicInput = shader.GetInput(TfToken("metallic"));
    const UsdShadeInput roughnessInput = shader.GetInput(TfToken("roughness"));
    const UsdShadeInput occlusionInput = shader.GetInput(TfToken("occlusion"));

    auto readBinding = [&](const UsdShadeInput& input,
                           TextureData& texture,
                           uint32_t& channel,
                           const char* slot) {
        texture = readTexture(input);
        if (!texture) {
            return false;
        }
        channel = textureChannel(texture.channel);
        if (channel != kMissingOffset) {
            return true;
        }
        TF_WARN("Ignoring %s texture on material <%s>: unsupported output '%s'.",
                slot,
                material.GetPath().GetText(),
                texture.channel.GetText());
        texture = {};
        return false;
    };

    readBinding(baseInput,
                result.baseTexture,
                result.baseChannel,
                "base-color");
    readBinding(opacityInput,
                result.opacityTexture,
                result.opacityChannel,
                "opacity");
    readBinding(shader.GetInput(TfToken("normal")),
                result.normalTexture,
                result.normalChannel,
                "normal");
    readBinding(metallicInput,
                result.metallicTexture,
                result.metallicChannel,
                "metallic");
    readBinding(roughnessInput,
                result.roughnessTexture,
                result.roughnessChannel,
                "roughness");
    readBinding(occlusionInput,
                result.occlusionTexture,
                result.occlusionChannel,
                "occlusion");
    readBinding(shader.GetInput(TfToken("emissiveColor")),
                result.emissiveTexture,
                result.emissiveChannel,
                "emissive");

    TfToken materialUv;
    auto validateUv = [&](TextureData& texture,
                          uint32_t& channel,
                          const char* slot) {
        if (!texture) {
            return true;
        }
        if (materialUv.IsEmpty()) {
            materialUv = texture.uvName;
            return true;
        }
        if (materialUv == texture.uvName) {
            return true;
        }
        TF_WARN("Ignoring %s texture on material <%s>: it uses UV primvar '%s', while the "
                "material's first texture uses '%s'.",
                slot,
                material.GetPath().GetText(),
                texture.uvName.GetText(),
                materialUv.GetText());
        texture = {};
        channel = kMissingOffset;
        return false;
    };
    validateUv(result.baseTexture,
               result.baseChannel,
               "base-color");
    validateUv(result.opacityTexture,
               result.opacityChannel,
               "opacity");
    validateUv(result.normalTexture,
               result.normalChannel,
               "normal");
    validateUv(result.metallicTexture,
               result.metallicChannel,
               "metallic");
    validateUv(result.roughnessTexture,
               result.roughnessChannel,
               "roughness");
    validateUv(result.occlusionTexture,
               result.occlusionChannel,
               "occlusion");
    validateUv(result.emissiveTexture,
               result.emissiveChannel,
               "emissive");

    result.unlit = TfStringToLower(shaderId.GetString()).find("unlit") != std::string::npos;
    return result;
}

uint32_t
materialId(SceneData& scene, const UsdShadeMaterial& material)
{
    if (!material) {
        return 0;
    }
    const std::string key = material.GetPath().GetString();
    const auto found = scene.materialIds.find(key);
    if (found != scene.materialIds.end()) {
        return found->second;
    }
    const std::optional<MaterialData> parsed = readMaterial(material);
    if (!parsed) {
        scene.materialIds.emplace(key, 0);
        return 0;
    }
    const uint32_t id = static_cast<uint32_t>(scene.materials.size());
    scene.materialIds.emplace(key, id);
    scene.materials.push_back(*parsed);
    return id;
}

uint32_t
boundMaterialId(SceneData& scene, const UsdPrim& prim)
{
    const UsdShadeMaterial material =
      UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(&scene.bindingsCache,
                                                            &scene.collectionQueryCache);
    return materialId(scene, material);
}

template<typename T>
PrimvarData<T>
readPrimvar(const UsdGeomPrimvar& primvar,
            const UsdTimeCode time = UsdTimeCode::Default())
{
    PrimvarData<T> result;
    if (!primvar || !primvar.ComputeFlattened(&result.values, time)) {
        return {};
    }
    result.interpolation = primvar.GetInterpolation();
    if (result.interpolation.IsEmpty()) {
        result.interpolation = UsdGeomTokens->constant;
    }
    return result;
}

template<typename T>
PrimvarData<T>
readAuthoredPrimvar(const UsdGeomPrimvar& primvar,
                    const UsdTimeCode time = UsdTimeCode::Default())
{
    return primvar && primvar.HasAuthoredValue() ? readPrimvar<T>(primvar, time)
                                                 : PrimvarData<T>{};
}

PrimvarData<GfVec3f>
readNormals(const UsdGeomMesh& mesh,
            const UsdTimeCode time = UsdTimeCode::Default())
{
    const UsdGeomPrimvar authored =
      UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(TfToken("normals"));
    PrimvarData<GfVec3f> result = readPrimvar<GfVec3f>(authored, time);
    if (result) {
        return result;
    }
    mesh.GetNormalsAttr().Get(&result.values, time);
    result.interpolation = mesh.GetNormalsInterpolation();
    return result;
}

PrimvarData<GfVec2f>
readUvs(const UsdGeomMesh& mesh,
        const TfToken& requestedName,
        const UsdTimeCode time = UsdTimeCode::Default())
{
    const UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
    if (!requestedName.IsEmpty()) {
        PrimvarData<GfVec2f> requested =
          readPrimvar<GfVec2f>(
            primvars.FindPrimvarWithInheritance(requestedName), time);
        if (requested) {
            return requested;
        }
        TF_WARN("Mesh <%s> does not provide requested UV primvar '%s'.",
                mesh.GetPath().GetText(),
                requestedName.GetText());
        return {};
    }
    static const std::array<TfToken, 3> preferred = {
        TfToken("st"), TfToken("uv"), TfToken("UVMap")
    };
    for (const TfToken& name : preferred) {
        PrimvarData<GfVec2f> result =
          readPrimvar<GfVec2f>(primvars.GetPrimvar(name), time);
        if (result) {
            return result;
        }
    }
    for (const UsdGeomPrimvar& primvar : primvars.GetPrimvarsWithValues()) {
        PrimvarData<GfVec2f> result = readPrimvar<GfVec2f>(primvar, time);
        if (result) {
            return result;
        }
    }
    return {};
}

bool
materialUvName(const SceneData& scene,
               const std::vector<uint32_t>& materialIds,
               TfToken& selected)
{
    std::vector<bool> visited(scene.materials.size(), false);
    for (const uint32_t materialId : materialIds) {
        if (materialId >= scene.materials.size() || visited[materialId]) {
            continue;
        }
        visited[materialId] = true;
        const MaterialData& material = scene.materials[materialId];
        const std::array<const TextureData*, 7> textures = {
            &material.baseTexture,
            &material.opacityTexture,
            &material.normalTexture,
            &material.metallicTexture,
            &material.roughnessTexture,
            &material.occlusionTexture,
            &material.emissiveTexture,
        };
        for (const TextureData* texture : textures) {
            if (!*texture) {
                continue;
            }
            if (selected.IsEmpty()) {
                selected = texture->uvName;
            } else if (selected != texture->uvName) {
                TF_WARN("Cannot emit mesh using material '%s': its UV primvar '%s' conflicts "
                        "with '%s' required by another bound material.",
                        material.name.c_str(),
                        texture->uvName.GetText(),
                        selected.GetText());
                return false;
            }
        }
    }
    return true;
}

std::vector<GfVec3f>
computeSmoothNormals(const VtVec3fArray& points,
                     const VtIntArray& faceCounts,
                     const VtIntArray& faceIndices,
                     bool leftHanded)
{
    std::vector<GfVec3f> normals(points.size(), GfVec3f(0.0f));
    size_t cornerOffset = 0;
    for (const int count : faceCounts) {
        if (count < 3 || cornerOffset + static_cast<size_t>(count) > faceIndices.size()) {
            cornerOffset += std::max(count, 0);
            continue;
        }
        const int first = faceIndices[cornerOffset];
        for (int corner = 1; corner + 1 < count; ++corner) {
            const int second = faceIndices[cornerOffset + corner];
            const int third = faceIndices[cornerOffset + corner + 1];
            if (first < 0 || second < 0 || third < 0 ||
                static_cast<size_t>(first) >= points.size() ||
                static_cast<size_t>(second) >= points.size() ||
                static_cast<size_t>(third) >= points.size()) {
                continue;
            }
            GfVec3f faceNormal =
              GfCross(points[second] - points[first], points[third] - points[first]);
            if (leftHanded) {
                faceNormal = -faceNormal;
            }
            normals[first] += faceNormal;
            normals[second] += faceNormal;
            normals[third] += faceNormal;
        }
        cornerOffset += static_cast<size_t>(count);
    }
    for (GfVec3f& normal : normals) {
        if (normal.Normalize() <= std::numeric_limits<float>::epsilon()) {
            normal = GfVec3f(0.0f, 1.0f, 0.0f);
        }
    }
    return normals;
}

std::optional<bool>
inferLeftHandedWinding(const VtVec3fArray& points,
                       const VtIntArray& faceCounts,
                       const VtIntArray& faceIndices,
                       const PrimvarData<GfVec3f>& normals)
{
    if (!normals) {
        return std::nullopt;
    }
    constexpr size_t maxSamples = 4096;
    size_t positive = 0;
    size_t negative = 0;
    size_t cornerOffset = 0;
    for (size_t face = 0; face < faceCounts.size() &&
                          positive + negative < maxSamples;
         ++face) {
        const int count = faceCounts[face];
        for (int triangle = 0;
             triangle < count - 2 && positive + negative < maxSamples;
             ++triangle) {
            const std::array<size_t, 3> corners = {
                cornerOffset,
                cornerOffset + static_cast<size_t>(triangle + 1),
                cornerOffset + static_cast<size_t>(triangle + 2),
            };
            std::array<size_t, 3> pointIndices;
            bool valid = true;
            GfVec3f authoredNormal(0.0f);
            for (size_t vertex = 0; vertex < corners.size(); ++vertex) {
                const int pointIndex = faceIndices[corners[vertex]];
                if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= points.size()) {
                    valid = false;
                    break;
                }
                pointIndices[vertex] = static_cast<size_t>(pointIndex);
                const GfVec3f* normal =
                  normals.value(face, corners[vertex], pointIndices[vertex]);
                if (!normal) {
                    valid = false;
                    break;
                }
                authoredNormal += *normal;
            }
            if (!valid || authoredNormal.Normalize() <=
                            std::numeric_limits<float>::epsilon()) {
                continue;
            }
            GfVec3f geometricNormal =
              GfCross(points[pointIndices[1]] - points[pointIndices[0]],
                      points[pointIndices[2]] - points[pointIndices[0]]);
            if (geometricNormal.Normalize() <= std::numeric_limits<float>::epsilon()) {
                continue;
            }
            const float agreement = GfDot(geometricNormal, authoredNormal);
            if (agreement > 0.1f) {
                ++positive;
            } else if (agreement < -0.1f) {
                ++negative;
            }
        }
        cornerOffset += static_cast<size_t>(count);
    }
    const size_t considered = positive + negative;
    if (considered == 0 || std::max(positive, negative) * 4 < considered * 3) {
        return std::nullopt;
    }
    return negative > positive;
}

template<typename T>
void
hashValue(uint64_t& hash, const T& value)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t index = 0; index < sizeof(T); ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

uint64_t
hashVertex(const MeshData& mesh, size_t index)
{
    uint64_t hash = 1469598103934665603ull;
    hashValue(hash, mesh.positions[index]);
    if (mesh.normals.size() == mesh.positions.size()) {
        hashValue(hash, mesh.normals[index]);
    }
    if (mesh.uvs.size() == mesh.positions.size()) {
        hashValue(hash, mesh.uvs[index]);
    }
    if (mesh.colors.size() == mesh.positions.size()) {
        hashValue(hash, mesh.colors[index]);
    }
    if (mesh.joints.size() == mesh.positions.size()) {
        hashValue(hash, mesh.joints[index]);
    }
    if (mesh.weights.size() == mesh.positions.size()) {
        hashValue(hash, mesh.weights[index]);
    }
    if (mesh.sourcePointIndices.size() == mesh.positions.size()) {
        hashValue(hash, mesh.sourcePointIndices[index]);
    }
    return hash;
}

template<typename T>
bool
vertexValueEqual(const std::vector<T>& values,
                 size_t vertexCount,
                 size_t left,
                 size_t right)
{
    return values.size() != vertexCount || values[left] == values[right];
}

bool
verticesEqual(const MeshData& mesh, size_t left, size_t right)
{
    const size_t count = mesh.positions.size();
    if (mesh.positions[left] != mesh.positions[right] ||
        !vertexValueEqual(mesh.normals, count, left, right) ||
        !vertexValueEqual(mesh.uvs, count, left, right) ||
        !vertexValueEqual(mesh.colors, count, left, right) ||
        !vertexValueEqual(mesh.joints, count, left, right) ||
        !vertexValueEqual(mesh.weights, count, left, right) ||
        !vertexValueEqual(mesh.sourcePointIndices, count, left, right)) {
        return false;
    }
    return true;
}

template<typename T>
void
appendVertexValue(std::vector<T>& destination,
                  const std::vector<T>& source,
                  size_t vertexCount,
                  size_t index)
{
    if (source.size() == vertexCount) {
        destination.push_back(source[index]);
    }
}

bool
optimizeMesh(MeshData& mesh)
{
    const size_t sourceVertexCount = mesh.positions.size();
    if (sourceVertexCount == 0 || mesh.indices.empty()) {
        return false;
    }

    MeshData welded;
    welded.name = mesh.name;
    welded.doubleSided = mesh.doubleSided;
    welded.leftHanded = mesh.leftHanded;
    welded.skeletonId = mesh.skeletonId;
    welded.influenceCount = mesh.influenceCount;
    welded.submeshes = mesh.submeshes;
    welded.nodeIds = mesh.nodeIds;
    welded.positions.reserve(sourceVertexCount);
    welded.normals.reserve(mesh.normals.size());
    welded.uvs.reserve(mesh.uvs.size());
    welded.colors.reserve(mesh.colors.size());
    welded.joints.reserve(mesh.joints.size());
    welded.weights.reserve(mesh.weights.size());
    welded.sourcePointIndices.reserve(mesh.sourcePointIndices.size());
    welded.indices.reserve(mesh.indices.size());
    welded.morphTargets.reserve(mesh.morphTargets.size());
    for (MorphTargetData& source : mesh.morphTargets) {
        MorphTargetData target;
        target.name = source.name;
        target.influence = source.influence;
        target.animation = source.animation;
        target.pointOffsets = std::move(source.pointOffsets);
        target.normalOffsets = std::move(source.normalOffsets);
        welded.morphTargets.push_back(std::move(target));
    }

    struct Candidate
    {
        uint32_t sourceIndex;
        uint32_t weldedIndex;
        uint32_t next;
    };
    constexpr uint32_t noCandidate = std::numeric_limits<uint32_t>::max();
    std::unordered_map<uint64_t, uint32_t> bucketHeads;
    bucketHeads.reserve(sourceVertexCount);
    std::vector<Candidate> candidates;
    candidates.reserve(sourceVertexCount);

    for (const uint32_t sourceIndex : mesh.indices) {
        if (sourceIndex >= sourceVertexCount) {
            return false;
        }
        const uint64_t hash = hashVertex(mesh, sourceIndex);
        const auto head = bucketHeads.find(hash);
        bool weldedExisting = false;
        for (uint32_t candidateIndex =
               head == bucketHeads.end() ? noCandidate : head->second;
             candidateIndex != noCandidate;
             candidateIndex = candidates[candidateIndex].next) {
            const Candidate& candidate = candidates[candidateIndex];
            if (verticesEqual(mesh, sourceIndex, candidate.sourceIndex)) {
                welded.indices.push_back(candidate.weldedIndex);
                weldedExisting = true;
                break;
            }
        }
        if (weldedExisting) {
            continue;
        }

        const uint32_t weldedIndex =
          static_cast<uint32_t>(welded.positions.size());
        const uint32_t next =
          head == bucketHeads.end() ? noCandidate : head->second;
        const uint32_t candidateIndex =
          static_cast<uint32_t>(candidates.size());
        candidates.push_back({ sourceIndex, weldedIndex, next });
        bucketHeads[hash] = candidateIndex;
        welded.positions.push_back(mesh.positions[sourceIndex]);
        appendVertexValue(
          welded.normals, mesh.normals, sourceVertexCount, sourceIndex);
        appendVertexValue(welded.uvs, mesh.uvs, sourceVertexCount, sourceIndex);
        appendVertexValue(
          welded.colors, mesh.colors, sourceVertexCount, sourceIndex);
        appendVertexValue(
          welded.joints, mesh.joints, sourceVertexCount, sourceIndex);
        appendVertexValue(
          welded.weights, mesh.weights, sourceVertexCount, sourceIndex);
        appendVertexValue(welded.sourcePointIndices,
                          mesh.sourcePointIndices,
                          sourceVertexCount,
                          sourceIndex);
        welded.indices.push_back(weldedIndex);
    }

    for (MorphTargetData& target : welded.morphTargets) {
        target.positions.reserve(welded.positions.size());
        if (!target.normalOffsets.empty()) {
            target.normals.reserve(welded.normals.size());
        }
        for (size_t vertexIndex = 0;
             vertexIndex < welded.positions.size();
             ++vertexIndex) {
            const uint32_t pointIndex =
              welded.sourcePointIndices[vertexIndex];
            target.positions.push_back(
              welded.positions[vertexIndex] + target.pointOffsets[pointIndex]);
            if (!target.normalOffsets.empty()) {
                GfVec3f normal =
                  welded.normals[vertexIndex] + target.normalOffsets[pointIndex];
                normal.Normalize();
                target.normals.push_back(normal);
            }
        }
        target.pointOffsets.clear();
        target.pointOffsets.shrink_to_fit();
        target.normalOffsets.clear();
        target.normalOffsets.shrink_to_fit();
    }
    welded.sourcePointIndices.clear();
    welded.sourcePointIndices.shrink_to_fit();
    mesh = std::move(welded);
    return true;
}

uint32_t
findParentNode(const SceneData& scene, SdfPath path)
{
    for (path = path.GetParentPath(); !path.IsEmpty() && path != SdfPath::AbsoluteRootPath();
         path = path.GetParentPath()) {
        const auto found = scene.nodeIds.find(path.GetString());
        if (found != scene.nodeIds.end()) {
            return found->second;
        }
    }
    return kMissingOffset;
}

void
addFrameIntervalSamples(std::vector<double>& times)
{
    if (times.size() < 2) {
        return;
    }
    std::sort(times.begin(), times.end());
    const double first = times.front();
    const double last = times.back();
    constexpr double maxGeneratedSamples = 10000.0;
    if (last <= first || last - first > maxGeneratedSamples) {
        return;
    }
    for (double time = std::ceil(first); time <= std::floor(last); time += 1.0) {
        times.push_back(time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(),
                            times.end(),
                            [](double left, double right) {
                                return std::abs(left - right) < 1e-9;
                            }),
                times.end());
}

NodeData
readNode(const UsdPrim& prim, const SceneData& scene)
{
    NodeData node;
    node.path = prim.GetPath();
    node.name = displayName(prim, "Node");
    node.parentId = findParentNode(scene, prim.GetPath());
    const UsdGeomXformable xformable(prim);
    bool resets = false;
    xformable.GetLocalTransformation(
      &node.localTransform, &resets, UsdTimeCode::Default());
    if (resets) {
        node.parentId = kMissingOffset;
    }

    if (!xformable.TransformMightBeTimeVarying()) {
        return node;
    }
    std::vector<double> times;
    xformable.GetTimeSamples(&times);
    addFrameIntervalSamples(times);
    node.animation.times.reserve(times.size());
    node.animation.matrices.reserve(times.size());
    for (const double time : times) {
        GfMatrix4d matrix(1.0);
        bool sampleResets = false;
        if (!xformable.GetLocalTransformation(&matrix, &sampleResets, UsdTimeCode(time))) {
            continue;
        }
        node.animation.times.push_back(static_cast<float>(time));
        node.animation.matrices.emplace_back(matrix);
    }
    return node;
}

std::vector<uint16_t>
buildJointMap(const UsdSkelSkinningQuery& skinningQuery, const VtTokenArray& skeletonJoints)
{
    VtTokenArray localJoints;
    if (!skinningQuery.GetJointOrder(&localJoints) || localJoints.empty()) {
        std::vector<uint16_t> identity(skeletonJoints.size());
        for (size_t index = 0; index < identity.size(); ++index) {
            identity[index] = static_cast<uint16_t>(index);
        }
        return identity;
    }
    std::vector<uint16_t> result(localJoints.size(), 0);
    for (size_t localIndex = 0; localIndex < localJoints.size(); ++localIndex) {
        const auto found =
          std::find(skeletonJoints.begin(), skeletonJoints.end(), localJoints[localIndex]);
        if (found != skeletonJoints.end()) {
            result[localIndex] =
              static_cast<uint16_t>(std::distance(skeletonJoints.begin(), found));
        }
    }
    return result;
}

SdfPath
mappedSkeletonPath(const UsdSkelSkinningQuery& skinningQuery,
                   const SdfPath& skeletonPath)
{
    const UsdPrim skinningPrim = skinningQuery.GetPrim();
    if (!skinningPrim.IsInstanceProxy()) {
        return skeletonPath;
    }
    const UsdPrim prototypePrim = skinningPrim.GetPrimInPrototype();
    if (!prototypePrim) {
        return skeletonPath;
    }
    SdfPath prototypeRoot;
    for (const SdfPath& prefix : prototypePrim.GetPath().GetPrefixes()) {
        if (UsdPrim::IsPrototypePath(prefix)) {
            prototypeRoot = prefix;
            break;
        }
    }
    if (prototypeRoot.IsEmpty() || !skeletonPath.HasPrefix(prototypeRoot)) {
        return skeletonPath;
    }

    SdfPath instanceRoot = skinningPrim.GetPath();
    size_t relativeElementCount =
      prototypePrim.GetPath().GetPathElementCount() -
      prototypeRoot.GetPathElementCount();
    while (relativeElementCount-- > 0) {
        instanceRoot = instanceRoot.GetParentPath();
    }
    return skeletonPath.ReplacePrefix(prototypeRoot, instanceRoot);
}

uint32_t
registerSkeleton(SceneData& scene, const UsdSkelSkeletonQuery& query)
{
    if (!query) {
        return kMissingOffset;
    }
    const UsdSkelAnimQuery& animQuery = query.GetAnimQuery();
    const std::string key = query.GetPrim().GetPath().GetString() + "|" +
                            (animQuery ? animQuery.GetPrim().GetPath().GetString() : "");
    const auto found = scene.skeletonIds.find(key);
    if (found != scene.skeletonIds.end()) {
        return found->second;
    }

    SkeletonData skeleton;
    skeleton.name = displayName(query.GetPrim(), "Skeleton");
    skeleton.joints = query.GetJointOrder();
    skeleton.parents.resize(skeleton.joints.size());
    const UsdSkelTopology& topology = query.GetTopology();
    for (size_t index = 0; index < skeleton.joints.size(); ++index) {
        skeleton.parents[index] = topology.GetParent(index);
    }
    query.ComputeJointLocalTransforms(
      &skeleton.restTransforms, UsdTimeCode::Default(), true);
    if (skeleton.restTransforms.size() != skeleton.joints.size()) {
        skeleton.restTransforms.assign(skeleton.joints.size(), GfMatrix4d(1.0));
    }
    VtMatrix4dArray skeletonBindTransforms;
    if (!query.GetSkeleton().GetBindTransformsAttr().Get(&skeletonBindTransforms) ||
        skeletonBindTransforms.size() != skeleton.joints.size()) {
        skeleton.bindTransforms = skeleton.restTransforms;
    } else {
        skeleton.bindTransforms.resize(skeleton.joints.size());
        if (!UsdSkelComputeJointLocalTransforms(
              topology, skeletonBindTransforms, skeleton.bindTransforms)) {
            skeleton.bindTransforms = skeleton.restTransforms;
        }
    }

    if (animQuery) {
        std::vector<double> times;
        animQuery.GetJointTransformTimeSamples(&times);
        addFrameIntervalSamples(times);
        for (const double time : times) {
            VtMatrix4dArray transforms;
            if (!query.ComputeJointLocalTransforms(
                  &transforms, UsdTimeCode(time), false) ||
                transforms.size() != skeleton.joints.size()) {
                continue;
            }
            skeleton.animation.times.push_back(static_cast<float>(time));
            skeleton.animation.localTransforms.push_back(std::move(transforms));
        }
    }

    scene.skeletons.push_back(std::move(skeleton));
    const uint32_t id = static_cast<uint32_t>(scene.skeletons.size());
    scene.skeletonIds.emplace(key, id);
    return id;
}

void
collectSkeletonBindings(const UsdStageRefPtr& stage, SceneData& scene)
{
    const Usd_PrimFlagsPredicate predicate = UsdTraverseInstanceProxies();
    for (const UsdPrim& prim : stage->Traverse(predicate)) {
        if (!prim.IsA<UsdSkelRoot>()) {
            continue;
        }
        const UsdSkelRoot root(prim);
        if (!scene.skelCache.Populate(root, predicate)) {
            continue;
        }
        std::vector<UsdSkelBinding> bindings;
        if (!scene.skelCache.ComputeSkelBindings(root, &bindings, predicate)) {
            continue;
        }
        for (const UsdSkelBinding& binding : bindings) {
            const UsdSkelSkeletonQuery skeletonQuery =
              scene.skelCache.GetSkelQuery(binding.GetSkeleton());
            const uint32_t skeletonId = registerSkeleton(scene, skeletonQuery);
            if (skeletonId == kMissingOffset) {
                continue;
            }
            const SkeletonData& skeleton = scene.skeletons[skeletonId - 1];
            for (const UsdSkelSkinningQuery& skinningQuery :
                 binding.GetSkinningTargets()) {
                SkinBinding skin;
                skin.skeletonId = skeletonId;
                skin.skeletonPath =
                  mappedSkeletonPath(skinningQuery,
                                     skeletonQuery.GetPrim().GetPath());
                skin.query = skinningQuery;
                skin.jointMap = buildJointMap(skinningQuery, skeleton.joints);
                scene.skinBindings[skinningQuery.GetPrim().GetPath().GetString()] =
                  std::move(skin);
            }
        }
    }
}

bool
readSkinning(const SkinBinding* skin,
             size_t pointCount,
             GfMatrix4d& geomBind,
             uint32_t& influenceCount,
             std::vector<JointSet>& joints,
             std::vector<WeightSet>& weights,
             const UsdTimeCode time = UsdTimeCode::Default())
{
    if (!skin) {
        return false;
    }
    VtIntArray sourceJoints;
    VtFloatArray sourceWeights;
    if (!skin->query.ComputeVaryingJointInfluences(
          pointCount, &sourceJoints, &sourceWeights, time)) {
        return false;
    }
    const int sourceInfluences = skin->query.GetNumInfluencesPerComponent();
    if (sourceInfluences <= 0 ||
        sourceJoints.size() != pointCount * static_cast<size_t>(sourceInfluences) ||
        sourceWeights.size() != sourceJoints.size()) {
        return false;
    }

    joints.resize(pointCount);
    weights.resize(pointCount);
    influenceCount = std::min<uint32_t>(sourceInfluences, kMaxInfluences);
    for (size_t point = 0; point < pointCount; ++point) {
        std::vector<std::pair<float, uint16_t>> influences;
        influences.reserve(sourceInfluences);
        for (int influence = 0; influence < sourceInfluences; ++influence) {
            const size_t sourceIndex =
              point * static_cast<size_t>(sourceInfluences) + influence;
            const int localJoint = sourceJoints[sourceIndex];
            const uint16_t skeletonJoint =
              localJoint >= 0 && static_cast<size_t>(localJoint) < skin->jointMap.size()
                ? skin->jointMap[localJoint]
                : 0;
            influences.emplace_back(sourceWeights[sourceIndex], skeletonJoint);
        }
        std::partial_sort(influences.begin(),
                          influences.begin() + influenceCount,
                          influences.end(),
                          [](const auto& left, const auto& right) {
                              return left.first > right.first;
                          });
        float weightSum = 0.0f;
        for (size_t influence = 0; influence < influenceCount; ++influence) {
            joints[point][influence] = influences[influence].second;
            weights[point][influence] = std::max(influences[influence].first, 0.0f);
            weightSum += weights[point][influence];
        }
        if (weightSum > 0.0f) {
            for (size_t influence = 0; influence < influenceCount; ++influence) {
                weights[point][influence] /= weightSum;
            }
        } else {
            weights[point][0] = 1.0f;
        }
    }
    geomBind = skin->query.GetGeomBindTransform(time);
    return true;
}

bool
computeMorphWeights(const UsdSkelAnimQuery& animation,
                    const UsdSkelAnimMapper& mapper,
                    const UsdSkelBlendShapeQuery& blendShapes,
                    const UsdTimeCode time,
                    VtFloatArray& flattened)
{
    VtFloatArray animationWeights;
    if (!animation.ComputeBlendShapeWeights(&animationWeights, time)) {
        return false;
    }
    VtFloatArray localWeights;
    const float zero = 0.0f;
    if (!mapper.Remap(animationWeights, &localWeights, 1, &zero)) {
        return false;
    }
    return blendShapes.ComputeFlattenedSubShapeWeights(localWeights, &flattened);
}

bool
expandMorphOffsets(const VtVec3fArray& authored,
                   const VtIntArray& pointIndices,
                   size_t pointCount,
                   const UsdPrim& blendShape,
                   const char* label,
                   std::vector<GfVec3f>& expanded)
{
    expanded.assign(pointCount, GfVec3f(0.0f));
    if (authored.empty()) {
        return true;
    }
    if (pointIndices.empty()) {
        if (authored.size() != pointCount) {
            TF_WARN("Blend shape <%s> has %zu %s offsets for %zu mesh points.",
                    blendShape.GetPath().GetText(),
                    authored.size(),
                    label,
                    pointCount);
            return false;
        }
        std::copy(authored.begin(), authored.end(), expanded.begin());
        return true;
    }
    if (authored.size() != pointIndices.size()) {
        TF_WARN("Blend shape <%s> has mismatched %s offsets and point indices.",
                blendShape.GetPath().GetText(),
                label);
        return false;
    }
    for (size_t index = 0; index < pointIndices.size(); ++index) {
        const int pointIndex = pointIndices[index];
        if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= pointCount) {
            TF_WARN("Blend shape <%s> has an invalid point index %d.",
                    blendShape.GetPath().GetText(),
                    pointIndex);
            return false;
        }
        expanded[pointIndex] = authored[index];
    }
    return true;
}

std::vector<SourceMorphTarget>
readMorphTargets(SceneData& scene,
                 const UsdGeomMesh& mesh,
                 size_t pointCount)
{
    const UsdSkelBindingAPI binding(mesh.GetPrim());
    const UsdSkelBlendShapeQuery query(binding);
    if (!query || query.GetNumSubShapes() == 0) {
        return {};
    }

    const std::vector<VtIntArray> pointIndices =
      query.ComputeBlendShapePointIndices();
    const std::vector<VtVec3fArray> pointOffsets =
      query.ComputeSubShapePointOffsets();
    const std::vector<VtVec3fArray> normalOffsets =
      query.ComputeSubShapeNormalOffsets();
    if (pointOffsets.size() != query.GetNumSubShapes() ||
        normalOffsets.size() != query.GetNumSubShapes() ||
        pointIndices.size() != query.GetNumBlendShapes()) {
        TF_WARN("Could not resolve blend shapes for mesh <%s>.",
                mesh.GetPath().GetText());
        return {};
    }

    VtFloatArray initialWeights(query.GetNumSubShapes(), 0.0f);
    std::vector<float> animationTimes;
    std::vector<VtFloatArray> animationWeights;
    VtTokenArray localOrder;
    const UsdPrim animationPrim = binding.GetInheritedAnimationSource();
    const UsdSkelAnimation animationSchema(animationPrim);
    const UsdSkelAnimQuery animation =
      animationSchema ? scene.skelCache.GetAnimQuery(animationSchema)
                      : UsdSkelAnimQuery();
    if (animation &&
        binding.GetBlendShapesAttr().Get(&localOrder) &&
        localOrder.size() == query.GetNumBlendShapes()) {
        const UsdSkelAnimMapper mapper(
          animation.GetBlendShapeOrder(), localOrder);
        VtFloatArray resolvedInitial;
        if (computeMorphWeights(animation,
                                mapper,
                                query,
                                UsdTimeCode::Default(),
                                resolvedInitial) &&
            resolvedInitial.size() == query.GetNumSubShapes()) {
            initialWeights = std::move(resolvedInitial);
        }

        std::vector<double> times;
        animation.GetBlendShapeWeightTimeSamples(&times);
        addFrameIntervalSamples(times);
        for (const double sampleTime : times) {
            VtFloatArray resolved;
            if (computeMorphWeights(animation,
                                    mapper,
                                    query,
                                    UsdTimeCode(sampleTime),
                                    resolved) &&
                resolved.size() == query.GetNumSubShapes()) {
                animationTimes.push_back(static_cast<float>(sampleTime));
                animationWeights.push_back(std::move(resolved));
            }
        }
    }

    std::vector<SourceMorphTarget> result;
    result.reserve(query.GetNumSubShapes());
    for (size_t subShapeIndex = 0;
         subShapeIndex < query.GetNumSubShapes();
         ++subShapeIndex) {
        if (pointOffsets[subShapeIndex].empty()) {
            continue;
        }
        const size_t blendShapeIndex =
          query.GetBlendShapeIndex(subShapeIndex);
        if (blendShapeIndex >= pointIndices.size()) {
            continue;
        }
        const UsdSkelBlendShape blendShape =
          query.GetBlendShape(blendShapeIndex);
        if (!blendShape) {
            continue;
        }

        SourceMorphTarget target;
        target.subShapeIndex = subShapeIndex;
        target.target.name = displayName(blendShape.GetPrim(), "Blend shape");
        const UsdSkelInbetweenShape inbetween =
          query.GetInbetween(subShapeIndex);
        if (inbetween) {
            target.target.name += " " + inbetween.GetAttr().GetName().GetString();
        }
        target.target.influence = initialWeights[subShapeIndex];
        if (!expandMorphOffsets(pointOffsets[subShapeIndex],
                                pointIndices[blendShapeIndex],
                                pointCount,
                                blendShape.GetPrim(),
                                "position",
                                target.pointOffsets)) {
            continue;
        }
        if (!normalOffsets[subShapeIndex].empty() &&
            !expandMorphOffsets(normalOffsets[subShapeIndex],
                                pointIndices[blendShapeIndex],
                                pointCount,
                                blendShape.GetPrim(),
                                "normal",
                                target.normalOffsets)) {
            continue;
        }
        target.target.animation.times = animationTimes;
        target.target.animation.influences.reserve(animationWeights.size());
        for (const VtFloatArray& weights : animationWeights) {
            target.target.animation.influences.push_back(
              weights[subShapeIndex]);
        }
        result.push_back(std::move(target));
    }
    const bool hasNormalOffsets =
      std::any_of(result.begin(),
                  result.end(),
                  [](const SourceMorphTarget& target) {
                      return !target.normalOffsets.empty();
                  });
    if (hasNormalOffsets) {
        for (SourceMorphTarget& target : result) {
            if (target.normalOffsets.empty()) {
                target.normalOffsets.assign(pointCount, GfVec3f(0.0f));
            }
        }
    }
    return result;
}

std::string
meshCacheKey(const UsdPrim& prim,
             const std::vector<uint32_t>& faceMaterials,
             uint32_t skeletonId,
             bool doubleSided,
             bool leftHanded)
{
    const UsdPrim source = prim.IsInstanceProxy() ? prim.GetPrimInPrototype() : prim;
    std::string key = source ? source.GetPath().GetString() : prim.GetPath().GetString();
    key += "|s" + std::to_string(skeletonId);
    key += doubleSided ? "|d1" : "|d0";
    key += leftHanded ? "|l1" : "|l0";
    uint64_t materialHash = 1469598103934665603ull;
    for (const uint32_t material : faceMaterials) {
        materialHash ^= material;
        materialHash *= 1099511628211ull;
    }
    key += "|m" + std::to_string(materialHash);
    return key;
}

bool
extractMesh(const UsdGeomMesh& usdMesh,
            uint32_t nodeId,
            SceneData& scene,
            const SkinBinding* skin,
            size_t* meshIndexOut = nullptr,
            const std::string& cacheSuffix = {},
            const UsdTimeCode time = UsdTimeCode::Default())
{
    VtVec3fArray points;
    VtIntArray faceCounts;
    VtIntArray faceIndices;
    if (!usdMesh.GetPointsAttr().Get(&points, time) ||
        !usdMesh.GetFaceVertexCountsAttr().Get(&faceCounts, time) ||
        !usdMesh.GetFaceVertexIndicesAttr().Get(&faceIndices, time) ||
        points.empty() || faceCounts.empty()) {
        return false;
    }
    size_t expectedCorners = 0;
    for (const int count : faceCounts) {
        if (count < 3) {
            TF_WARN("Skipping mesh <%s> with a face containing fewer than three vertices.",
                    usdMesh.GetPath().GetText());
            return false;
        }
        expectedCorners += static_cast<size_t>(count);
    }
    if (expectedCorners != faceIndices.size()) {
        TF_WARN("Skipping mesh <%s> with inconsistent face topology.",
                usdMesh.GetPath().GetText());
        return false;
    }

    const uint32_t fallbackMaterial = boundMaterialId(scene, usdMesh.GetPrim());
    std::vector<uint32_t> faceMaterials(faceCounts.size(), fallbackMaterial);
    for (const UsdGeomSubset& subset :
         UsdShadeMaterialBindingAPI(usdMesh.GetPrim()).GetMaterialBindSubsets()) {
        VtIntArray subsetFaces;
        if (!subset.GetIndicesAttr().Get(&subsetFaces, time)) {
            continue;
        }
        const uint32_t subsetMaterial = boundMaterialId(scene, subset.GetPrim());
        for (const int face : subsetFaces) {
            if (face >= 0 && static_cast<size_t>(face) < faceMaterials.size()) {
                faceMaterials[face] = subsetMaterial;
            }
        }
    }

    bool doubleSided = false;
    usdMesh.GetDoubleSidedAttr().Get(&doubleSided, time);
    TfToken orientation = UsdGeomTokens->rightHanded;
    UsdGeomGprim(usdMesh.GetPrim())
      .GetOrientationAttr()
      .Get(&orientation, time);
    const bool declaredLeftHanded = orientation == UsdGeomTokens->leftHanded;
    const UsdPrim sourcePrim =
      usdMesh.GetPrim().IsInstanceProxy() ? usdMesh.GetPrim().GetPrimInPrototype()
                                          : usdMesh.GetPrim();
    const std::string windingKey =
      (sourcePrim ? sourcePrim.GetPath() : usdMesh.GetPath()).GetString() +
      (declaredLeftHanded ? "|left" : "|right");
    PrimvarData<GfVec3f> normalData;
    bool normalsRead = false;
    bool sourceLeftHanded = declaredLeftHanded;
    const auto cachedWinding = scene.meshWinding.find(windingKey);
    if (cachedWinding != scene.meshWinding.end()) {
        sourceLeftHanded = cachedWinding->second;
    } else {
        normalData = readNormals(usdMesh, time);
        normalsRead = true;
        if (const std::optional<bool> inferred =
              inferLeftHandedWinding(points, faceCounts, faceIndices, normalData)) {
            sourceLeftHanded = *inferred;
            if (sourceLeftHanded != declaredLeftHanded) {
                TF_WARN("Mesh <%s> has authored normals opposite its declared '%s' winding; "
                        "using the authored exterior direction for Babylon culling.",
                        usdMesh.GetPath().GetText(),
                        orientation.GetText());
            }
        }
        scene.meshWinding.emplace(windingKey, sourceLeftHanded);
    }
    GfMatrix4d geomBind(1.0);
    uint32_t influenceCount = 0;
    std::vector<JointSet> pointJoints;
    std::vector<WeightSet> pointWeights;
    bool skinningValid = false;
    if (skin) {
        skinningValid = readSkinning(
          skin,
          points.size(),
          geomBind,
          influenceCount,
          pointJoints,
          pointWeights,
          time);
        if (!skinningValid) {
            TF_WARN(
              "Could not read skinning influences for mesh <%s>; emitting it as a static mesh.",
              usdMesh.GetPath().GetText());
            geomBind.SetIdentity();
            influenceCount = 0;
            pointJoints.clear();
            pointWeights.clear();
        }
    }
    const bool bakedReflection = geomBind.GetDeterminant() < 0.0;
    const bool outputLeftHanded = sourceLeftHanded != bakedReflection;
    const uint32_t skeletonId =
      skinningValid ? skin->skeletonId : kMissingOffset;
    uint32_t placementNodeId = nodeId;
    if (skinningValid) {
        const auto skeletonNode =
          scene.nodeIds.find(skin->skeletonPath.GetString());
        if (skeletonNode == scene.nodeIds.end()) {
            TF_WARN("Could not find the Skeleton transform <%s> for skinned mesh <%s>.",
                    skin->skeletonPath.GetText(),
                    usdMesh.GetPath().GetText());
            return false;
        }
        placementNodeId = skeletonNode->second;
    }
    std::string key =
      meshCacheKey(usdMesh.GetPrim(),
                   faceMaterials,
                   skeletonId,
                   doubleSided,
                   outputLeftHanded) +
      cacheSuffix;
    if (skinningValid) {
        uint64_t geomBindHash = 1469598103934665603ull;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                hashValue(geomBindHash, geomBind[row][column]);
            }
        }
        key += "|g" + std::to_string(geomBindHash);
    }
    const UsdSkelBindingAPI blendShapeBinding(usdMesh.GetPrim());
    if (blendShapeBinding.GetBlendShapesAttr().HasAuthoredValue()) {
        const UsdPrim animationSource =
          blendShapeBinding.GetInheritedAnimationSource();
        key += "|morphAnimation:";
        key += animationSource
                 ? animationSource.GetPath().GetString()
                 : std::string("none");
    }
    const auto cached = scene.meshIds.find(key);
    if (cached != scene.meshIds.end()) {
        scene.meshes[cached->second].nodeIds.push_back(placementNodeId);
        if (meshIndexOut) {
            *meshIndexOut = cached->second;
        }
        return true;
    }

    MeshData mesh;
    mesh.name = displayName(usdMesh.GetPrim(), "Mesh");
    mesh.doubleSided = doubleSided;
    mesh.leftHanded = outputLeftHanded;
    mesh.skeletonId = skeletonId;
    mesh.influenceCount = influenceCount;

    if (!normalsRead) {
        normalData = readNormals(usdMesh, time);
    }
    std::vector<GfVec3f> generatedNormals;
    if (!normalData) {
        generatedNormals =
          computeSmoothNormals(points, faceCounts, faceIndices, sourceLeftHanded);
        normalData.values.assign(generatedNormals.begin(), generatedNormals.end());
        normalData.interpolation = UsdGeomTokens->vertex;
    }
    const GfMatrix4d normalTransform = geomBind.GetInverse().GetTranspose();
    std::vector<SourceMorphTarget> morphTargets =
      readMorphTargets(scene, usdMesh, points.size());
    for (SourceMorphTarget& target : morphTargets) {
        for (GfVec3f& offset : target.pointOffsets) {
            offset = GfVec3f(
              geomBind.TransformDir(GfVec3d(offset)));
        }
        for (GfVec3f& offset : target.normalOffsets) {
            offset = GfVec3f(
              normalTransform.TransformDir(GfVec3d(offset)));
        }
    }
    TfToken uvName;
    if (!materialUvName(scene, faceMaterials, uvName)) {
        return false;
    }
    const PrimvarData<GfVec2f> uvData = readUvs(usdMesh, uvName, time);
    if (!uvName.IsEmpty() && !uvData) {
        TF_WARN("Cannot emit mesh <%s>: its material textures require UV primvar '%s'.",
                usdMesh.GetPath().GetText(),
                uvName.GetText());
        return false;
    }
    const UsdGeomGprim gprim(usdMesh.GetPrim());
    const PrimvarData<GfVec3f> colorData =
      readAuthoredPrimvar<GfVec3f>(gprim.GetDisplayColorPrimvar(), time);
    const PrimvarData<float> opacityData =
      readAuthoredPrimvar<float>(gprim.GetDisplayOpacityPrimvar(), time);

    std::map<uint32_t, std::vector<uint32_t>> materialIndices;
    auto appendVertex = [&](size_t faceIndex, size_t cornerIndex, int pointIndex) {
        const size_t point = static_cast<size_t>(pointIndex);
        const GfVec3d transformedPoint = geomBind.Transform(GfVec3d(points[point]));
        mesh.positions.emplace_back(transformedPoint);

        GfVec3f normal(0.0f, 1.0f, 0.0f);
        if (const GfVec3f* value = normalData.value(faceIndex, cornerIndex, point)) {
            const GfVec3d transformedNormal =
              normalTransform.TransformDir(GfVec3d(*value));
            normal = GfVec3f(transformedNormal);
            normal.Normalize();
        }
        mesh.normals.push_back(normal);

        if (uvData) {
            const GfVec2f* uv = uvData.value(faceIndex, cornerIndex, point);
            mesh.uvs.push_back(uv ? *uv : GfVec2f(0.0f));
        }
        if (colorData || opacityData) {
            const GfVec3f* color = colorData.value(faceIndex, cornerIndex, point);
            const float* opacity = opacityData.value(faceIndex, cornerIndex, point);
            const GfVec3f rgb = color ? *color : GfVec3f(1.0f);
            mesh.colors.emplace_back(rgb[0], rgb[1], rgb[2], opacity ? *opacity : 1.0f);
        }
        if (!pointJoints.empty()) {
            mesh.joints.push_back(pointJoints[point]);
            mesh.weights.push_back(pointWeights[point]);
        }
        if (!morphTargets.empty()) {
            mesh.sourcePointIndices.push_back(
              static_cast<uint32_t>(point));
        }
        return static_cast<uint32_t>(mesh.positions.size() - 1);
    };

    size_t cornerOffset = 0;
    for (size_t face = 0; face < faceCounts.size(); ++face) {
        const int count = faceCounts[face];
        for (int triangle = 0; triangle < count - 2; ++triangle) {
            const std::array<size_t, 3> corners = {
                cornerOffset,
                cornerOffset + static_cast<size_t>(triangle + 1),
                cornerOffset + static_cast<size_t>(triangle + 2),
            };
            auto& output = materialIndices[faceMaterials[face]];
            for (const size_t corner : corners) {
                const int pointIndex = faceIndices[corner];
                if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= points.size()) {
                    TF_WARN("Skipping mesh <%s> with an invalid point index.",
                            usdMesh.GetPath().GetText());
                    return false;
                }
                output.push_back(appendVertex(face, corner, pointIndex));
            }
        }
        cornerOffset += static_cast<size_t>(count);
    }

    for (auto& [material, indices] : materialIndices) {
        SubmeshData submesh;
        submesh.materialId = material;
        submesh.indexStart = static_cast<uint32_t>(mesh.indices.size());
        submesh.indexCount = static_cast<uint32_t>(indices.size());
        mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
        mesh.submeshes.push_back(submesh);
    }
    mesh.morphTargets.reserve(morphTargets.size());
    for (SourceMorphTarget& target : morphTargets) {
        target.target.pointOffsets = std::move(target.pointOffsets);
        target.target.normalOffsets = std::move(target.normalOffsets);
        mesh.morphTargets.push_back(std::move(target.target));
    }
    mesh.nodeIds.push_back(placementNodeId);
    const size_t meshIndex = scene.meshes.size();
    scene.meshIds.emplace(key, meshIndex);
    scene.meshes.push_back(std::move(mesh));
    if (meshIndexOut) {
        *meshIndexOut = meshIndex;
    }
    return true;
}

bool
isVisible(const UsdPrim& prim,
          const UsdTimeCode time = UsdTimeCode::Default())
{
    const UsdGeomImageable imageable(prim);
    return !imageable ||
           imageable.ComputeVisibility(time) != UsdGeomTokens->invisible;
}

bool
extractAnalyticPrimitive(const UsdPrim& prim,
                         uint32_t nodeId,
                         SceneData& scene,
                         size_t* primitiveIndexOut,
                         const std::string& cacheSuffix,
                         UsdTimeCode time);

bool
extractPointInstancer(const UsdGeomPointInstancer& instancer,
                      uint32_t nodeId,
                      SceneData& scene)
{
    SdfPathVector prototypePaths;
    if (!instancer.GetPrototypesRel().GetForwardedTargets(&prototypePaths) ||
        prototypePaths.empty()) {
        TF_WARN("Skipping point instancer <%s> without prototypes.",
                instancer.GetPath().GetText());
        return true;
    }

    std::vector<double> instanceTimes;
    const std::array<UsdAttribute, 11> instanceAttributes = {
        instancer.GetProtoIndicesAttr(),
        instancer.GetPositionsAttr(),
        instancer.GetOrientationsAttr(),
        instancer.GetOrientationsfAttr(),
        instancer.GetScalesAttr(),
        instancer.GetVelocitiesAttr(),
        instancer.GetAccelerationsAttr(),
        instancer.GetAngularVelocitiesAttr(),
        instancer.GetIdsAttr(),
        instancer.GetInvisibleIdsAttr(),
        instancer.GetVisibilityAttr(),
    };
    for (const UsdAttribute& attribute : instanceAttributes) {
        std::vector<double> attributeTimes;
        attribute.GetTimeSamples(&attributeTimes);
        instanceTimes.insert(
          instanceTimes.end(), attributeTimes.begin(), attributeTimes.end());
    }
    for (const SdfPath& prototypePath : prototypePaths) {
        const UsdPrim prototype =
          instancer.GetPrim().GetStage()->GetPrimAtPath(prototypePath);
        for (const UsdPrim& prim :
             UsdPrimRange(prototype, UsdTraverseInstanceProxies())) {
            for (const UsdAttribute& attribute : prim.GetAttributes()) {
                std::vector<double> attributeTimes;
                attribute.GetTimeSamples(&attributeTimes);
                instanceTimes.insert(instanceTimes.end(),
                                     attributeTimes.begin(),
                                     attributeTimes.end());
            }
        }
    }
    std::sort(instanceTimes.begin(), instanceTimes.end());
    instanceTimes.erase(std::unique(instanceTimes.begin(), instanceTimes.end()),
                        instanceTimes.end());
    const bool unsupportedAnimation = instanceTimes.size() > 1;
    std::vector<double> instancerTransformTimes;
    UsdGeomXformable(instancer).GetTimeSamples(&instancerTransformTimes);
    instanceTimes.insert(instanceTimes.end(),
                         instancerTransformTimes.begin(),
                         instancerTransformTimes.end());
    std::sort(instanceTimes.begin(), instanceTimes.end());
    instanceTimes.erase(std::unique(instanceTimes.begin(), instanceTimes.end()),
                        instanceTimes.end());
    const UsdTimeCode time =
      instanceTimes.empty()
        ? UsdTimeCode::Default()
        : UsdTimeCode(instanceTimes.front());
    if (unsupportedAnimation) {
        TF_WARN("Point instancer <%s> has animated instance or prototype data; "
                "importing its first frame as static thin instances.",
                instancer.GetPath().GetText());
    }
    if (!isVisible(instancer.GetPrim(), time)) {
        return true;
    }
    if (!time.IsDefault()) {
        NodeData& instancerNode = scene.nodes[nodeId - 1];
        bool resetsXformStack = false;
        UsdGeomXformable(instancer).GetLocalTransformation(
          &instancerNode.localTransform, &resetsXformStack, time);
        if (resetsXformStack) {
            instancerNode.parentId = kMissingOffset;
        }
    }

    VtIntArray prototypeIndices;
    if (!instancer.GetProtoIndicesAttr().Get(&prototypeIndices, time)) {
        TF_WARN("Skipping point instancer <%s> without prototype indices.",
                instancer.GetPath().GetText());
        return true;
    }

    UsdGeomXformCache xformCache(time);
    VtMatrix4dArray instanceTransforms;
    if (!instancer.ComputeInstanceTransformsAtTime(
          &instanceTransforms,
          time,
          time,
          UsdGeomPointInstancer::IncludeProtoXform,
          UsdGeomPointInstancer::IgnoreMask) ||
        instanceTransforms.size() != prototypeIndices.size()) {
        TF_WARN("Could not compute transforms for point instancer <%s>.",
                instancer.GetPath().GetText());
        return false;
    }

    const std::vector<bool> mask = instancer.ComputeMaskAtTime(time);
    if (!mask.empty() && mask.size() != prototypeIndices.size()) {
        TF_WARN("Point instancer <%s> produced an invalid visibility mask.",
                instancer.GetPath().GetText());
        return false;
    }

    std::vector<std::vector<size_t>> instancesByPrototype(prototypePaths.size());
    for (size_t instanceIndex = 0; instanceIndex < prototypeIndices.size();
         ++instanceIndex) {
        const int prototypeIndex = prototypeIndices[instanceIndex];
        if (prototypeIndex < 0 ||
            static_cast<size_t>(prototypeIndex) >= prototypePaths.size()) {
            TF_WARN("Point instancer <%s> references invalid prototype index %d.",
                    instancer.GetPath().GetText(),
                    prototypeIndex);
            return false;
        }
        if (mask.empty() || mask[instanceIndex]) {
            instancesByPrototype[prototypeIndex].push_back(instanceIndex);
        }
    }
    const UsdStagePtr stage = instancer.GetPrim().GetStage();
    for (size_t prototypeIndex = 0; prototypeIndex < prototypePaths.size();
         ++prototypeIndex) {
        const std::vector<size_t>& instanceIndices =
          instancesByPrototype[prototypeIndex];
        if (instanceIndices.empty()) {
            continue;
        }
        const UsdPrim prototype = stage->GetPrimAtPath(prototypePaths[prototypeIndex]);
        if (!prototype) {
            TF_WARN("Point instancer <%s> references missing prototype <%s>.",
                    instancer.GetPath().GetText(),
                    prototypePaths[prototypeIndex].GetText());
            return false;
        }

        UsdPrimRange range(prototype, UsdTraverseInstanceProxies());
        for (auto iterator = range.begin(); iterator != range.end(); ++iterator) {
            const UsdPrim prim = *iterator;
            if (!isVisible(prim, time)) {
                iterator.PruneChildren();
                continue;
            }
            if (prim.IsA<UsdGeomPointInstancer>()) {
                TF_WARN("Nested point instancer <%s> is not yet supported.",
                        prim.GetPath().GetText());
                iterator.PruneChildren();
                continue;
            }
            const bool isMesh = prim.IsA<UsdGeomMesh>();
            const bool isAnalytic =
              prim.IsA<UsdGeomCube>() || prim.IsA<UsdGeomSphere>() ||
              prim.IsA<UsdGeomCylinder>() || prim.IsA<UsdGeomCone>();
            if (!isMesh && !isAnalytic) {
                if (prim.IsA<UsdGeomGprim>()) {
                    TF_WARN("Skipping unsupported point-instanced geometry <%s>.",
                            prim.GetPath().GetText());
                }
                continue;
            }

            bool resetsXformStack = false;
            const GfMatrix4d prototypeRelative =
              xformCache.ComputeRelativeTransform(
                prim, prototype, &resetsXformStack);
            if (resetsXformStack) {
                TF_WARN("Skipping point-instanced mesh <%s> with a reset transform stack.",
                        prim.GetPath().GetText());
                continue;
            }

            NodeData sourceNode;
            sourceNode.path = prim.GetPath();
            sourceNode.name = displayName(prim, "Prototype");
            sourceNode.parentId = nodeId;
            scene.nodes.push_back(std::move(sourceNode));
            const uint32_t sourceNodeId =
              static_cast<uint32_t>(scene.nodes.size());

            const std::string cacheSuffix =
              "|pointInstancer:" + instancer.GetPath().GetString() +
              "|placement:" + prim.GetPath().GetString();
            ThinInstanceData batch;
            if (isMesh) {
                if (!extractMesh(UsdGeomMesh(prim),
                                 sourceNodeId,
                                 scene,
                                 nullptr,
                                 &batch.sourceIndex,
                                 cacheSuffix,
                                 time)) {
                    return false;
                }
            } else {
                batch.analyticSource = true;
                if (!extractAnalyticPrimitive(
                      prim,
                      sourceNodeId,
                      scene,
                      &batch.sourceIndex,
                      cacheSuffix,
                      time)) {
                    return false;
                }
            }

            batch.transforms.reserve(instanceIndices.size());
            for (const size_t instanceIndex : instanceIndices) {
                batch.transforms.emplace_back(
                  prototypeRelative * instanceTransforms[instanceIndex]);
            }
            scene.thinInstances.push_back(std::move(batch));
        }
    }
    return true;
}

PrimitiveAxis
primitiveAxis(const TfToken& axis)
{
    if (axis == UsdGeomTokens->x) {
        return PrimitiveAxis::X;
    }
    if (axis == UsdGeomTokens->y) {
        return PrimitiveAxis::Y;
    }
    return PrimitiveAxis::Z;
}

uint32_t
analyticMaterialId(SceneData& scene,
                   const UsdPrim& prim,
                   const UsdTimeCode time)
{
    const uint32_t bound = boundMaterialId(scene, prim);
    if (bound != 0) {
        return bound;
    }
    const UsdPrim source = prim.IsInstanceProxy() ? prim.GetPrimInPrototype() : prim;
    const std::string key =
      source ? source.GetPath().GetString() : prim.GetPath().GetString();
    const auto cached = scene.analyticMaterialIds.find(key);
    if (cached != scene.analyticMaterialIds.end()) {
        return cached->second;
    }
    const UsdGeomGprim gprim(prim);
    const PrimvarData<GfVec3f> color =
      readAuthoredPrimvar<GfVec3f>(gprim.GetDisplayColorPrimvar(), time);
    const PrimvarData<float> opacity =
      readAuthoredPrimvar<float>(gprim.GetDisplayOpacityPrimvar(), time);
    if (!color && !opacity) {
        scene.analyticMaterialIds.emplace(key, 0);
        return 0;
    }

    MaterialData material;
    material.name = displayName(prim, "Primitive") + " display material";
    const bool constantColor =
      color && std::all_of(color.values.begin(),
                           color.values.end(),
                           [&](const GfVec3f& value) {
                               return value == color.values.front();
                           });
    const bool constantOpacity =
      opacity && std::all_of(opacity.values.begin(),
                             opacity.values.end(),
                             [&](float value) {
                                 return value == opacity.values.front();
                             });
    if (color && !constantColor) {
        TF_WARN("Ignoring varying displayColor on analytic primitive <%s>.",
                prim.GetPath().GetText());
    }
    if (opacity && !constantOpacity) {
        TF_WARN("Ignoring varying displayOpacity on analytic primitive <%s>.",
                prim.GetPath().GetText());
    }
    if (constantColor) {
        material.baseColor = color.values.front();
    }
    if (constantOpacity) {
        material.opacity = opacity.values.front();
    }
    if (!constantColor && !constantOpacity) {
        scene.analyticMaterialIds.emplace(key, 0);
        return 0;
    }
    scene.materials.push_back(std::move(material));
    const uint32_t id = static_cast<uint32_t>(scene.materials.size() - 1);
    scene.analyticMaterialIds.emplace(key, id);
    return id;
}

bool
extractAnalyticPrimitive(const UsdPrim& prim,
                         uint32_t nodeId,
                         SceneData& scene,
                         size_t* primitiveIndexOut,
                         const std::string& cacheSuffix,
                         const UsdTimeCode time)
{
    AnalyticPrimitiveData primitive;
    primitive.name = displayName(prim, "Primitive");
    primitive.nodeIds.push_back(nodeId);
    primitive.materialId = analyticMaterialId(scene, prim, time);

    if (prim.IsA<UsdGeomCube>()) {
        double size = 2.0;
        UsdGeomCube(prim).GetSizeAttr().Get(&size, time);
        primitive.type = AnalyticPrimitiveType::Cube;
        primitive.sizeOrRadius = static_cast<float>(size);
        primitive.tessellation = 0;
    } else if (prim.IsA<UsdGeomSphere>()) {
        double radius = 1.0;
        UsdGeomSphere(prim).GetRadiusAttr().Get(&radius, time);
        primitive.type = AnalyticPrimitiveType::Sphere;
        primitive.sizeOrRadius = static_cast<float>(radius);
    } else if (prim.IsA<UsdGeomCylinder>()) {
        double radius = 1.0;
        double height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        const UsdGeomCylinder cylinder(prim);
        cylinder.GetRadiusAttr().Get(&radius, time);
        cylinder.GetHeightAttr().Get(&height, time);
        cylinder.GetAxisAttr().Get(&axis, time);
        primitive.type = AnalyticPrimitiveType::Cylinder;
        primitive.axis = primitiveAxis(axis);
        primitive.sizeOrRadius = static_cast<float>(radius);
        primitive.height = static_cast<float>(height);
    } else if (prim.IsA<UsdGeomCone>()) {
        double radius = 1.0;
        double height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        const UsdGeomCone cone(prim);
        cone.GetRadiusAttr().Get(&radius, time);
        cone.GetHeightAttr().Get(&height, time);
        cone.GetAxisAttr().Get(&axis, time);
        primitive.type = AnalyticPrimitiveType::Cone;
        primitive.axis = primitiveAxis(axis);
        primitive.sizeOrRadius = static_cast<float>(radius);
        primitive.height = static_cast<float>(height);
    } else {
        return false;
    }
    if (!std::isfinite(primitive.sizeOrRadius) || primitive.sizeOrRadius <= 0.0f ||
        ((primitive.type == AnalyticPrimitiveType::Cylinder ||
          primitive.type == AnalyticPrimitiveType::Cone) &&
         (!std::isfinite(primitive.height) || primitive.height <= 0.0f))) {
        TF_WARN("Skipping analytic primitive <%s> with invalid dimensions.",
                prim.GetPath().GetText());
        return false;
    }

    const UsdGeomGprim gprim(prim);
    bool doubleSided = false;
    TfToken orientation = UsdGeomTokens->rightHanded;
    gprim.GetDoubleSidedAttr().Get(&doubleSided, time);
    gprim.GetOrientationAttr().Get(&orientation, time);
    primitive.flags = doubleSided ? MeshDoubleSided : 0;
    primitive.flags |= orientation == UsdGeomTokens->leftHanded ? MeshLeftHanded : 0;
    const UsdPrim source = prim.IsInstanceProxy() ? prim.GetPrimInPrototype() : prim;
    const std::string sourcePath =
      source ? source.GetPath().GetString() : prim.GetPath().GetString();
    const std::string key =
      sourcePath + "|" + std::to_string(static_cast<uint32_t>(primitive.type)) + "|" +
      std::to_string(primitive.materialId) + "|" + std::to_string(primitive.flags) + "|" +
      std::to_string(static_cast<uint32_t>(primitive.axis)) + "|" +
      std::to_string(primitive.sizeOrRadius) + "|" + std::to_string(primitive.height) +
      cacheSuffix;
    const auto cachedPrimitive = scene.analyticPrimitiveIds.find(key);
    if (cachedPrimitive != scene.analyticPrimitiveIds.end()) {
        scene.analyticPrimitives[cachedPrimitive->second].nodeIds.push_back(nodeId);
        if (primitiveIndexOut) {
            *primitiveIndexOut = cachedPrimitive->second;
        }
        return true;
    }
    const size_t primitiveIndex = scene.analyticPrimitives.size();
    scene.analyticPrimitiveIds.emplace(key, primitiveIndex);
    scene.analyticPrimitives.push_back(std::move(primitive));
    if (primitiveIndexOut) {
        *primitiveIndexOut = primitiveIndex;
    }
    return true;
}

bool
extractStage(const UsdStageRefPtr& stage, SceneData& scene)
{
    scene.upAxis = UsdGeomGetStageUpAxis(stage);
    scene.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
    scene.timeCodesPerSecond = stage->GetTimeCodesPerSecond();
    collectSkeletonBindings(stage, scene);

    UsdPrimRange range = stage->Traverse(UsdTraverseInstanceProxies());
    for (auto iterator = range.begin(); iterator != range.end(); ++iterator) {
        const UsdPrim prim = *iterator;
        const bool isPointInstancer = prim.IsA<UsdGeomPointInstancer>();
        const UsdGeomXformable xformable(prim);
        if (xformable) {
            scene.nodes.push_back(readNode(prim, scene));
            scene.nodeIds[prim.GetPath().GetString()] =
              static_cast<uint32_t>(scene.nodes.size());
        }
        if (isPointInstancer) {
            iterator.PruneChildren();
        }
    }

    range = stage->Traverse(UsdTraverseInstanceProxies());
    for (auto iterator = range.begin(); iterator != range.end(); ++iterator) {
        const UsdPrim prim = *iterator;
        const bool isPointInstancer = prim.IsA<UsdGeomPointInstancer>();
        if (!isPointInstancer && !isVisible(prim)) {
            iterator.PruneChildren();
            continue;
        }
        const auto node = scene.nodeIds.find(prim.GetPath().GetString());
        if (node == scene.nodeIds.end()) {
            continue;
        }
        const uint32_t nodeId = node->second;
        if (isPointInstancer) {
            if (!extractPointInstancer(
                  UsdGeomPointInstancer(prim), nodeId, scene)) {
                return false;
            }
            iterator.PruneChildren();
            continue;
        }
        if (prim.IsA<UsdGeomMesh>()) {
            const auto skin = scene.skinBindings.find(prim.GetPath().GetString());
            if (!extractMesh(UsdGeomMesh(prim),
                             nodeId,
                             scene,
                             skin == scene.skinBindings.end() ? nullptr : &skin->second)) {
                return false;
            }
        } else {
            extractAnalyticPrimitive(
              prim, nodeId, scene, nullptr, {}, UsdTimeCode::Default());
        }
    }
    return true;
}

std::string
resolvedTexturePath(const TextureData& texture)
{
    if (!texture.asset.GetResolvedPath().empty()) {
        return texture.asset.GetResolvedPath();
    }
    return ArGetResolver().Resolve(texture.asset.GetAssetPath()).GetPathString();
}

uint32_t
mimeType(const std::string& path)
{
    const std::string extension =
      TfStringToLower(ArGetResolver().GetExtension(path));
    if (extension == "jpg" || extension == "jpeg") {
        return 2;
    }
    if (extension == "bmp") {
        return 3;
    }
    if (extension == "webp") {
        return 4;
    }
    if (extension == "png") {
        return 1;
    }
    return 0;
}

uint32_t
wrapMode(const TfToken& mode)
{
    if (mode == TfToken("mirror")) {
        return 2;
    }
    return mode == TfToken("repeat") ? 1 : 0;
}

TextureSourceColorSpace
sourceColorSpace(const TfToken& value)
{
    if (value == TfToken("raw")) {
        return TextureSourceColorSpace::Raw;
    }
    if (value == TfToken("sRGB")) {
        return TextureSourceColorSpace::SRGB;
    }
    return TextureSourceColorSpace::Auto;
}

int32_t
emitTexture(CommandWriter& commands,
            BufferWriter& data,
            const TextureData& texture,
            uint32_t& nextTextureId,
            std::unordered_map<std::string, uint32_t>& textureCache,
            std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>& imageCache)
{
    if (!texture) {
        return -1;
    }
    const std::string sourceKey = texture.sourceShader.GetString();
    if (!sourceKey.empty()) {
        const auto existing = textureCache.find(sourceKey);
        if (existing != textureCache.end()) {
            return static_cast<int32_t>(existing->second);
        }
    }
    const std::string resolved = resolvedTexturePath(texture);
    if (resolved.empty()) {
        return -1;
    }
    const uint32_t mime = mimeType(resolved);
    if (mime == 0) {
        TF_WARN("Skipping browser-unsupported texture format '%s'.", resolved.c_str());
        return -1;
    }

    auto cached = imageCache.find(resolved);
    if (cached == imageCache.end()) {
        const std::shared_ptr<ArAsset> asset =
          ArGetResolver().OpenAsset(ArResolvedPath(resolved));
        if (!asset || asset->GetSize() == 0) {
            return -1;
        }
        const std::shared_ptr<const char> buffer = asset->GetBuffer();
        if (!buffer) {
            return -1;
        }
        const uint32_t offset =
          data.appendBytes(buffer.get(), asset->GetSize(), 4);
        cached =
          imageCache.emplace(resolved,
                             std::make_pair(offset,
                                            static_cast<uint32_t>(asset->GetSize())))
            .first;
    }

    uint32_t nameLength = 0;
    const uint32_t nameOffset =
      appendString(data, texture.name.empty() ? TfGetBaseName(resolved) : texture.name, nameLength);
    data.align();
    const uint32_t transformOffset = data.size();
    data.f32(texture.uvScale[0]);
    data.f32(texture.uvScale[1]);
    data.f32(texture.uvTranslation[0]);
    data.f32(texture.uvTranslation[1]);
    data.f32(texture.rotation);
    const uint32_t valueTransformOffset = data.size();
    for (int index = 0; index < 4; ++index) {
        data.f32(texture.valueScale[index]);
    }
    for (int index = 0; index < 4; ++index) {
        data.f32(texture.valueBias[index]);
    }

    const uint32_t textureId = nextTextureId++;
    const uint32_t record = commands.begin(Command::Texture);
    commands.buffer.u32(textureId);
    commands.buffer.u32(nameOffset);
    commands.buffer.u32(nameLength);
    commands.buffer.u32(mime);
    commands.buffer.u32(cached->second.first);
    commands.buffer.u32(cached->second.second);
    commands.buffer.u32(static_cast<uint32_t>(std::max(texture.uvIndex, 0)));
    commands.buffer.u32(transformOffset);
    commands.buffer.u32(wrapMode(texture.wrapS));
    commands.buffer.u32(wrapMode(texture.wrapT));
    commands.buffer.u32(static_cast<uint32_t>(sourceColorSpace(texture.sourceColorSpace)));
    commands.buffer.u32(valueTransformOffset);
    commands.end(record);
    if (!sourceKey.empty()) {
        textureCache.emplace(sourceKey, textureId);
    }
    return static_cast<int32_t>(textureId);
}

void
emitMaterials(CommandWriter& commands, BufferWriter& data, const SceneData& scene)
{
    uint32_t nextTextureId = 1;
    std::unordered_map<std::string, uint32_t> textureCache;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> imageCache;
    for (size_t materialIndex = 0; materialIndex < scene.materials.size();
         ++materialIndex) {
        const MaterialData& material = scene.materials[materialIndex];
        const std::array<const TextureData*, 7> textureData = {
            &material.baseTexture,
            &material.opacityTexture,
            &material.normalTexture,
            &material.metallicTexture,
            &material.roughnessTexture,
            &material.occlusionTexture,
            &material.emissiveTexture,
        };
        std::array<int32_t, 7> textureIds;
        for (size_t index = 0; index < textureData.size(); ++index) {
            textureIds[index] = emitTexture(commands,
                                            data,
                                            *textureData[index],
                                            nextTextureId,
                                            textureCache,
                                            imageCache);
        }
        const std::array<uint32_t, 7> authoredChannels = {
            material.baseChannel,
            material.opacityChannel,
            material.normalChannel,
            material.metallicChannel,
            material.roughnessChannel,
            material.occlusionChannel,
            material.emissiveChannel,
        };

        data.align();
        const bool hasBaseTexture = textureIds[0] >= 0;
        const bool hasOpacityTexture = textureIds[1] >= 0;
        const bool hasMetallicTexture = textureIds[3] >= 0;
        const bool hasRoughnessTexture = textureIds[4] >= 0;
        const bool hasEmissiveTexture = textureIds[6] >= 0;
        const uint32_t baseOffset = data.size();
        data.f32(hasBaseTexture ? 1.0f : material.baseColor[0]);
        data.f32(hasBaseTexture ? 1.0f : material.baseColor[1]);
        data.f32(hasBaseTexture ? 1.0f : material.baseColor[2]);
        data.f32(hasOpacityTexture ? 1.0f : material.opacity);
        const uint32_t emissiveOffset = data.size();
        data.f32(hasEmissiveTexture ? 1.0f : material.emissive[0]);
        data.f32(hasEmissiveTexture ? 1.0f : material.emissive[1]);
        data.f32(hasEmissiveTexture ? 1.0f : material.emissive[2]);
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, material.name, nameLength);

        uint32_t flags = material.unlit ? MaterialUnlit : 0;
        flags |= material.doubleSided ? MaterialDoubleSided : 0;
        flags |= material.opacity < 0.999f || hasOpacityTexture ? MaterialAlphaBlend : 0;
        const uint32_t record = commands.begin(Command::Material);
        commands.buffer.u32(static_cast<uint32_t>(materialIndex));
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(baseOffset);
        commands.buffer.u32(emissiveOffset);
        commands.buffer.f32(hasMetallicTexture ? 1.0f : material.metallic);
        commands.buffer.f32(hasRoughnessTexture ? 1.0f : material.roughness);
        commands.buffer.f32(material.normalScale);
        commands.buffer.f32(material.alphaCutoff);
        commands.buffer.u32(flags);
        for (const int32_t textureId : textureIds) {
            commands.buffer.u32(static_cast<uint32_t>(textureId));
        }
        for (size_t index = 0; index < textureIds.size(); ++index) {
            commands.buffer.u32(textureIds[index] >= 0 ? authoredChannels[index]
                                                       : kMissingOffset);
        }
        commands.end(record);
    }
}

template<typename T>
uint32_t
appendArray(BufferWriter& data, const std::vector<T>& values)
{
    return values.empty()
             ? kMissingOffset
             : data.appendBytes(values.data(), values.size() * sizeof(T), alignof(T));
}

uint32_t
appendUvs(BufferWriter& data, const std::vector<GfVec2f>& values)
{
    if (values.empty()) {
        return kMissingOffset;
    }
    data.align();
    const uint32_t offset = data.size();
    for (const GfVec2f& uv : values) {
        data.f32(uv[0]);
        data.f32(1.0f - uv[1]);
    }
    return offset;
}

void
emitAnimation(CommandWriter& commands,
              BufferWriter& data,
              AnimationTarget target,
              uint32_t targetId,
              AnimationProperty property,
              const std::vector<float>& times,
              const void* values,
              size_t valueBytes,
              uint32_t stride)
{
    if (times.empty() || values == nullptr || valueBytes == 0) {
        return;
    }
    const uint32_t timesOffset =
      data.appendBytes(times.data(), times.size() * sizeof(float), 4);
    const uint32_t valuesOffset = data.appendBytes(values, valueBytes, 4);
    const uint32_t record = commands.begin(Command::Animation);
    commands.buffer.u32(static_cast<uint32_t>(target));
    commands.buffer.u32(targetId);
    commands.buffer.u32(static_cast<uint32_t>(property));
    commands.buffer.u32(0);
    commands.buffer.u32(static_cast<uint32_t>(times.size()));
    commands.buffer.u32(timesOffset);
    commands.buffer.u32(valuesOffset);
    commands.buffer.u32(stride);
    commands.end(record);
}

bool
packScene(const SceneData& scene, SceneBuffers& result)
{
    CommandWriter commands;
    BufferWriter data;

    const uint32_t sceneRecord = commands.begin(Command::Scene);
    commands.buffer.u32(scene.upAxis == UsdGeomTokens->z ? 1 : 0);
    commands.buffer.f32(
      scene.metersPerUnit > 0.0 ? static_cast<float>(scene.metersPerUnit) : 1.0f);
    commands.buffer.f32(static_cast<float>(scene.timeCodesPerSecond));
    commands.end(sceneRecord);
    emitMaterials(commands, data, scene);

    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
        const NodeData& node = scene.nodes[nodeIndex];
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, node.name, nameLength);
        const uint32_t matrixOffset = appendMatrix(data, node.localTransform);
        const uint32_t record = commands.begin(Command::TransformNode);
        commands.buffer.u32(static_cast<uint32_t>(nodeIndex + 1));
        commands.buffer.u32(node.parentId);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(matrixOffset);
        commands.end(record);
    }

    std::vector<std::vector<uint32_t>> boneIds(scene.skeletons.size());
    uint32_t nextBoneId = 1;
    for (size_t skeletonIndex = 0; skeletonIndex < scene.skeletons.size();
         ++skeletonIndex) {
        const SkeletonData& skeleton = scene.skeletons[skeletonIndex];
        boneIds[skeletonIndex].resize(skeleton.joints.size());
        struct PendingJoint
        {
            uint32_t parent;
            uint32_t boneId;
            uint32_t nameOffset;
            uint32_t nameLength;
            uint32_t restMatrixOffset;
            uint32_t bindMatrixOffset;
        };
        std::vector<PendingJoint> pending;
        pending.reserve(skeleton.joints.size());
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            const uint32_t boneId = nextBoneId++;
            boneIds[skeletonIndex][jointIndex] = boneId;
            uint32_t nameLength = 0;
            const uint32_t nameOffset =
              appendString(data, skeleton.joints[jointIndex].GetString(), nameLength);
            const uint32_t restMatrixOffset =
              appendMatrix(data, skeleton.restTransforms[jointIndex]);
            const uint32_t bindMatrixOffset =
              appendMatrix(data, skeleton.bindTransforms[jointIndex]);
            pending.push_back({
                skeleton.parents[jointIndex] >= 0
                  ? static_cast<uint32_t>(skeleton.parents[jointIndex])
                  : kMissingOffset,
                boneId,
                nameOffset,
                nameLength,
                restMatrixOffset,
                bindMatrixOffset,
            });
        }
        data.align();
        const uint32_t actualJointsOffset = data.size();
        for (const PendingJoint& joint : pending) {
            data.u32(joint.parent);
            data.u32(joint.boneId);
            data.u32(joint.nameOffset);
            data.u32(joint.nameLength);
            data.u32(joint.restMatrixOffset);
            data.u32(joint.bindMatrixOffset);
        }
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, skeleton.name, nameLength);
        const uint32_t record = commands.begin(Command::Skeleton);
        commands.buffer.u32(static_cast<uint32_t>(skeletonIndex + 1));
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(static_cast<uint32_t>(skeleton.joints.size()));
        commands.buffer.u32(actualJointsOffset);
        commands.end(record);
    }

    std::vector<std::vector<uint32_t>> morphTargetIds(scene.meshes.size());
    uint32_t nextMorphTargetId = 1;
    for (size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        const MeshData& mesh = scene.meshes[meshIndex];
        const uint32_t positionsOffset = appendArray(data, mesh.positions);
        const uint32_t normalsOffset = appendArray(data, mesh.normals);
        const uint32_t uvOffset = appendUvs(data, mesh.uvs);
        const uint32_t colorsOffset = appendArray(data, mesh.colors);
        uint32_t joints0Offset = kMissingOffset;
        uint32_t joints1Offset = kMissingOffset;
        uint32_t weights0Offset = kMissingOffset;
        uint32_t weights1Offset = kMissingOffset;
        if (!mesh.joints.empty()) {
            data.align();
            joints0Offset = data.size();
            for (const JointSet& joints : mesh.joints) {
                for (size_t influence = 0; influence < 4; ++influence) {
                    data.u16(joints[influence]);
                }
            }
            if (mesh.influenceCount > 4) {
                data.align();
                joints1Offset = data.size();
                for (const JointSet& joints : mesh.joints) {
                    for (size_t influence = 4; influence < 8; ++influence) {
                        data.u16(joints[influence]);
                    }
                }
            }
        }
        if (!mesh.weights.empty()) {
            data.align();
            weights0Offset = data.size();
            for (const WeightSet& weights : mesh.weights) {
                for (size_t influence = 0; influence < 4; ++influence) {
                    data.f32(weights[influence]);
                }
            }
            if (mesh.influenceCount > 4) {
                data.align();
                weights1Offset = data.size();
                for (const WeightSet& weights : mesh.weights) {
                    for (size_t influence = 4; influence < 8; ++influence) {
                        data.f32(weights[influence]);
                    }
                }
            }
        }
        const uint32_t indicesOffset = appendArray(data, mesh.indices);
        uint32_t geometryFlags = GeometryHasNormals;
        geometryFlags |= !mesh.uvs.empty() ? GeometryHasUv0 : 0;
        geometryFlags |= !mesh.colors.empty() ? GeometryHasColors : 0;
        geometryFlags |= joints0Offset != kMissingOffset ? GeometryHasSkin0 : 0;
        geometryFlags |= joints1Offset != kMissingOffset ? GeometryHasSkin1 : 0;
        const uint32_t geometryRecord = commands.begin(Command::Geometry);
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(static_cast<uint32_t>(mesh.positions.size()));
        commands.buffer.u32(static_cast<uint32_t>(mesh.indices.size()));
        commands.buffer.u32(geometryFlags);
        commands.buffer.u32(positionsOffset);
        commands.buffer.u32(normalsOffset);
        commands.buffer.u32(kMissingOffset);
        commands.buffer.u32(uvOffset);
        commands.buffer.u32(colorsOffset);
        commands.buffer.u32(joints0Offset);
        commands.buffer.u32(weights0Offset);
        commands.buffer.u32(joints1Offset);
        commands.buffer.u32(weights1Offset);
        commands.buffer.u32(indicesOffset);
        commands.buffer.u32(mesh.influenceCount);
        commands.end(geometryRecord);

        data.align();
        const uint32_t submeshesOffset = data.size();
        for (const SubmeshData& submesh : mesh.submeshes) {
            data.u32(submesh.materialId);
            data.u32(submesh.indexStart);
            data.u32(submesh.indexCount);
            data.u32(0);
            data.u32(static_cast<uint32_t>(mesh.positions.size()));
        }
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, mesh.name, nameLength);
        const uint32_t meshRecord = commands.begin(Command::Mesh);
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(mesh.nodeIds.front());
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(mesh.submeshes.size() == 1
                              ? mesh.submeshes.front().materialId
                              : kMissingOffset);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        uint32_t meshFlags = mesh.doubleSided ? MeshDoubleSided : 0;
        meshFlags |= mesh.leftHanded ? MeshLeftHanded : 0;
        commands.buffer.u32(meshFlags);
        commands.buffer.u32(mesh.skeletonId);
        commands.buffer.u32(submeshesOffset);
        commands.buffer.u32(static_cast<uint32_t>(mesh.submeshes.size()));
        commands.end(meshRecord);
        ++result.meshCount;

        morphTargetIds[meshIndex].reserve(mesh.morphTargets.size());
        for (const MorphTargetData& target : mesh.morphTargets) {
            const uint32_t targetId = nextMorphTargetId++;
            morphTargetIds[meshIndex].push_back(targetId);
            const uint32_t positionsOffset =
              appendArray(data, target.positions);
            const uint32_t normalsOffset =
              appendArray(data, target.normals);
            uint32_t targetNameLength = 0;
            const uint32_t targetNameOffset =
              appendString(data, target.name, targetNameLength);
            const uint32_t targetRecord =
              commands.begin(Command::MorphTarget);
            commands.buffer.u32(targetId);
            commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
            commands.buffer.u32(targetNameOffset);
            commands.buffer.u32(targetNameLength);
            commands.buffer.u32(
              static_cast<uint32_t>(target.positions.size()));
            commands.buffer.u32(positionsOffset);
            commands.buffer.u32(normalsOffset);
            commands.buffer.f32(target.influence);
            commands.end(targetRecord);
        }

        for (size_t placement = 1; placement < mesh.nodeIds.size(); ++placement) {
            const std::string name = mesh.name + " instance";
            uint32_t instanceNameLength = 0;
            const uint32_t instanceNameOffset =
              appendString(data, name, instanceNameLength);
            const uint32_t instanceRecord = commands.begin(Command::Instance);
            commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
            commands.buffer.u32(mesh.nodeIds[placement]);
            commands.buffer.u32(instanceNameOffset);
            commands.buffer.u32(instanceNameLength);
            commands.end(instanceRecord);
            ++result.instanceCount;
        }

        result.vertexCount += mesh.positions.size();
        result.triangleCount += mesh.indices.size() / 3;
    }

    for (size_t primitiveIndex = 0;
         primitiveIndex < scene.analyticPrimitives.size();
         ++primitiveIndex) {
        const AnalyticPrimitiveData& primitive =
          scene.analyticPrimitives[primitiveIndex];
        const uint32_t meshId =
          static_cast<uint32_t>(scene.meshes.size() + primitiveIndex + 1);
        uint32_t nameLength = 0;
        const uint32_t nameOffset =
          appendString(data, primitive.name, nameLength);
        const uint32_t record = commands.begin(Command::AnalyticPrimitive);
        commands.buffer.u32(meshId);
        commands.buffer.u32(primitive.nodeIds.front());
        commands.buffer.u32(static_cast<uint32_t>(primitive.type));
        commands.buffer.u32(primitive.materialId);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(primitive.flags);
        commands.buffer.u32(static_cast<uint32_t>(primitive.axis));
        commands.buffer.f32(primitive.sizeOrRadius);
        commands.buffer.f32(primitive.height);
        commands.buffer.u32(primitive.tessellation);
        commands.end(record);

        for (size_t placement = 1; placement < primitive.nodeIds.size(); ++placement) {
            const std::string instanceName = primitive.name + " instance";
            uint32_t instanceNameLength = 0;
            const uint32_t instanceNameOffset =
              appendString(data, instanceName, instanceNameLength);
            const uint32_t instanceRecord = commands.begin(Command::Instance);
            commands.buffer.u32(meshId);
            commands.buffer.u32(primitive.nodeIds[placement]);
            commands.buffer.u32(instanceNameOffset);
            commands.buffer.u32(instanceNameLength);
            commands.end(instanceRecord);
            ++result.instanceCount;
        }
    }

    for (const ThinInstanceData& batch : scene.thinInstances) {
        if (batch.transforms.empty()) {
            continue;
        }
        data.align();
        const uint32_t transformsOffset = data.size();
        for (const GfMatrix4d& transform : batch.transforms) {
            appendMatrix(data, transform);
        }
        const uint32_t sourceId =
          batch.analyticSource
            ? static_cast<uint32_t>(
                scene.meshes.size() + batch.sourceIndex + 1)
            : static_cast<uint32_t>(batch.sourceIndex + 1);
        const uint32_t record = commands.begin(Command::ThinInstances);
        commands.buffer.u32(sourceId);
        commands.buffer.u32(transformsOffset);
        commands.buffer.u32(static_cast<uint32_t>(batch.transforms.size()));
        commands.end(record);
        result.instanceCount += batch.transforms.size();
    }

    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
        const NodeAnimation& animation = scene.nodes[nodeIndex].animation;
        emitAnimation(commands,
                      data,
                      AnimationTarget::Node,
                      static_cast<uint32_t>(nodeIndex + 1),
                      AnimationProperty::Matrix,
                      animation.times,
                      animation.matrices.data(),
                      animation.matrices.size() * sizeof(GfMatrix4f),
                      16);
    }

    for (size_t skeletonIndex = 0; skeletonIndex < scene.skeletons.size();
         ++skeletonIndex) {
        const SkeletonData& skeleton = scene.skeletons[skeletonIndex];
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            std::vector<GfMatrix4f> matrices;
            matrices.reserve(skeleton.animation.localTransforms.size());
            for (const VtMatrix4dArray& sample : skeleton.animation.localTransforms) {
                matrices.emplace_back(sample[jointIndex]);
            }
            emitAnimation(commands,
                          data,
                          AnimationTarget::Bone,
                          boneIds[skeletonIndex][jointIndex],
                          AnimationProperty::Matrix,
                          skeleton.animation.times,
                          matrices.data(),
                          matrices.size() * sizeof(GfMatrix4f),
                          16);
        }
    }

    for (size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        const MeshData& mesh = scene.meshes[meshIndex];
        for (size_t targetIndex = 0;
             targetIndex < mesh.morphTargets.size();
             ++targetIndex) {
            const MorphTargetAnimation& animation =
              mesh.morphTargets[targetIndex].animation;
            emitAnimation(commands,
                          data,
                          AnimationTarget::MorphTarget,
                          morphTargetIds[meshIndex][targetIndex],
                          AnimationProperty::Influence,
                          animation.times,
                          animation.influences.data(),
                          animation.influences.size() * sizeof(float),
                          1);
        }
    }

    result.commands = commands.finish();
    result.data = std::move(data.bytes);
    result.nodeCount = static_cast<uint32_t>(scene.nodes.size());
    result.analyticPrimitiveCount =
      static_cast<uint32_t>(scene.analyticPrimitives.size());
    result.materialCount =
      static_cast<uint32_t>(scene.materials.empty() ? 0 : scene.materials.size() - 1);
    return true;
}

} // namespace

bool
buildSceneBuffers(const UsdStageRefPtr& stage, SceneBuffers& result)
{
    if (!stage) {
        return false;
    }

    const auto readStarted = std::chrono::steady_clock::now();
    SceneData scene;
    if (!extractStage(stage, scene)) {
        return false;
    }
    const auto readFinished = std::chrono::steady_clock::now();

    for (MeshData& mesh : scene.meshes) {
        optimizeMesh(mesh);
    }
    const auto preparationFinished = std::chrono::steady_clock::now();

    if (!packScene(scene, result)) {
        return false;
    }
    const auto packingFinished = std::chrono::steady_clock::now();

    result.stageReadMs =
      std::chrono::duration<double, std::milli>(readFinished - readStarted).count();
    result.preparationMs =
      std::chrono::duration<double, std::milli>(preparationFinished - readFinished)
        .count();
    result.packingMs =
      std::chrono::duration<double, std::milli>(packingFinished - preparationFinished)
        .count();
    return true;
}

} // namespace babylon::usd_importer
