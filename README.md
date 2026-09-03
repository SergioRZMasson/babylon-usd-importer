# Babylon USD Importer

An experimental foundation for loading USD assets directly into Babylon.js at runtime.

**[Live demo](https://sergiorzmasson.github.io/babylon-usd-importer/)**

The importer opens a composed USD stage with Pixar OpenUSD in WebAssembly, traverses and
prepares the renderable scene in C++, then returns two transferable buffers:

- a versioned command queue describing Babylon.js objects;
- an aligned raw-data buffer containing geometry, textures, matrices, skinning, and
  animation samples.

There is no intermediate GLB or `.babylon` file, no per-prim JavaScript/OpenUSD traffic,
and no stage flattening.

> This repository is intended as a technical foundation for a future official Babylon.js
> USD importer. It is not currently part of the Babylon.js release distribution.

## Usage

```ts
import { loadUsdIntoSceneAsync } from "babylon-usd-importer";

const result = await loadUsdIntoSceneAsync(scene, await file.arrayBuffer(), {
    fileName: file.name,
    onProgress: ({ message }) => console.log(message),
    onLog: (level, message) => console[level](message),
});

console.log(result.timings);
console.log(result.statistics);
```

Multi-file stages can supply referenced layers and textures through an in-memory virtual
file tree:

```ts
await loadUsdIntoSceneAsync(scene, rootLayer, {
    fileName: "robot/root.usda",
    files: {
        "robot/parts/arm.usda": armLayer,
        "robot/textures/base.png": baseColor,
    },
});
```

The default loader runs extraction in a module Web Worker and adds the resulting
`AssetContainer` to the supplied scene. Set `addToScene: false` to manage the container
yourself.

## Architecture

```text
USD / USDA / USDC / USDZ bytes
        |
        v
Web Worker + Emscripten virtual filesystem
        |
        v
UsdStage::Open
        |
        v
Direct C++ traversal of the composed stage
  - UsdGeom hierarchy, meshes, primvars, subsets
  - UsdShade bindings and UsdPreviewSurface
  - UsdSkel skeletons, skinning, animation
  - instance-proxy geometry reuse
        |
        v
Exact vertex welding + first-use vertex ordering
        |
        +--> command buffer
        +--> raw-data buffer
        |
        v
Babylon.js AssetContainer materialization
```

OpenUSD stage composition remains authoritative. References, payloads, variants, and
instance proxies are read from the composed stage directly.

### No flattening

The importer never calls `UsdStage::Flatten`, serializes a temporary USD layer, or reopens
one. Flattening was required only by file-format-export workflows. Reading the composed
stage directly avoids that extra allocation and preserves native instance/prototype
relationships.

### No submodules

The repository has no git submodules. Its native dependency is OpenUSD, consumed through
vcpkg. OpenUSD brings its required oneTBB and zlib dependencies transitively.

There are no dependencies on:

- Adobe USD-Fileformat-plugins;
- TinyGLTF;
- meshoptimizer;
- nlohmann-json;
- GLB or `.babylon` serialization libraries.

The repository-owned weld compares complete vertex streams exactly, preserving normal, UV,
color, opacity, and skinning seams while emitting vertices in first-use order.

## Supported runtime data

- USD, USDA, USDC, and USDZ root layers.
- Relative references and browser-supplied virtual files.
- Conservative file-name fallback for absolute paths authored on another machine.
- Composed transform hierarchy and reset-xform-stack behavior.
- Y-up/Z-up and Babylon left-/right-handed scene conversion.
- USD `rightHanded`/`leftHanded` mesh orientation and winding validation.
- Polygon triangulation and USD primvar interpolation.
- Material binding subsets.
- Exact vertex welding and indexed geometry.
- `UsdPreviewSurface`, `UsdUVTexture`, `UsdTransform2d`, and primvar readers.
- PBR base color, opacity, metallic/roughness packing, normal, emissive, and UV transforms.
- Shared source geometry and Babylon instances.
- Up to eight skinning influences.
- Skeletons, node animation, and skeletal animation.

## Current limitations

- Polygon triangulation is a convex fan; concave n-gons need a more robust triangulator.
- Only one UV stream is currently emitted per mesh.
- Separately authored metallic and roughness textures are not repacked yet.
- MaterialX, MDL, OpenPBR, and other surface models fall back explicitly.
- Browser-unsupported image formats require native transcoding.
- Blend shapes, point instancers, analytic gprims, cameras, lights, physics, and runtime
  variant switching are not yet represented by the command protocol.
- Babylon object construction runs on the main thread after worker extraction.

## Build

Requirements:

- CMake 3.24 or newer;
- Ninja;
- Emscripten;
- a vcpkg checkout exposed through `VCPKG_ROOT`;
- Node.js 18 or newer and npm.

Build the Wasm module:

```sh
export VCPKG_ROOT=/path/to/vcpkg
export EMSCRIPTEN_ROOT=/path/to/emscripten

cmake --workflow --preset wasm
```

Build the Wasm module and npm package:

```sh
cmake --workflow --preset wasm-package
```

Build the demo:

```sh
npm --prefix demo install
npm --prefix demo run build
npm --prefix demo run serve
```

The demo is emitted under `docs/` for GitHub Pages.

Run the Node/Emscripten protocol smoke test:

```sh
node test/smoke.mjs
```

## Repository layout

```text
src/          OpenUSD traversal, command packing, resolver, and Emscripten bindings
package/      TypeScript worker, protocol decoder, Babylon materializer, and public API
demo/         direct-import browser demo
docs/         generated GitHub Pages demo
ports/        vcpkg OpenUSD and single-threaded oneTBB overlays
triplets/     release SIMD/LTO Emscripten triplet
resources/    WebResolver plugin metadata
scripts/      convenience build wrapper
test/         Node/Emscripten protocol smoke test
```
