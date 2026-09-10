# Tests

`smoke.mjs` loads the generated Emscripten module in Node and validates protocol payload
sizes, geometry counts, analytic primitives, and the protocol-v5 material texture layout:

```sh
node test/smoke.mjs
```

`assets/` contains small public regression stages for:

- full affine/shear transform preservation;
- matrix animation;
- instance-proxy geometry reuse;
- analytic-gprim instance reuse;
- explicit left-handed mesh orientation.
- shared packed metallic/roughness shader-node reuse;
- separate metallic, roughness, and occlusion texture bindings;
- texture source color space and float4 scale/bias serialization;
- distinct sampling metadata for shader nodes that reference the same image asset.

Private performance and skeleton assets are intentionally not committed.
