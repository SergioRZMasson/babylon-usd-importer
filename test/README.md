# Tests

`smoke.mjs` loads the generated Emscripten module in Node, converts the public cube sample,
and validates the command protocol header and geometry counts:

```sh
node test/smoke.mjs
```

`assets/` contains small public regression stages for:

- full affine/shear transform preservation;
- matrix animation;
- instance-proxy geometry reuse;
- analytic-gprim instance reuse;
- explicit left-handed mesh orientation.

Private performance and skeleton assets are intentionally not committed.
