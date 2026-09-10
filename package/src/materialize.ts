import { Animation } from "@babylonjs/core/Animations/animation.js";
import { AnimationGroup } from "@babylonjs/core/Animations/animationGroup.js";
import { AssetContainer } from "@babylonjs/core/assetContainer.js";
import { Bone } from "@babylonjs/core/Bones/bone.js";
import { Skeleton } from "@babylonjs/core/Bones/skeleton.js";
import { ShaderStore } from "@babylonjs/core/Engines/shaderStore.js";
import { Color3, Color4 } from "@babylonjs/core/Maths/math.color.js";
import {
    Matrix,
    Quaternion,
    Vector3,
} from "@babylonjs/core/Maths/math.vector.js";
import { Material } from "@babylonjs/core/Materials/material.js";
import { MultiMaterial } from "@babylonjs/core/Materials/multiMaterial.js";
import { PBRMaterial } from "@babylonjs/core/Materials/PBR/pbrMaterial.js";
import type { BaseTexture } from "@babylonjs/core/Materials/Textures/baseTexture.js";
import { MorphTarget } from "@babylonjs/core/Morph/morphTarget.js";
import { MorphTargetManager } from "@babylonjs/core/Morph/morphTargetManager.js";
import { Texture } from "@babylonjs/core/Materials/Textures/texture.js";
import {
    ChannelMask,
    CreateFactorOperand,
    CreateTextureOperand,
    LerpTexturesAsync,
    TextureChannel,
    TextureColorSpace,
} from "@babylonjs/core/Materials/Textures/textureProcessor.js";
import { CreateBox } from "@babylonjs/core/Meshes/Builders/boxBuilder.js";
import { CreateCylinder } from "@babylonjs/core/Meshes/Builders/cylinderBuilder.js";
import { CreateSphere } from "@babylonjs/core/Meshes/Builders/sphereBuilder.js";
import { Mesh } from "@babylonjs/core/Meshes/mesh.js";
import "@babylonjs/core/Meshes/instancedMesh.js";
import "@babylonjs/core/Meshes/thinInstanceMesh.js";
import { SubMesh } from "@babylonjs/core/Meshes/subMesh.js";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode.js";
import { VertexData } from "@babylonjs/core/Meshes/mesh.vertexData.js";
import type { Scene } from "@babylonjs/core/scene.js";
import { pbrPixelShader } from "@babylonjs/core/Shaders/pbr.fragment.js";
import { pbrVertexShader } from "@babylonjs/core/Shaders/pbr.vertex.js";

import {
    AnalyticPrimitiveType,
    AnimationProperty,
    AnimationTarget,
    Command,
    GeometryFlags,
    MaterialFlags,
    MeshFlags,
    MISSING_OFFSET,
    PayloadReader,
    PrimitiveAxis,
    TextureOutputChannel,
    TextureSourceColorSpace,
    readCommands,
} from "./protocol.js";

interface GeometryDescriptor {
    vertexCount: number;
    indexCount: number;
    flags: number;
    positions: number;
    normals: number;
    tangents: number;
    uv0: number;
    colors: number;
    joints0: number;
    weights0: number;
    joints1: number;
    weights1: number;
    indices: number;
    influences: number;
}

type Float4 = readonly [number, number, number, number];

interface TextureDescriptor {
    id: number;
    texture: Texture;
    sourceColorSpace: TextureSourceColorSpace;
    scale: Float4;
    bias: Float4;
}

interface MaterialTextureBinding {
    descriptor: TextureDescriptor;
    channel: TextureOutputChannel;
}

export interface MaterializationResult {
    container: AssetContainer;
    materializeMs: number;
}

const decoder = new TextDecoder();
const NODE_MATRIX_PROPERTY = "__babylonUsdLocalMatrix";

function assertRange(
    data: ArrayBuffer,
    offset: number,
    count: number,
    bytesPerElement: number,
    label: string,
): void {
    if (
        !Number.isInteger(offset) ||
        !Number.isInteger(count) ||
        offset < 0 ||
        count < 0 ||
        offset % Math.min(bytesPerElement, 4) !== 0 ||
        count > Math.floor((data.byteLength - offset) / bytesPerElement)
    ) {
        throw new Error(
            `Invalid ${label} range in OpenUSD Babylon data buffer.`,
        );
    }
}

function stringAt(data: ArrayBuffer, offset: number, length: number): string {
    assertRange(data, offset, length, 1, "string");
    return decoder.decode(new Uint8Array(data, offset, length));
}

function matrixAt(data: ArrayBuffer, offset: number): Matrix {
    assertRange(data, offset, 16, 4, "matrix");
    const values = new Float32Array(data, offset, 16);
    return Matrix.FromValues(
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7],
        values[8],
        values[9],
        values[10],
        values[11],
        values[12],
        values[13],
        values[14],
        values[15],
    );
}

function float4At(data: ArrayBuffer, offset: number, label: string): Float4 {
    assertRange(data, offset, 4, 4, label);
    const values = new Float32Array(data, offset, 4);
    return [values[0], values[1], values[2], values[3]];
}

function closeTo(left: number, right: number): boolean {
    return Math.abs(left - right) <= 1e-6;
}

function processorChannel(channel: TextureOutputChannel): TextureChannel {
    switch (channel) {
        case TextureOutputChannel.R:
            return TextureChannel.R;
        case TextureOutputChannel.G:
            return TextureChannel.G;
        case TextureOutputChannel.B:
            return TextureChannel.B;
        case TextureOutputChannel.A:
            return TextureChannel.A;
        case TextureOutputChannel.RGB:
            return TextureChannel.RGBA;
        default:
            throw new Error(`Invalid texture output channel ${channel}.`);
    }
}

function channelComponent(
    values: Float4,
    channel: TextureOutputChannel,
): number {
    if (channel < TextureOutputChannel.R || channel > TextureOutputChannel.A) {
        throw new Error(`Texture channel ${channel} is not scalar.`);
    }
    return values[channel as 0 | 1 | 2 | 3];
}

function bindingValue(values: Float4, channel: TextureOutputChannel): Float4 {
    if (channel === TextureOutputChannel.RGB) {
        return values;
    }
    const value = channelComponent(values, channel);
    return [value, value, value, value];
}

function processorColorSpace(descriptor: TextureDescriptor): TextureColorSpace {
    if (descriptor.sourceColorSpace === TextureSourceColorSpace.SRGB) {
        return TextureColorSpace.SRGB;
    }
    if (descriptor.sourceColorSpace === TextureSourceColorSpace.Raw) {
        return TextureColorSpace.Linear;
    }
    return descriptor.texture.gammaSpace
        ? TextureColorSpace.SRGB
        : TextureColorSpace.Linear;
}

function hasLinearSource(descriptor: TextureDescriptor): boolean {
    return processorColorSpace(descriptor) === TextureColorSpace.Linear;
}

function mimeType(value: number): string {
    switch (value) {
        case 2:
            return "image/jpeg";
        case 3:
            return "image/bmp";
        case 4:
            return "image/webp";
        default:
            return "image/png";
    }
}

function wrapMode(value: number): number {
    switch (value) {
        case 0:
            return Texture.CLAMP_ADDRESSMODE;
        case 1:
            return Texture.WRAP_ADDRESSMODE;
        case 2:
            return Texture.MIRROR_ADDRESSMODE;
        default:
            throw new Error(`Invalid texture wrap mode ${value}.`);
    }
}

function ensurePbrShaders(): void {
    ShaderStore.ShadersStore[pbrVertexShader.name] ??= pbrVertexShader.shader;
    ShaderStore.ShadersStore[pbrPixelShader.name] ??= pbrPixelShader.shader;
}

export async function materializeCommandBuffers(
    scene: Scene,
    commandBuffer: ArrayBuffer,
    dataBuffer: ArrayBuffer,
    addToScene: boolean,
): Promise<MaterializationResult> {
    const started = performance.now();
    ensurePbrShaders();
    const container = new AssetContainer(scene);
    const commands = readCommands(commandBuffer);
    const nodes = new Map<number, TransformNode>();
    const pendingParents: Array<{ node: TransformNode; parentId: number }> = [];
    const textures = new Map<number, TextureDescriptor>();
    const processedTextures = new Map<string, Promise<BaseTexture>>();
    const materials = new Map<number, PBRMaterial>();
    const doubleSidedMaterials = new Map<number, PBRMaterial>();
    const skeletons = new Map<number, Skeleton>();
    const bones = new Map<number, Bone>();
    const geometries = new Map<number, GeometryDescriptor>();
    const meshes = new Map<number, Mesh>();
    const morphTargetManagers = new Map<number, MorphTargetManager>();
    const morphTargets = new Map<number, MorphTarget>();
    const classicInstanceSources = new Set<number>();
    const thinInstanceSources = new Set<number>();
    const animationGroups = new Map<number, AnimationGroup>();
    const textureLoads: Promise<void>[] = [];
    let root: TransformNode | undefined;
    let timeCodesPerSecond = 24;

    const applyMeshOrientation = (mesh: Mesh, flags: number): void => {
        const sourceIsRightHanded = !(flags & MeshFlags.LeftHanded);
        mesh.sideOrientation =
            scene.useRightHandedSystem === sourceIsRightHanded
                ? Material.CounterClockWiseSideOrientation
                : Material.ClockWiseSideOrientation;
    };
    const materialForMesh = (
        id: number,
        doubleSided: boolean,
    ): PBRMaterial | null => {
        const material = materials.get(id);
        if (!material || !doubleSided || !material.backFaceCulling) {
            return material ?? null;
        }
        let variant = doubleSidedMaterials.get(id);
        if (!variant) {
            variant = material.clone(`${material.name} (double-sided)`);
            variant.backFaceCulling = false;
            variant.twoSidedLighting = true;
            doubleSidedMaterials.set(id, variant);
            container.materials.push(variant);
        }
        return variant;
    };
    const textureBinding = (
        textureId: number,
        channel: number,
        label: string,
    ): MaterialTextureBinding | undefined => {
        if (textureId === MISSING_OFFSET) {
            if (channel !== MISSING_OFFSET) {
                throw new Error(`${label} has a channel but no texture.`);
            }
            return undefined;
        }
        if (
            channel < TextureOutputChannel.R ||
            channel > TextureOutputChannel.RGB
        ) {
            throw new Error(`${label} has invalid output channel ${channel}.`);
        }
        const descriptor = textures.get(textureId);
        if (!descriptor) {
            throw new Error(
                `${label} references missing texture ${textureId}.`,
            );
        }
        return {
            descriptor,
            channel: channel as TextureOutputChannel,
        };
    };
    const processValueTexture = (
        binding: MaterialTextureBinding,
        scale: Float4,
        bias: Float4,
        outputMask: ChannelMask,
        name: string,
    ): Promise<BaseTexture> => {
        const key = [
            binding.descriptor.id,
            binding.channel,
            outputMask,
            ...scale,
            ...bias,
        ].join(":");
        let processed = processedTextures.get(key);
        if (!processed) {
            processed = (async () => {
                const result = await LerpTexturesAsync(
                    name,
                    CreateFactorOperand(
                        new Color4(bias[0], bias[1], bias[2], bias[3]),
                    ),
                    CreateFactorOperand(
                        new Color4(
                            bias[0] + scale[0],
                            bias[1] + scale[1],
                            bias[2] + scale[2],
                            bias[3] + scale[3],
                        ),
                    ),
                    CreateTextureOperand(
                        binding.descriptor.texture,
                        processorChannel(binding.channel),
                        processorColorSpace(binding.descriptor),
                    ),
                    scene,
                    TextureColorSpace.Linear,
                    outputMask,
                );
                if (!result.texture) {
                    throw new Error(
                        `Texture processor returned no texture for '${name}'.`,
                    );
                }
                if (!(result.texture instanceof Texture)) {
                    throw new Error(
                        `Texture processor returned an unsupported texture for '${name}'.`,
                    );
                }
                const source = binding.descriptor.texture;
                const output = result.texture;
                output.name = name;
                output.coordinatesIndex = source.coordinatesIndex;
                output.wrapU = source.wrapU;
                output.wrapV = source.wrapV;
                output.uScale = source.uScale;
                output.vScale = source.vScale;
                output.uOffset = source.uOffset;
                output.vOffset = source.vOffset;
                output.uAng = source.uAng;
                output.vAng = source.vAng;
                output.wAng = source.wAng;
                output.uRotationCenter = 0;
                output.vRotationCenter = 0;
                container.textures.push(output);
                return output;
            })();
            processedTextures.set(key, processed);
        }
        return processed;
    };
    const processScalarTexture = (
        binding: MaterialTextureBinding,
        name: string,
    ): Promise<BaseTexture> => {
        const scale = channelComponent(
            binding.descriptor.scale,
            binding.channel,
        );
        const bias = channelComponent(binding.descriptor.bias, binding.channel);
        return processValueTexture(
            binding,
            [scale, scale, scale, scale],
            [bias, bias, bias, bias],
            ChannelMask.RGB,
            name,
        );
    };

    try {
        for (const command of commands) {
            const payload = new PayloadReader(
                commandBuffer,
                command.payloadOffset,
                command.payloadLength,
            );
            switch (command.opcode) {
                case Command.Scene: {
                    const zUp = payload.u32() === 1;
                    const metersPerUnit = payload.f32();
                    timeCodesPerSecond = payload.f32() || 24;
                    root = new TransformNode("USD Root", scene);
                    root.rotationQuaternion = zUp
                        ? Quaternion.FromArray([-0.7071068, 0, 0, 0.7071068])
                        : Quaternion.Identity();
                    root.scaling.copyFrom(
                        scene.useRightHandedSystem
                            ? new Vector3(
                                  metersPerUnit,
                                  metersPerUnit,
                                  metersPerUnit,
                              )
                            : zUp
                              ? new Vector3(
                                    metersPerUnit,
                                    -metersPerUnit,
                                    metersPerUnit,
                                )
                              : new Vector3(
                                    metersPerUnit,
                                    metersPerUnit,
                                    -metersPerUnit,
                                ),
                    );
                    container.transformNodes.push(root);
                    container.rootNodes.push(root);
                    break;
                }
                case Command.Texture: {
                    const id = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const mime = mimeType(payload.u32());
                    const imageOffset = payload.u32();
                    const imageLength = payload.u32();
                    const coordinatesIndex = payload.u32();
                    const transformOffset = payload.u32();
                    const wrapU = payload.u32();
                    const wrapV = payload.u32();
                    const sourceColorSpace =
                        payload.u32() as TextureSourceColorSpace;
                    const valueTransformOffset = payload.u32();
                    if (
                        sourceColorSpace < TextureSourceColorSpace.Auto ||
                        sourceColorSpace > TextureSourceColorSpace.SRGB
                    ) {
                        throw new Error(
                            `Invalid texture source color space ${sourceColorSpace}.`,
                        );
                    }
                    assertRange(
                        dataBuffer,
                        transformOffset,
                        5,
                        4,
                        "texture transform",
                    );
                    assertRange(
                        dataBuffer,
                        imageOffset,
                        imageLength,
                        1,
                        "texture image",
                    );
                    const transform = new Float32Array(
                        dataBuffer,
                        transformOffset,
                        5,
                    );
                    const scale = float4At(
                        dataBuffer,
                        valueTransformOffset,
                        "texture value scale",
                    );
                    const bias = float4At(
                        dataBuffer,
                        valueTransformOffset + 16,
                        "texture value bias",
                    );
                    const bytes = new Uint8Array(
                        dataBuffer,
                        imageOffset,
                        imageLength,
                    );
                    const url = URL.createObjectURL(
                        new Blob([bytes], { type: mime }),
                    );
                    let resolveLoad!: () => void;
                    let rejectLoad!: (error: Error) => void;
                    const loaded = new Promise<void>((resolve, reject) => {
                        resolveLoad = resolve;
                        rejectLoad = reject;
                    });
                    const texture = new Texture(
                        url,
                        scene,
                        false,
                        false,
                        Texture.TRILINEAR_SAMPLINGMODE,
                        resolveLoad,
                        (message, exception) => {
                            URL.revokeObjectURL(url);
                            rejectLoad(
                                exception instanceof Error
                                    ? exception
                                    : new Error(
                                          message ||
                                              `Could not load texture '${url}'.`,
                                      ),
                            );
                        },
                    );
                    textureLoads.push(loaded);
                    texture.name = stringAt(dataBuffer, nameOffset, nameLength);
                    texture.coordinatesIndex = coordinatesIndex;
                    texture.uScale = transform[0];
                    texture.vScale = transform[1];
                    texture.uRotationCenter = 0;
                    texture.vRotationCenter = 0;
                    texture.uOffset =
                        transform[2] - transform[1] * Math.sin(transform[4]);
                    texture.vOffset =
                        1 -
                        transform[1] * Math.cos(transform[4]) -
                        transform[3];
                    texture.wAng = -transform[4];
                    texture.wrapU = wrapMode(wrapU);
                    texture.wrapV = wrapMode(wrapV);
                    if (sourceColorSpace === TextureSourceColorSpace.Raw) {
                        texture.gammaSpace = false;
                    } else if (
                        sourceColorSpace === TextureSourceColorSpace.SRGB
                    ) {
                        texture.gammaSpace = true;
                    }
                    texture.onDisposeObservable.addOnce(() =>
                        URL.revokeObjectURL(url),
                    );
                    textures.set(id, {
                        id,
                        texture,
                        sourceColorSpace,
                        scale,
                        bias,
                    });
                    container.textures.push(texture);
                    break;
                }
                case Command.Material: {
                    const id = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const baseOffset = payload.u32();
                    const emissiveOffset = payload.u32();
                    const metallic = payload.f32();
                    const roughness = payload.f32();
                    const normalScale = payload.f32();
                    const alphaCutoff = payload.f32();
                    const flags = payload.u32();
                    const textureIds = Array.from({ length: 7 }, () =>
                        payload.u32(),
                    );
                    const channels = Array.from({ length: 7 }, () =>
                        payload.u32(),
                    );
                    assertRange(
                        dataBuffer,
                        baseOffset,
                        4,
                        4,
                        "material base color",
                    );
                    assertRange(
                        dataBuffer,
                        emissiveOffset,
                        3,
                        4,
                        "material emissive color",
                    );
                    const base = new Float32Array(dataBuffer, baseOffset, 4);
                    const emissive = new Float32Array(
                        dataBuffer,
                        emissiveOffset,
                        3,
                    );
                    await Promise.all(textureLoads);
                    const baseTexture = textureBinding(
                        textureIds[0],
                        channels[0],
                        "base-color texture",
                    );
                    const opacityTexture = textureBinding(
                        textureIds[1],
                        channels[1],
                        "opacity texture",
                    );
                    const normalTexture = textureBinding(
                        textureIds[2],
                        channels[2],
                        "normal texture",
                    );
                    const metallicTexture = textureBinding(
                        textureIds[3],
                        channels[3],
                        "metallic texture",
                    );
                    const roughnessTexture = textureBinding(
                        textureIds[4],
                        channels[4],
                        "roughness texture",
                    );
                    const occlusionTexture = textureBinding(
                        textureIds[5],
                        channels[5],
                        "occlusion texture",
                    );
                    const emissiveTexture = textureBinding(
                        textureIds[6],
                        channels[6],
                        "emissive texture",
                    );
                    const material = new PBRMaterial(
                        stringAt(dataBuffer, nameOffset, nameLength),
                        scene,
                    );
                    material.id = `usd-material-${id}`;
                    material.albedoColor = new Color3(
                        base[0],
                        base[1],
                        base[2],
                    );
                    material.alpha = base[3];
                    material.metallic = metallic;
                    material.roughness = roughness;
                    material.emissiveColor = new Color3(
                        emissive[0],
                        emissive[1],
                        emissive[2],
                    );
                    material.useRoughnessFromMetallicTextureAlpha = false;
                    material.useRoughnessFromMetallicTextureGreen = false;
                    material.useMetallnessFromMetallicTextureBlue = false;
                    material.useAmbientOcclusionFromMetallicTextureRed = false;
                    const doubleSided = Boolean(
                        flags & MaterialFlags.DoubleSided,
                    );
                    material.backFaceCulling = !doubleSided;
                    material.twoSidedLighting = doubleSided;
                    material.unlit = Boolean(flags & MaterialFlags.Unlit);
                    if (flags & MaterialFlags.AlphaBlend) {
                        material.transparencyMode = 2;
                    }
                    if (alphaCutoff > 0) {
                        material.transparencyMode = 1;
                        material.alphaCutOff = alphaCutoff;
                    }

                    if (baseTexture) {
                        const scale = bindingValue(
                            baseTexture.descriptor.scale,
                            baseTexture.channel,
                        );
                        const bias = bindingValue(
                            baseTexture.descriptor.bias,
                            baseTexture.channel,
                        );
                        const identity =
                            baseTexture.channel === TextureOutputChannel.RGB &&
                            scale
                                .slice(0, 3)
                                .every((value) => closeTo(value, 1)) &&
                            bias
                                .slice(0, 3)
                                .every((value) => closeTo(value, 0));
                        material.albedoTexture = identity
                            ? baseTexture.descriptor.texture
                            : await processValueTexture(
                                  baseTexture,
                                  scale,
                                  bias,
                                  ChannelMask.RGB,
                                  `${material.name} base color`,
                              );
                    }
                    if (opacityTexture) {
                        const opacityScale = channelComponent(
                            opacityTexture.descriptor.scale,
                            opacityTexture.channel,
                        );
                        const opacityBias = channelComponent(
                            opacityTexture.descriptor.bias,
                            opacityTexture.channel,
                        );
                        if (
                            opacityTexture.channel === TextureOutputChannel.A &&
                            closeTo(opacityBias, 0)
                        ) {
                            material.opacityTexture =
                                opacityTexture.descriptor.texture;
                            material.opacityTexture.hasAlpha = true;
                            material.opacityTexture.getAlphaFromRGB = false;
                            material.alpha *= opacityScale;
                        } else {
                            material.opacityTexture =
                                await processScalarTexture(
                                    opacityTexture,
                                    `${material.name} opacity`,
                                );
                            material.opacityTexture.getAlphaFromRGB = true;
                        }
                    }
                    if (normalTexture) {
                        const sourceScale = bindingValue(
                            normalTexture.descriptor.scale,
                            normalTexture.channel,
                        );
                        const sourceBias = bindingValue(
                            normalTexture.descriptor.bias,
                            normalTexture.channel,
                        );
                        const canonical =
                            normalTexture.descriptor.sourceColorSpace ===
                                TextureSourceColorSpace.Raw &&
                            normalTexture.channel ===
                                TextureOutputChannel.RGB &&
                            sourceScale.every((value, index) =>
                                closeTo(value, [2, 2, 2, 1][index]),
                            ) &&
                            sourceBias.every((value, index) =>
                                closeTo(value, [-1, -1, -1, 0][index]),
                            );
                        if (canonical) {
                            material.bumpTexture =
                                normalTexture.descriptor.texture;
                        } else {
                            const adjustedScale: Float4 = [
                                sourceScale[0] / 2,
                                sourceScale[1] / 2,
                                sourceScale[2] / 2,
                                sourceScale[3] / 2,
                            ];
                            const adjustedBias: Float4 = [
                                (sourceBias[0] + 1) / 2,
                                (sourceBias[1] + 1) / 2,
                                (sourceBias[2] + 1) / 2,
                                (sourceBias[3] + 1) / 2,
                            ];
                            material.bumpTexture = await processValueTexture(
                                normalTexture,
                                adjustedScale,
                                adjustedBias,
                                ChannelMask.RGB,
                                `${material.name} normal`,
                            );
                        }
                        material.bumpTexture.level = normalScale;
                    }

                    let metallicHandled = false;
                    let roughnessHandled = false;
                    let occlusionHandled = false;
                    if (
                        metallicTexture &&
                        roughnessTexture &&
                        metallicTexture.descriptor.id ===
                            roughnessTexture.descriptor.id &&
                        hasLinearSource(metallicTexture.descriptor) &&
                        (metallicTexture.channel === TextureOutputChannel.R ||
                            metallicTexture.channel ===
                                TextureOutputChannel.B) &&
                        (roughnessTexture.channel === TextureOutputChannel.G ||
                            roughnessTexture.channel ===
                                TextureOutputChannel.A) &&
                        metallicTexture.descriptor.bias.every((value) =>
                            closeTo(value, 0),
                        )
                    ) {
                        material.metallicTexture =
                            metallicTexture.descriptor.texture;
                        material.useMetallnessFromMetallicTextureBlue =
                            metallicTexture.channel === TextureOutputChannel.B;
                        material.useRoughnessFromMetallicTextureGreen =
                            roughnessTexture.channel === TextureOutputChannel.G;
                        material.useRoughnessFromMetallicTextureAlpha =
                            roughnessTexture.channel === TextureOutputChannel.A;
                        material.metallic = channelComponent(
                            metallicTexture.descriptor.scale,
                            metallicTexture.channel,
                        );
                        material.roughness = channelComponent(
                            roughnessTexture.descriptor.scale,
                            roughnessTexture.channel,
                        );
                        metallicHandled = true;
                        roughnessHandled = true;
                        if (
                            occlusionTexture &&
                            occlusionTexture.descriptor.id ===
                                metallicTexture.descriptor.id &&
                            occlusionTexture.channel ===
                                TextureOutputChannel.R &&
                            closeTo(
                                channelComponent(
                                    occlusionTexture.descriptor.scale,
                                    occlusionTexture.channel,
                                ),
                                1,
                            ) &&
                            closeTo(
                                channelComponent(
                                    occlusionTexture.descriptor.bias,
                                    occlusionTexture.channel,
                                ),
                                0,
                            )
                        ) {
                            material.useAmbientOcclusionFromMetallicTextureRed = true;
                            material.ambientTextureStrength = 1;
                            occlusionHandled = true;
                        }
                    }
                    if (metallicTexture && !metallicHandled) {
                        const direct =
                            hasLinearSource(metallicTexture.descriptor) &&
                            (metallicTexture.channel ===
                                TextureOutputChannel.R ||
                                metallicTexture.channel ===
                                    TextureOutputChannel.B) &&
                            closeTo(
                                channelComponent(
                                    metallicTexture.descriptor.bias,
                                    metallicTexture.channel,
                                ),
                                0,
                            );
                        material.metallicTexture = direct
                            ? metallicTexture.descriptor.texture
                            : await processScalarTexture(
                                  metallicTexture,
                                  `${material.name} metallic`,
                              );
                        material.useMetallnessFromMetallicTextureBlue =
                            direct &&
                            metallicTexture.channel === TextureOutputChannel.B;
                        material.metallic = direct
                            ? channelComponent(
                                  metallicTexture.descriptor.scale,
                                  metallicTexture.channel,
                              )
                            : 1;
                    }
                    if (roughnessTexture && !roughnessHandled) {
                        const direct =
                            hasLinearSource(roughnessTexture.descriptor) &&
                            roughnessTexture.channel ===
                                TextureOutputChannel.R &&
                            closeTo(
                                channelComponent(
                                    roughnessTexture.descriptor.bias,
                                    roughnessTexture.channel,
                                ),
                                0,
                            );
                        material.microSurfaceTexture = direct
                            ? roughnessTexture.descriptor.texture
                            : await processScalarTexture(
                                  roughnessTexture,
                                  `${material.name} roughness`,
                              );
                        material.roughness = direct
                            ? channelComponent(
                                  roughnessTexture.descriptor.scale,
                                  roughnessTexture.channel,
                              )
                            : 1;
                    }
                    if (occlusionTexture && !occlusionHandled) {
                        const direct =
                            hasLinearSource(occlusionTexture.descriptor) &&
                            occlusionTexture.channel ===
                                TextureOutputChannel.R &&
                            closeTo(
                                channelComponent(
                                    occlusionTexture.descriptor.scale,
                                    occlusionTexture.channel,
                                ),
                                1,
                            ) &&
                            closeTo(
                                channelComponent(
                                    occlusionTexture.descriptor.bias,
                                    occlusionTexture.channel,
                                ),
                                0,
                            );
                        material.ambientTexture = direct
                            ? occlusionTexture.descriptor.texture
                            : await processScalarTexture(
                                  occlusionTexture,
                                  `${material.name} occlusion`,
                              );
                        material.useAmbientInGrayScale = true;
                        material.ambientTextureStrength = 1;
                    }
                    if (emissiveTexture) {
                        const scale = bindingValue(
                            emissiveTexture.descriptor.scale,
                            emissiveTexture.channel,
                        );
                        const bias = bindingValue(
                            emissiveTexture.descriptor.bias,
                            emissiveTexture.channel,
                        );
                        const identity =
                            emissiveTexture.channel ===
                                TextureOutputChannel.RGB &&
                            scale
                                .slice(0, 3)
                                .every((value) => closeTo(value, 1)) &&
                            bias
                                .slice(0, 3)
                                .every((value) => closeTo(value, 0));
                        material.emissiveTexture = identity
                            ? emissiveTexture.descriptor.texture
                            : await processValueTexture(
                                  emissiveTexture,
                                  scale,
                                  bias,
                                  ChannelMask.RGB,
                                  `${material.name} emissive`,
                              );
                    }
                    materials.set(id, material);
                    container.materials.push(material);
                    break;
                }
                case Command.TransformNode: {
                    const id = payload.u32();
                    const parentId = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const matrixOffset = payload.u32();
                    const node = new TransformNode(
                        stringAt(dataBuffer, nameOffset, nameLength),
                        scene,
                    );
                    node.setPreTransformMatrix(
                        matrixAt(dataBuffer, matrixOffset),
                    );
                    Object.defineProperty(node, NODE_MATRIX_PROPERTY, {
                        configurable: true,
                        get: () => node.getPivotMatrix(),
                        set: (matrix: Matrix) =>
                            node.setPreTransformMatrix(matrix),
                    });
                    nodes.set(id, node);
                    container.transformNodes.push(node);
                    if (parentId === MISSING_OFFSET) {
                        node.parent = root ?? null;
                    } else {
                        pendingParents.push({ node, parentId });
                    }
                    break;
                }
                case Command.Skeleton: {
                    const id = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const jointCount = payload.u32();
                    const jointsOffset = payload.u32();
                    const skeleton = new Skeleton(
                        stringAt(dataBuffer, nameOffset, nameLength),
                        `usd-skeleton-${id}`,
                        scene,
                    );
                    const jointView = new DataView(dataBuffer);
                    assertRange(
                        dataBuffer,
                        jointsOffset,
                        jointCount * 6,
                        4,
                        "skeleton joints",
                    );
                    const created: Bone[] = [];
                    for (let index = 0; index < jointCount; ++index) {
                        const offset = jointsOffset + index * 24;
                        const parentIndex = jointView.getUint32(offset, true);
                        const boneId = jointView.getUint32(offset + 4, true);
                        const jointNameOffset = jointView.getUint32(
                            offset + 8,
                            true,
                        );
                        const jointNameLength = jointView.getUint32(
                            offset + 12,
                            true,
                        );
                        const restMatrixOffset = jointView.getUint32(
                            offset + 16,
                            true,
                        );
                        const bindMatrixOffset = jointView.getUint32(
                            offset + 20,
                            true,
                        );
                        if (
                            parentIndex !== MISSING_OFFSET &&
                            parentIndex >= index
                        ) {
                            throw new Error(
                                `Skeleton ${id} has an invalid parent joint index.`,
                            );
                        }
                        const bone = new Bone(
                            stringAt(
                                dataBuffer,
                                jointNameOffset,
                                jointNameLength,
                            ),
                            skeleton,
                            parentIndex === MISSING_OFFSET
                                ? null
                                : created[parentIndex],
                            matrixAt(dataBuffer, restMatrixOffset),
                            matrixAt(dataBuffer, restMatrixOffset),
                            matrixAt(dataBuffer, bindMatrixOffset),
                            index,
                        );
                        created.push(bone);
                        bones.set(boneId, bone);
                    }
                    skeletons.set(id, skeleton);
                    container.skeletons.push(skeleton);
                    break;
                }
                case Command.Geometry: {
                    const id = payload.u32();
                    geometries.set(id, {
                        vertexCount: payload.u32(),
                        indexCount: payload.u32(),
                        flags: payload.u32(),
                        positions: payload.u32(),
                        normals: payload.u32(),
                        tangents: payload.u32(),
                        uv0: payload.u32(),
                        colors: payload.u32(),
                        joints0: payload.u32(),
                        weights0: payload.u32(),
                        joints1: payload.u32(),
                        weights1: payload.u32(),
                        indices: payload.u32(),
                        influences: payload.u32(),
                    });
                    break;
                }
                case Command.Mesh: {
                    const id = payload.u32();
                    const nodeId = payload.u32();
                    const geometryId = payload.u32();
                    const materialId = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const flags = payload.u32();
                    const skeletonId = payload.u32();
                    const submeshesOffset = payload.u32();
                    const submeshCount = payload.u32();
                    const descriptor = geometries.get(geometryId);
                    if (!descriptor) {
                        throw new Error(
                            `Mesh ${id} references missing geometry ${geometryId}.`,
                        );
                    }
                    const mesh = new Mesh(
                        stringAt(dataBuffer, nameOffset, nameLength),
                        scene,
                    );
                    applyMeshOrientation(mesh, flags);
                    mesh.parent = nodes.get(nodeId) ?? root ?? null;
                    const vertexData = new VertexData();
                    assertRange(
                        dataBuffer,
                        descriptor.positions,
                        descriptor.vertexCount * 3,
                        4,
                        "positions",
                    );
                    vertexData.positions = new Float32Array(
                        dataBuffer,
                        descriptor.positions,
                        descriptor.vertexCount * 3,
                    );
                    if (descriptor.flags & GeometryFlags.Normals) {
                        assertRange(
                            dataBuffer,
                            descriptor.normals,
                            descriptor.vertexCount * 3,
                            4,
                            "normals",
                        );
                        vertexData.normals = new Float32Array(
                            dataBuffer,
                            descriptor.normals,
                            descriptor.vertexCount * 3,
                        );
                    }
                    if (descriptor.flags & GeometryFlags.Tangents) {
                        assertRange(
                            dataBuffer,
                            descriptor.tangents,
                            descriptor.vertexCount * 4,
                            4,
                            "tangents",
                        );
                        vertexData.tangents = new Float32Array(
                            dataBuffer,
                            descriptor.tangents,
                            descriptor.vertexCount * 4,
                        );
                    }
                    if (descriptor.flags & GeometryFlags.Uv0) {
                        assertRange(
                            dataBuffer,
                            descriptor.uv0,
                            descriptor.vertexCount * 2,
                            4,
                            "texture coordinates",
                        );
                        vertexData.uvs = new Float32Array(
                            dataBuffer,
                            descriptor.uv0,
                            descriptor.vertexCount * 2,
                        );
                    }
                    if (descriptor.flags & GeometryFlags.Colors) {
                        assertRange(
                            dataBuffer,
                            descriptor.colors,
                            descriptor.vertexCount * 4,
                            4,
                            "vertex colors",
                        );
                        vertexData.colors = new Float32Array(
                            dataBuffer,
                            descriptor.colors,
                            descriptor.vertexCount * 4,
                        );
                    }
                    if (descriptor.flags & GeometryFlags.Skin0) {
                        assertRange(
                            dataBuffer,
                            descriptor.joints0,
                            descriptor.vertexCount * 4,
                            2,
                            "joint indices",
                        );
                        assertRange(
                            dataBuffer,
                            descriptor.weights0,
                            descriptor.vertexCount * 4,
                            4,
                            "joint weights",
                        );
                        vertexData.matricesIndices = Float32Array.from(
                            new Uint16Array(
                                dataBuffer,
                                descriptor.joints0,
                                descriptor.vertexCount * 4,
                            ),
                        );
                        vertexData.matricesWeights = new Float32Array(
                            dataBuffer,
                            descriptor.weights0,
                            descriptor.vertexCount * 4,
                        );
                    }
                    if (descriptor.flags & GeometryFlags.Skin1) {
                        assertRange(
                            dataBuffer,
                            descriptor.joints1,
                            descriptor.vertexCount * 4,
                            2,
                            "extra joint indices",
                        );
                        assertRange(
                            dataBuffer,
                            descriptor.weights1,
                            descriptor.vertexCount * 4,
                            4,
                            "extra joint weights",
                        );
                        vertexData.matricesIndicesExtra = Float32Array.from(
                            new Uint16Array(
                                dataBuffer,
                                descriptor.joints1,
                                descriptor.vertexCount * 4,
                            ),
                        );
                        vertexData.matricesWeightsExtra = new Float32Array(
                            dataBuffer,
                            descriptor.weights1,
                            descriptor.vertexCount * 4,
                        );
                    }
                    assertRange(
                        dataBuffer,
                        descriptor.indices,
                        descriptor.indexCount,
                        4,
                        "indices",
                    );
                    vertexData.indices = new Uint32Array(
                        dataBuffer,
                        descriptor.indices,
                        descriptor.indexCount,
                    );
                    vertexData.applyToMesh(mesh, true);
                    mesh.numBoneInfluencers = Math.min(
                        descriptor.influences,
                        8,
                    );
                    if (skeletonId !== MISSING_OFFSET) {
                        mesh.skeleton = skeletons.get(skeletonId) ?? null;
                    }

                    const submeshView = new DataView(dataBuffer);
                    assertRange(
                        dataBuffer,
                        submeshesOffset,
                        submeshCount * 5,
                        4,
                        "submeshes",
                    );
                    const doubleSided = Boolean(flags & MeshFlags.DoubleSided);
                    if (submeshCount === 1 && materialId !== MISSING_OFFSET) {
                        mesh.material = materialForMesh(
                            materialId,
                            doubleSided,
                        );
                    } else {
                        const multi = new MultiMaterial(
                            `${mesh.name} materials`,
                            scene,
                        );
                        for (let index = 0; index < submeshCount; ++index) {
                            const offset = submeshesOffset + index * 20;
                            multi.subMaterials.push(
                                materialForMesh(
                                    submeshView.getUint32(offset, true),
                                    doubleSided,
                                ),
                            );
                        }
                        mesh.material = multi;
                        container.multiMaterials.push(multi);
                    }
                    mesh.releaseSubMeshes();
                    for (let index = 0; index < submeshCount; ++index) {
                        const offset = submeshesOffset + index * 20;
                        new SubMesh(
                            index,
                            submeshView.getUint32(offset + 12, true),
                            submeshView.getUint32(offset + 16, true),
                            submeshView.getUint32(offset + 4, true),
                            submeshView.getUint32(offset + 8, true),
                            mesh,
                        );
                    }
                    meshes.set(id, mesh);
                    container.meshes.push(mesh);
                    if (mesh.geometry) {
                        container.geometries.push(mesh.geometry);
                    }
                    break;
                }
                case Command.MorphTarget: {
                    const id = payload.u32();
                    const meshId = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const vertexCount = payload.u32();
                    const positionsOffset = payload.u32();
                    const normalsOffset = payload.u32();
                    const influence = payload.f32();
                    const mesh = meshes.get(meshId);
                    if (!mesh) {
                        throw new Error(
                            `Morph target references missing mesh ${meshId}.`,
                        );
                    }
                    if (
                        morphTargets.has(id) ||
                        vertexCount !== mesh.getTotalVertices() ||
                        !Number.isFinite(influence)
                    ) {
                        throw new Error(
                            `Morph target ${id} has invalid metadata.`,
                        );
                    }
                    assertRange(
                        dataBuffer,
                        positionsOffset,
                        vertexCount * 3,
                        4,
                        "morph target positions",
                    );
                    let manager = morphTargetManagers.get(meshId);
                    if (!manager) {
                        manager = new MorphTargetManager(scene, mesh.name);
                        manager.areUpdatesFrozen = true;
                        container.morphTargetManagers.push(manager);
                        manager._parentContainer = container;
                        mesh.morphTargetManager = manager;
                        morphTargetManagers.set(meshId, manager);
                    }
                    const target = new MorphTarget(
                        stringAt(dataBuffer, nameOffset, nameLength),
                        influence,
                        scene,
                        manager,
                    );
                    target.id = `usd-morph-target-${id}`;
                    target.setPositions(
                        new Float32Array(
                            dataBuffer,
                            positionsOffset,
                            vertexCount * 3,
                        ),
                    );
                    if (normalsOffset !== MISSING_OFFSET) {
                        assertRange(
                            dataBuffer,
                            normalsOffset,
                            vertexCount * 3,
                            4,
                            "morph target normals",
                        );
                        target.setNormals(
                            new Float32Array(
                                dataBuffer,
                                normalsOffset,
                                vertexCount * 3,
                            ),
                        );
                    }
                    manager.addTarget(target);
                    morphTargets.set(id, target);
                    break;
                }
                case Command.AnalyticPrimitive: {
                    const id = payload.u32();
                    const nodeId = payload.u32();
                    const type = payload.u32();
                    const materialId = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const flags = payload.u32();
                    const axis = payload.u32();
                    const sizeOrRadius = payload.f32();
                    const height = payload.f32();
                    const tessellation = payload.u32();
                    const name = stringAt(dataBuffer, nameOffset, nameLength);
                    if (!Number.isFinite(sizeOrRadius) || sizeOrRadius <= 0) {
                        throw new Error(
                            `Analytic primitive ${id} has an invalid size or radius.`,
                        );
                    }
                    if (
                        (type === AnalyticPrimitiveType.Cylinder ||
                            type === AnalyticPrimitiveType.Cone) &&
                        (!Number.isFinite(height) || height <= 0)
                    ) {
                        throw new Error(
                            `Analytic primitive ${id} has an invalid height.`,
                        );
                    }
                    if (
                        type !== AnalyticPrimitiveType.Cube &&
                        (!Number.isInteger(tessellation) ||
                            tessellation < 3 ||
                            tessellation > 512)
                    ) {
                        throw new Error(
                            `Analytic primitive ${id} has invalid tessellation.`,
                        );
                    }
                    let mesh: Mesh;
                    switch (type) {
                        case AnalyticPrimitiveType.Cube:
                            mesh = CreateBox(
                                name,
                                { size: sizeOrRadius },
                                scene,
                            );
                            break;
                        case AnalyticPrimitiveType.Sphere:
                            mesh = CreateSphere(
                                name,
                                {
                                    diameter: sizeOrRadius * 2,
                                    segments: tessellation,
                                },
                                scene,
                            );
                            break;
                        case AnalyticPrimitiveType.Cylinder:
                        case AnalyticPrimitiveType.Cone:
                            mesh = CreateCylinder(
                                name,
                                {
                                    height,
                                    diameterTop:
                                        type === AnalyticPrimitiveType.Cone
                                            ? 0
                                            : sizeOrRadius * 2,
                                    diameterBottom: sizeOrRadius * 2,
                                    tessellation,
                                },
                                scene,
                            );
                            break;
                        default:
                            throw new Error(
                                `Unsupported analytic primitive type ${type}.`,
                            );
                    }

                    // Babylon builders emit left-handed local winding. Reverse only for USD's
                    // default right-handed convention; normals already point outward.
                    if (!(flags & MeshFlags.LeftHanded)) {
                        const sourceIndices = mesh.getIndices();
                        if (!sourceIndices) {
                            throw new Error(
                                `Analytic primitive ${id} has no generated indices.`,
                            );
                        }
                        const reversed = Array.from(sourceIndices);
                        for (
                            let index = 0;
                            index < reversed.length;
                            index += 3
                        ) {
                            [reversed[index], reversed[index + 2]] = [
                                reversed[index + 2],
                                reversed[index],
                            ];
                        }
                        mesh.setIndices(reversed);
                    }
                    if (axis === PrimitiveAxis.X) {
                        mesh.rotationQuaternion = Quaternion.RotationAxis(
                            new Vector3(0, 0, 1),
                            -Math.PI / 2,
                        );
                    } else if (axis === PrimitiveAxis.Z) {
                        mesh.rotationQuaternion = Quaternion.RotationAxis(
                            new Vector3(1, 0, 0),
                            Math.PI / 2,
                        );
                    } else if (axis !== PrimitiveAxis.Y) {
                        throw new Error(
                            `Unsupported analytic primitive axis ${axis}.`,
                        );
                    }
                    applyMeshOrientation(mesh, flags);
                    mesh.parent = nodes.get(nodeId) ?? root ?? null;
                    mesh.material = materialForMesh(
                        materialId,
                        Boolean(flags & MeshFlags.DoubleSided),
                    );
                    container.meshes.push(mesh);
                    meshes.set(id, mesh);
                    if (mesh.geometry) {
                        container.geometries.push(mesh.geometry);
                    }
                    break;
                }
                case Command.Instance: {
                    const sourceId = payload.u32();
                    const nodeId = payload.u32();
                    const nameOffset = payload.u32();
                    const nameLength = payload.u32();
                    const source = meshes.get(sourceId);
                    if (!source) {
                        throw new Error(
                            `Instance references missing mesh ${sourceId}.`,
                        );
                    }
                    if (thinInstanceSources.has(sourceId)) {
                        throw new Error(
                            `Mesh ${sourceId} cannot mix classic and thin instances.`,
                        );
                    }
                    classicInstanceSources.add(sourceId);
                    const instance = source.createInstance(
                        stringAt(dataBuffer, nameOffset, nameLength),
                    );
                    instance.parent = nodes.get(nodeId) ?? root ?? null;
                    container.meshes.push(instance);
                    break;
                }
                case Command.ThinInstances: {
                    const sourceId = payload.u32();
                    const transformsOffset = payload.u32();
                    const instanceCount = payload.u32();
                    const source = meshes.get(sourceId);
                    if (!source) {
                        throw new Error(
                            `Thin instances reference missing mesh ${sourceId}.`,
                        );
                    }
                    if (
                        thinInstanceSources.has(sourceId) ||
                        classicInstanceSources.has(sourceId)
                    ) {
                        throw new Error(
                            `Mesh ${sourceId} has duplicate or mixed thin instances.`,
                        );
                    }
                    assertRange(
                        dataBuffer,
                        transformsOffset,
                        instanceCount * 16,
                        4,
                        "thin instance transforms",
                    );
                    source.thinInstanceSetBuffer(
                        "matrix",
                        new Float32Array(
                            dataBuffer,
                            transformsOffset,
                            instanceCount * 16,
                        ),
                        16,
                        true,
                    );
                    source.thinInstanceEnablePicking = true;
                    thinInstanceSources.add(sourceId);
                    break;
                }
                case Command.Animation: {
                    const targetKind = payload.u32();
                    const targetId = payload.u32();
                    const property = payload.u32();
                    const trackIndex = payload.u32();
                    const keyCount = payload.u32();
                    const timesOffset = payload.u32();
                    const valuesOffset = payload.u32();
                    const stride = payload.u32();
                    if (
                        targetKind !== AnimationTarget.Node &&
                        targetKind !== AnimationTarget.Bone &&
                        targetKind !== AnimationTarget.MorphTarget
                    ) {
                        throw new Error(
                          `Invalid animation target kind ${targetKind}.`,
                        );
                    }
                    if (
                        (targetKind === AnimationTarget.MorphTarget) !==
                        (property === AnimationProperty.Influence)
                    ) {
                        throw new Error(
                          `Invalid animation property ${property} for target kind ${targetKind}.`,
                        );
                    }
                    const expectedStride =
                        property === AnimationProperty.Influence
                          ? 1
                          : property === AnimationProperty.Position ||
                        property === AnimationProperty.Scaling
                            ? 3
                            : property === AnimationProperty.RotationQuaternion
                              ? 4
                              : property === AnimationProperty.Matrix
                                ? 16
                                : 0;
                    if (stride !== expectedStride) {
                        throw new Error(
                            `Invalid animation value stride ${stride} for property ${property}.`,
                        );
                    }
                    const target =
                        targetKind === AnimationTarget.Node
                            ? nodes.get(targetId)
                            : targetKind === AnimationTarget.Bone
                              ? bones.get(targetId)
                              : morphTargets.get(targetId);
                    if (!target) {
                        break;
                    }
                    const propertyName =
                        property === AnimationProperty.Position
                            ? "position"
                            : property === AnimationProperty.RotationQuaternion
                              ? "rotationQuaternion"
                              : property === AnimationProperty.Scaling
                                ? "scaling"
                                : property === AnimationProperty.Influence
                                  ? "influence"
                                  : targetKind === AnimationTarget.Node
                                  ? NODE_MATRIX_PROPERTY
                                  : "_matrix";
                    const dataType =
                        property === AnimationProperty.Influence
                            ? Animation.ANIMATIONTYPE_FLOAT
                            : property === AnimationProperty.RotationQuaternion
                            ? Animation.ANIMATIONTYPE_QUATERNION
                            : property === AnimationProperty.Matrix
                              ? Animation.ANIMATIONTYPE_MATRIX
                              : Animation.ANIMATIONTYPE_VECTOR3;
                    const animation = new Animation(
                        `USD ${propertyName}`,
                        propertyName,
                        timeCodesPerSecond,
                        dataType,
                        Animation.ANIMATIONLOOPMODE_CYCLE,
                    );
                    assertRange(
                        dataBuffer,
                        timesOffset,
                        keyCount,
                        4,
                        "animation times",
                    );
                    assertRange(
                        dataBuffer,
                        valuesOffset,
                        keyCount * stride,
                        4,
                        "animation values",
                    );
                    const times = new Float32Array(
                        dataBuffer,
                        timesOffset,
                        keyCount,
                    );
                    const values = new Float32Array(
                        dataBuffer,
                        valuesOffset,
                        keyCount * stride,
                    );
                    animation.setKeys(
                        Array.from({ length: keyCount }, (_, index) => ({
                            frame: times[index],
                            value:
                                dataType === Animation.ANIMATIONTYPE_QUATERNION
                                    ? Quaternion.FromArray(
                                          values,
                                          index * stride,
                                      )
                                    : dataType ===
                                        Animation.ANIMATIONTYPE_MATRIX
                                      ? Matrix.FromArray(values, index * stride)
                                      : dataType === Animation.ANIMATIONTYPE_FLOAT
                                        ? values[index * stride]
                                        : Vector3.FromArray(
                                            values,
                                            index * stride,
                                          ),
                        })),
                    );
                    let group = animationGroups.get(trackIndex);
                    if (!group) {
                        group = new AnimationGroup(
                            `USD Animation ${trackIndex + 1}`,
                            scene,
                        );
                        animationGroups.set(trackIndex, group);
                        container.animationGroups.push(group);
                    }
                    group.addTargetedAnimation(animation, target);
                    break;
                }
            }
            for (const manager of morphTargetManagers.values()) {
                manager.areUpdatesFrozen = false;
                if (
                    manager.isUsingTextureForTargets ||
                    manager.numTargets <=
                        MorphTargetManager.MaxActiveMorphTargetsInVertexAttributeMode
                ) {
                    manager.optimizeInfluencers = false;
                    manager.numMaxInfluencers = manager.numTargets;
                }
            }
        }

        await Promise.all(textureLoads);
        for (const { node, parentId } of pendingParents) {
            node.parent = nodes.get(parentId) ?? root ?? null;
        }
        if (!addToScene) {
            container.removeAllFromScene();
        }
        return { container, materializeMs: performance.now() - started };
    } catch (error) {
        container.dispose();
        throw error;
    }
}
