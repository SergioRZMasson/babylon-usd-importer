import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const binaryDirectory = resolve(root, "build", "wasm", "bin");
const moduleUrl = pathToFileURL(resolve(binaryDirectory, "babylon-usd-importer.js"));
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

const cube = await extract("cube.usda");
try {
    const result = cube.result;
    const commands = module.HEAPU8.subarray(
        result.commandPtr(),
        result.commandPtr() + result.commandSize(),
    );
    const header = new DataView(commands.buffer, commands.byteOffset, commands.byteLength);
    if (header.getUint32(0, true) !== 0x42445355 || header.getUint16(4, true) !== 4) {
        throw new Error("Unexpected Babylon USD command protocol header.");
    }
    if (result.meshCount() !== 2 || result.vertexCount() !== 28 || result.triangleCount() !== 14) {
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
    module.FS.rmdir("/test");
}
