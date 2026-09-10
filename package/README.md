# babylon-usd-importer

Asynchronously load composed USD stages directly into Babylon.js.

```ts
import { loadUsdIntoSceneAsync } from "babylon-usd-importer";

const result = await loadUsdIntoSceneAsync(scene, await file.arrayBuffer(), {
    fileName: file.name,
    onProgress: ({ phase, message }) => console.log(phase, message),
});

console.log(result.container);
console.log(result.timings);
console.log(result.statistics);
```

The package sends the source asset to a Web Worker. Pixar OpenUSD performs stage
composition, traversal, geometry preparation, material extraction, skinning, and animation
sampling in C++. One command buffer and one raw-data buffer are transferred back to the
main thread, where they are materialized into a Babylon.js `AssetContainer`.

No intermediate GLB or `.babylon` file is created.

## Multi-file stages

```ts
await loadUsdIntoSceneAsync(scene, rootLayer, {
    fileName: "scene/root.usda",
    files: {
        "scene/parts/table.usdc": tableLayer,
        "scene/textures/wood.png": woodTexture,
    },
});
```

Keys in `files` preserve the directory structure relative to the root layer.
`resolveByFileName` defaults to `true`, allowing an absolute reference authored on another
machine to fall back to one unambiguous supplied file with the same name.

## Options

- `fileName`: virtual path of the root layer. File names are preserved when available;
  unnamed ZIP/USDZ bytes use `scene.usdz`, otherwise the generic fallback is `scene.usd`.
- `files`: supporting layers, payloads, and textures.
- `addToScene`: add the returned container immediately; defaults to `true`.
- `resolveByFileName`: enable conservative absolute-path recovery.
- `workerUrl`: override the package worker URL.
- `glueUrl`: override the generated Emscripten JavaScript module URL.
- `wasmUrl`: override the Wasm binary URL.
- `dataUrl`: override the OpenUSD preloaded resource bundle URL.
- `onProgress`: receive `initializing`, `staging`, `extracting`, and `materializing` updates.
- `onLog`: receive OpenUSD status, warning, and error messages.

## Returned data

`LoadResult` contains:

- `container`: the populated Babylon.js `AssetContainer`;
- `timings`: OpenUSD open, traversal, vertex preparation, packing, heap copy, and Babylon
  materialization timing;
- `statistics`: source meshes, instances, unique vertices/triangles, and buffer sizes;
- `missingAssets`: unresolved referenced file names.

`OpenUsdBabylonLoader` owns a persistent worker and can service multiple sequential loads.
Call `dispose()` when the loader is no longer needed.

## Runtime model

The versioned little-endian protocol validates command lengths, typed-array alignment, and
raw-data ranges before creating Babylon objects. Protocol v5 preserves seven independent
UsdPreviewSurface texture bindings, output channels, `UsdUVTexture` source color space and
float4 scale/bias transforms. Babylon's native PBR texture slots and TextureProcessor APIs
materialize packed or separate metallic, roughness, occlusion, opacity, normal, base-color,
and emissive maps. The protocol also supports optimized indexed geometry, subsets,
instances, skeletons, skinning, animation, and native cube/sphere/cylinder/cone commands
backed by Babylon procedural mesh builders.

The package has a Babylon.js peer dependency and contains the generated OpenUSD Wasm module
under `dist/wasm`.

Stable package subpaths are exported for custom hosting:

- `babylon-usd-importer/worker`
- `babylon-usd-importer/wasm/glue`
- `babylon-usd-importer/wasm`
- `babylon-usd-importer/wasm/data`
