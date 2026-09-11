import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const binaryDirectory = resolve(root, "build", "wasm", "bin");
const moduleUrl = pathToFileURL(
    resolve(binaryDirectory, "babylon-usd-importer.js"),
);
const { default: createModule } = await import(moduleUrl.href);

const module = await createModule({
    noExitRuntime: true,
    locateFile: (name) => resolve(binaryDirectory, name),
});

module.FS.mkdir("/test");

async function extract(name) {
    const sourcePath = resolve(root, "demo", "assets", name);
    const virtualPath = `/test/${name}`;
    module.FS.writeFile(virtualPath, await readFile(sourcePath));
    const result = module.extract(virtualPath);
    if (!result.ok()) {
        const error = result.error();
        result.delete();
        module.FS.unlink(virtualPath);
        throw new Error(error);
    }
    return { result, virtualPath };
}

function commandRecords(result) {
    const commands = module.HEAPU8.slice(
        result.commandPtr(),
        result.commandPtr() + result.commandSize(),
    );
    const view = new DataView(commands.buffer);
    if (
        view.getUint32(0, true) !== 0x42445355 ||
        view.getUint16(4, true) !== 5
    ) {
        throw new Error("Unexpected Babylon USD command protocol header.");
    }
    const expectedLengths = new Map([
        [1, 12],
        [2, 48],
        [3, 96],
        [4, 20],
        [5, 20],
        [6, 60],
        [7, 40],
        [8, 16],
        [9, 32],
        [10, 44],
        [11, 12],
        [12, 32],
    ]);
    const records = [];
    let offset = 16;
    for (let index = 0; index < view.getUint32(8, true); ++index) {
        const opcode = view.getUint16(offset, true);
        const length = view.getUint32(offset + 4, true);
        if (length !== expectedLengths.get(opcode)) {
            throw new Error(
                `Unexpected payload length ${length} for command ${opcode}.`,
            );
        }
        records.push({ opcode, offset: offset + 8, length });
        offset += 8 + length;
    }
    if (offset !== commands.byteLength) {
        throw new Error("Unexpected trailing command data.");
    }
    return { commands, view, records };
}

const cube = await extract("cube.usda");
try {
    const result = cube.result;
    commandRecords(result);
    if (
        result.meshCount() !== 2 ||
        result.vertexCount() !== 28 ||
        result.triangleCount() !== 14
    ) {
        throw new Error(
            `Unexpected cube result: ${result.meshCount()} meshes, ` +
                `${result.vertexCount()} vertices, ${result.triangleCount()} triangles.`,
        );
    }

    console.log(
        `OpenUSD ${module.getUsdVersion()}: ${result.meshCount()} meshes, ` +
            `${result.vertexCount()} vertices, ${result.triangleCount()} triangles, ` +
            `${result.totalMs().toFixed(1)} ms`,
    );
} finally {
    cube.result.delete();
    module.FS.unlink(cube.virtualPath);
}

const analytic = await extract("analytic.usda");
try {
    if (
        analytic.result.meshCount() !== 0 ||
        analytic.result.analyticPrimitiveCount() !== 4
    ) {
        throw new Error(
            `Unexpected analytic result: ${analytic.result.meshCount()} meshes, ` +
                `${analytic.result.analyticPrimitiveCount()} analytic primitives.`,
        );
    }
} finally {
    analytic.result.delete();
    module.FS.unlink(analytic.virtualPath);
}

const bindPosePath = "/test/bind-pose.usda";
module.FS.writeFile(
    bindPosePath,
    await readFile(resolve(root, "test", "assets", "bind-pose.usda")),
);
const bindPoseResult = module.extract(bindPosePath);
try {
    if (!bindPoseResult.ok()) {
        throw new Error(bindPoseResult.error());
    }
    const { view, records } = commandRecords(bindPoseResult);
    const skeleton = records.find((record) => record.opcode === 5);
    if (!skeleton || view.getUint32(skeleton.offset + 12, true) !== 2) {
        throw new Error("Expected the bind-pose fixture to emit a two-joint skeleton.");
    }
    const data = module.HEAPU8.slice(
        bindPoseResult.dataPtr(),
        bindPoseResult.dataPtr() + bindPoseResult.dataSize(),
    );
    const dataView = new DataView(data.buffer);
    const jointsOffset = view.getUint32(skeleton.offset + 16, true);
    const rootRestOffset = dataView.getUint32(jointsOffset + 16, true);
    const rootBindOffset = dataView.getUint32(jointsOffset + 20, true);
    const childRestOffset = dataView.getUint32(jointsOffset + 40, true);
    const childBindOffset = dataView.getUint32(jointsOffset + 44, true);
    const skeletonNode = records.find((record) => {
        if (record.opcode !== 4) {
            return false;
        }
        const nameOffset = view.getUint32(record.offset + 8, true);
        const nameLength = view.getUint32(record.offset + 12, true);
        return (
            new TextDecoder().decode(
                data.subarray(nameOffset, nameOffset + nameLength),
            ) === "Rig"
        );
    });
    const mesh = records.find((record) => record.opcode === 7);
    const geometry = records.find((record) => record.opcode === 6);
    const positionsOffset = geometry
        ? view.getUint32(geometry.offset + 16, true)
        : 0;
    if (
        Math.abs(dataView.getFloat32(rootRestOffset + 52, true) - 5) > 1e-6 ||
        Math.abs(dataView.getFloat32(rootBindOffset + 52, true) - 2) > 1e-6 ||
        Math.abs(dataView.getFloat32(childRestOffset + 48, true) - 3) > 1e-6 ||
        Math.abs(dataView.getFloat32(childBindOffset + 48, true) - 3) > 1e-6 ||
        !skeletonNode ||
        !mesh ||
        !geometry ||
        view.getUint32(mesh.offset + 4, true) !==
            view.getUint32(skeletonNode.offset, true) ||
        Math.abs(dataView.getFloat32(positionsOffset, true) - 7) > 1e-6
    ) {
        throw new Error(
            "Skeleton bind transforms or skeleton-space mesh placement were not preserved.",
        );
    }
} finally {
    bindPoseResult.delete();
    module.FS.unlink(bindPosePath);
}

const skinnedInstancesPath = "/test/skinned-instances.usda";
module.FS.writeFile(
    skinnedInstancesPath,
    await readFile(resolve(root, "test", "assets", "skinned-instances.usda")),
);
const skinnedInstancesResult = module.extract(skinnedInstancesPath);
try {
    if (!skinnedInstancesResult.ok()) {
        throw new Error(skinnedInstancesResult.error());
    }
    if (
        skinnedInstancesResult.meshCount() !== 1 ||
        skinnedInstancesResult.instanceCount() !== 1
    ) {
        throw new Error(
            "Native-instanced skinned geometry did not preserve source sharing.",
        );
    }
    const { view, records } = commandRecords(skinnedInstancesResult);
    const data = module.HEAPU8.slice(
        skinnedInstancesResult.dataPtr(),
        skinnedInstancesResult.dataPtr() + skinnedInstancesResult.dataSize(),
    );
    const rigNodeIds = records
        .filter((record) => record.opcode === 4)
        .filter((record) => {
            const nameOffset = view.getUint32(record.offset + 8, true);
            const nameLength = view.getUint32(record.offset + 12, true);
            return (
                new TextDecoder().decode(
                    data.subarray(nameOffset, nameOffset + nameLength),
                ) === "Rig"
            );
        })
        .map((record) => view.getUint32(record.offset, true));
    const mesh = records.find((record) => record.opcode === 7);
    const instance = records.find((record) => record.opcode === 8);
    if (
        rigNodeIds.length !== 2 ||
        !mesh ||
        !instance ||
        !rigNodeIds.includes(view.getUint32(mesh.offset + 4, true)) ||
        !rigNodeIds.includes(view.getUint32(instance.offset + 4, true)) ||
        view.getUint32(mesh.offset + 4, true) ===
            view.getUint32(instance.offset + 4, true)
    ) {
        throw new Error(
            "Native-instanced skinned meshes were not parented to their instance-proxy Skeleton nodes.",
        );
    }
} finally {
    skinnedInstancesResult.delete();
    module.FS.unlink(skinnedInstancesPath);
}

const pointInstancerPath = "/test/point-instancer.usda";
module.FS.writeFile(
    pointInstancerPath,
    await readFile(resolve(root, "test", "assets", "point-instancer.usda")),
);
const pointInstancerResult = module.extract(pointInstancerPath);
try {
    if (!pointInstancerResult.ok()) {
        throw new Error(pointInstancerResult.error());
    }
    const { view, records } = commandRecords(pointInstancerResult);
    const batch = records.find((record) => record.opcode === 11);
    if (
        pointInstancerResult.meshCount() !== 1 ||
        pointInstancerResult.instanceCount() !== 2 ||
        !batch ||
        view.getUint32(batch.offset, true) !== 1 ||
        view.getUint32(batch.offset + 8, true) !== 2
    ) {
        throw new Error("PointInstancer was not emitted as one thin-instance batch.");
    }
    const data = module.HEAPU8.slice(
        pointInstancerResult.dataPtr(),
        pointInstancerResult.dataPtr() + pointInstancerResult.dataSize(),
    );
    const dataView = new DataView(data.buffer);
    const transformsOffset = view.getUint32(batch.offset + 4, true);
    const transforms = new Float32Array(
        data.buffer,
        transformsOffset,
        32,
    );
    const instancerNode = records.find((record) => {
        if (record.opcode !== 4) {
            return false;
        }
        const nameOffset = view.getUint32(record.offset + 8, true);
        const nameLength = view.getUint32(record.offset + 12, true);
        return (
            new TextDecoder().decode(
                data.subarray(nameOffset, nameOffset + nameLength),
            ) === "Scatter"
        );
    });
    const instancerMatrixOffset = instancerNode
        ? view.getUint32(instancerNode.offset + 16, true)
        : 0;
    if (
        !instancerNode ||
        Math.abs(dataView.getFloat32(instancerMatrixOffset + 48, true) - 100) >
            1e-6 ||
        Math.abs(transforms[12] - 11) > 1e-6 ||
        Math.abs(transforms[13] - 2) > 1e-6 ||
        Math.abs(transforms[28] - 31) > 1e-6 ||
        Math.abs(transforms[29] - 2) > 1e-6
    ) {
        throw new Error(
            "PointInstancer prototype, descendant, mask, or placement transforms were lost: " +
                `${transforms[12]},${transforms[13]} and ${transforms[28]},${transforms[29]}.`,
        );
    }
} finally {
    pointInstancerResult.delete();
    module.FS.unlink(pointInstancerPath);
}

const pointInstancerIdsPath = "/test/point-instancer-ids.usda";
module.FS.writeFile(
    pointInstancerIdsPath,
    await readFile(resolve(root, "test", "assets", "point-instancer-ids.usda")),
);
const pointInstancerIdsResult = module.extract(pointInstancerIdsPath);
try {
    if (!pointInstancerIdsResult.ok()) {
        throw new Error(pointInstancerIdsResult.error());
    }
    const { view, records } = commandRecords(pointInstancerIdsResult);
    const batch = records.find((record) => record.opcode === 11);
    if (
        !batch ||
        pointInstancerIdsResult.instanceCount() !== 2 ||
        view.getUint32(batch.offset + 8, true) !== 2
    ) {
        throw new Error(
            "Time-sampled PointInstancer ids were not used for instance masking.",
        );
    }
} finally {
    pointInstancerIdsResult.delete();
    module.FS.unlink(pointInstancerIdsPath);
}

const morphTargetsPath = "/test/morph-targets.usda";
module.FS.writeFile(
    morphTargetsPath,
    await readFile(resolve(root, "test", "assets", "morph-targets.usda")),
);
const morphTargetsResult = module.extract(morphTargetsPath);
try {
    if (!morphTargetsResult.ok()) {
        throw new Error(morphTargetsResult.error());
    }
    const { view, records } = commandRecords(morphTargetsResult);
    const data = module.HEAPU8.slice(
        morphTargetsResult.dataPtr(),
        morphTargetsResult.dataPtr() + morphTargetsResult.dataSize(),
    );
    const dataView = new DataView(data.buffer);
    const targets = new Map();
    for (const record of records) {
        if (record.opcode !== 12) {
            continue;
        }
        const id = view.getUint32(record.offset, true);
        const nameOffset = view.getUint32(record.offset + 8, true);
        const nameLength = view.getUint32(record.offset + 12, true);
        const positionsOffset = view.getUint32(record.offset + 20, true);
        targets.set(id, {
            name: new TextDecoder().decode(
                data.subarray(nameOffset, nameOffset + nameLength),
            ),
            positions: new Float32Array(data.buffer, positionsOffset, 9),
            normalsOffset: view.getUint32(record.offset + 24, true),
        });
    }
    const smileHalf = [...targets.entries()].find(([, target]) =>
        target.name.includes("inbetweens:half"),
    );
    const smile = [...targets.entries()].find(
        ([, target]) =>
            target.name === "Smile" &&
            !target.name.includes("inbetweens:half"),
    );
    const blink = [...targets.entries()].find(
        ([, target]) => target.name === "Blink",
    );
    if (
        targets.size !== 3 ||
        !smileHalf ||
        !smile ||
        !blink ||
        Math.abs(smileHalf[1].positions[7] - 1.25) > 1e-6 ||
        Math.abs(smile[1].positions[7] - 2) > 1e-6 ||
        Math.abs(blink[1].positions[1] + 0.5) > 1e-6 ||
        smileHalf[1].normalsOffset === 0xffffffff ||
        smile[1].normalsOffset === 0xffffffff ||
        blink[1].normalsOffset === 0xffffffff
    ) {
        throw new Error(
            "Sparse primary or in-between morph target data was not preserved.",
        );
    }
    const animations = records.filter(
        (record) =>
            record.opcode === 9 &&
            view.getUint32(record.offset, true) === 2,
    );
    const influenceAt = (targetId, frame) => {
        const animation = animations.find(
            (record) => view.getUint32(record.offset + 4, true) === targetId,
        );
        if (!animation || view.getUint32(animation.offset + 28, true) !== 1) {
            return Number.NaN;
        }
        const keyCount = view.getUint32(animation.offset + 16, true);
        const timesOffset = view.getUint32(animation.offset + 20, true);
        const frameIndex = Array.from(
            new Float32Array(data.buffer, timesOffset, keyCount),
        ).indexOf(frame);
        if (frameIndex < 0) {
            return Number.NaN;
        }
        return dataView.getFloat32(
            view.getUint32(animation.offset + 24, true) + frameIndex * 4,
            true,
        );
    };
    if (
        animations.length !== 3 ||
        Math.abs(influenceAt(smileHalf[0], 12) - 1) > 1e-6 ||
        Math.abs(influenceAt(smile[0], 12)) > 1e-6 ||
        Math.abs(influenceAt(smile[0], 24) - 1) > 1e-6 ||
        Math.abs(influenceAt(blink[0], 12) - 0.2) > 1e-6
    ) {
        throw new Error(
            "Morph target order, in-between weights, or influence animation was not preserved: " +
                `animations=${animations.length}, half=${influenceAt(smileHalf[0], 1)}, ` +
                `smile12=${influenceAt(smile[0], 12)}, smile24=${influenceAt(smile[0], 24)}, ` +
                `blink=${influenceAt(blink[0], 12)}.`,
        );
    }
} finally {
    morphTargetsResult.delete();
    module.FS.unlink(morphTargetsPath);
}

const textureFixturePath = "/test/material-textures.usda";
const textureImagePath = "/test/texture.png";
module.FS.writeFile(
    textureFixturePath,
    await readFile(resolve(root, "test", "assets", "material-textures.usda")),
);
module.FS.writeFile(
    textureImagePath,
    Buffer.from(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
        "base64",
    ),
);
const textureResult = module.extract(textureFixturePath);
try {
    if (!textureResult.ok()) {
        throw new Error(textureResult.error());
    }
    const { view, records } = commandRecords(textureResult);
    const data = module.HEAPU8.slice(
        textureResult.dataPtr(),
        textureResult.dataPtr() + textureResult.dataSize(),
    );
    const dataView = new DataView(data.buffer);
    const textures = new Map();
    const materials = new Map();
    for (const record of records) {
        if (record.opcode === 2) {
            const id = view.getUint32(record.offset, true);
            const valueTransformOffset = view.getUint32(
                record.offset + 44,
                true,
            );
            textures.set(id, {
                sourceColorSpace: view.getUint32(record.offset + 40, true),
                scale: Array.from({ length: 4 }, (_, index) =>
                    dataView.getFloat32(valueTransformOffset + index * 4, true),
                ),
                bias: Array.from({ length: 4 }, (_, index) =>
                    dataView.getFloat32(
                        valueTransformOffset + 16 + index * 4,
                        true,
                    ),
                ),
            });
        } else if (record.opcode === 3) {
            const nameOffset = view.getUint32(record.offset + 4, true);
            const nameLength = view.getUint32(record.offset + 8, true);
            const name = new TextDecoder().decode(
                data.subarray(nameOffset, nameOffset + nameLength),
            );
            materials.set(name, {
                metallic: view.getFloat32(record.offset + 20, true),
                roughness: view.getFloat32(record.offset + 24, true),
                textureIds: Array.from({ length: 7 }, (_, index) =>
                    view.getUint32(record.offset + 40 + index * 4, true),
                ),
                channels: Array.from({ length: 7 }, (_, index) =>
                    view.getUint32(record.offset + 68 + index * 4, true),
                ),
            });
        }
    }
    if (textures.size !== 4) {
        throw new Error(
            `Expected four source-node texture commands, found ${textures.size}.`,
        );
    }
    const packed = materials.get("Packed");
    if (
        !packed ||
        packed.textureIds[3] !== packed.textureIds[4] ||
        packed.channels[3] !== 2 ||
        packed.channels[4] !== 1 ||
        packed.metallic !== 1 ||
        packed.roughness !== 1
    ) {
        throw new Error(
            "Packed G/B texture bindings were not preserved and neutralized.",
        );
    }
    const packedTexture = textures.get(packed.textureIds[3]);
    if (
        !packedTexture ||
        packedTexture.sourceColorSpace !== 2 ||
        packedTexture.scale.some(
            (value, index) =>
                Math.abs(value - [0.25, 0.5, 0.75, 1][index]) > 1e-6,
        ) ||
        packedTexture.bias.some((value) => value !== 0)
    ) {
        throw new Error(
            "Texture source color space or scale/bias was not serialized.",
        );
    }
    const separate = materials.get("Separate");
    const separateIds = separate?.textureIds.slice(3, 6) ?? [];
    if (
        !separate ||
        separate.metallic !== 1 ||
        separate.roughness !== 1 ||
        separate.channels.slice(3, 6).some((channel) => channel !== 0) ||
        separateIds.some((id) => id === 0xffffffff) ||
        new Set(separateIds).size !== 3
    ) {
        throw new Error(
            "Separate metallic, roughness, and occlusion textures were conflated.",
        );
    }
    const separateTextures = separateIds.map((id) => textures.get(id));
    if (
        separateTextures.some((texture) => !texture) ||
        separateTextures
            .map((texture) => texture.sourceColorSpace)
            .join(",") !== "1,1,0" ||
        separateTextures.some(
            (texture, index) =>
                Math.abs(texture.scale[0] - [0.2, 0.3, 0.4][index]) > 1e-6,
        ) ||
        Math.abs(separateTextures[1].bias[0] - 0.1) > 1e-6
    ) {
        throw new Error(
            "Same-asset shader nodes lost distinct color-space or value transforms.",
        );
    }
} finally {
    textureResult.delete();
    module.FS.unlink(textureFixturePath);
    module.FS.unlink(textureImagePath);
    module.FS.rmdir("/test");
}
