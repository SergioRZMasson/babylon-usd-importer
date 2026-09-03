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
module.FS.writeFile("/test/cube.usda", await readFile(resolve(root, "demo", "assets", "cube.usda")));

const result = module.extract("/test/cube.usda");
try {
    if (!result.ok()) {
        throw new Error(result.error());
    }

    const commands = module.HEAPU8.subarray(
        result.commandPtr(),
        result.commandPtr() + result.commandSize(),
    );
    const header = new DataView(commands.buffer, commands.byteOffset, commands.byteLength);
    if (header.getUint32(0, true) !== 0x42445355 || header.getUint16(4, true) !== 3) {
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
    result.delete();
    module.FS.unlink("/test/cube.usda");
    module.FS.rmdir("/test");
}
