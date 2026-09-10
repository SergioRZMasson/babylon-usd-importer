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
        view.getUint16(4, true) !== 4
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
    if (
        Math.abs(dataView.getFloat32(rootRestOffset + 52, true) - 5) > 1e-6 ||
        Math.abs(dataView.getFloat32(rootBindOffset + 52, true) - 2) > 1e-6 ||
        Math.abs(dataView.getFloat32(childRestOffset + 48, true) - 3) > 1e-6 ||
        Math.abs(dataView.getFloat32(childBindOffset + 48, true) - 3) > 1e-6
    ) {
        throw new Error("Skeleton rest and local bind transforms were not preserved.");
    }
} finally {
    bindPoseResult.delete();
    module.FS.unlink(bindPosePath);
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
