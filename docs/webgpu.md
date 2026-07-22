# WebGPU (experimental)

Nordstjernen has an **experimental** WebGPU (`navigator.gpu`)
implementation. Unlike WebGL — which is in the standard build and mapped
onto the in-tree GLES path — WebGPU is layered on the external
[wgpu-native](https://github.com/gfx-rs/wgpu-native) library. It exists for
experimentation and is kept behind a runtime gate because wgpu-native is a
large dependency that does not fit the minimalism of the rest of the engine.

The `webgpu` meson feature is `auto`: the binding is **compiled in whenever
wgpu-native is present** and silently skipped when it is not. So a stock
build on a machine without wgpu-native contains **no** WebGPU symbol, code,
or dependency — `navigator.gpu` is simply `undefined`, exactly as before —
while a machine that has wgpu-native gets a WebGPU-capable binary without
needing to pass any extra build flag. Even then the API stays **off at
runtime** until the browser is started with `--enable-webgpu`.

## Building with WebGPU

WebGPU is gated by the `webgpu` meson feature (`auto` by default). Building
it requires the wgpu-native library and headers; once those are reachable,
the default `auto` setting picks them up automatically.

1. Get a wgpu-native release for your platform from
   <https://github.com/gfx-rs/wgpu-native/releases> and extract it. A
   release contains `lib/libwgpu_native.{so,a}` and
   `include/webgpu/{webgpu.h,wgpu.h}`. The C API headers are also vendored
   in-tree under `third_party/wgpu-native/` (pinned to the supported
   release — currently **v29.0.0.0**); only the library is fetched
   externally.

2. Configure pointing at the extracted release:

   ```sh
   meson setup builddir -Dwgpu_native_root=/path/to/wgpu-native-release
   meson compile -C builddir
   ```

   If your distribution ships a `wgpu_native` pkg-config file, `auto` finds
   the library automatically and `-Dwgpu_native_root` is unnecessary. Pass
   `-Dwebgpu=enabled` to make a missing wgpu-native a hard configure error
   (useful in CI), or `-Dwebgpu=disabled` to force the binding out even when
   the library is installed.

   At runtime the shared library must be on the loader path (e.g.
   `LD_LIBRARY_PATH=/path/to/release/lib`) unless it is installed
   system-wide.

The headers are pinned in-tree so the binding always compiles against a
known API version; to move to a newer wgpu-native, replace the two headers
under `third_party/wgpu-native/include/webgpu/` with the matching release
and rebuild.

## Runtime gating

Even in a build that contains WebGPU, the API is **denied by default**.
`navigator.gpu.requestAdapter()` resolves to `null` unless WebGPU is
explicitly enabled, by either:

- starting the browser with the **`--enable-webgpu`** command-line flag, or
- setting the environment variable **`NS_WEBGPU_ALLOW=1`** (what the flag
  does internally).

The shell sets the variable before the sandboxed renderer is spawned, so the
renderer — where the page's JS and `src/webgpu.c` actually run — inherits the
permission. On a build without WebGPU, `--enable-webgpu` prints a one-line
notice and is otherwise ignored. Unlike WebGL, which is enabled by default,
this keeps the large native GPU stack dormant unless explicitly requested.

## Implemented surface

The implementation (`src/webgpu.c`) covers device acquisition, buffers, and
a working **render-to-canvas** path:

- `navigator.gpu` — `requestAdapter()`, `getPreferredCanvasFormat()`,
  `wgslLanguageFeatures`.
- `GPUAdapter` — `requestDevice()`, `info` (`vendor`/`architecture`/
  `device`/`description`), `features`, `limits`, `isFallbackAdapter`.
- `GPUDevice` — `queue`, `features`, `limits`, `createBuffer()`,
  `createCommandEncoder()`, `getQueue()`, `destroy()`.
- `GPUQueue` — `writeBuffer()`, `submit()`.
- `GPUBuffer` — `size`, `usage`, `destroy()`.
- `GPUCanvasContext` (`canvas.getContext('webgpu')`) — `configure()`
  (`device`, `format`, `alphaMode`), `unconfigure()`, `getConfiguration()`,
  `getCurrentTexture()`.
- `GPUTexture` — `createView()`, `width`/`height`/`format`, `destroy()`;
  `GPUTextureView`.
- `GPUCommandEncoder` — `beginRenderPass()` (color attachment `view`,
  `loadOp`/`storeOp`, `clearValue`), `finish()`.
- `GPURenderPassEncoder` — `end()`, `setPipeline()`, `setVertexBuffer()`,
  `setIndexBuffer()`, `draw()`, `drawIndexed()`. (`setBindGroup`,
  `setViewport`, `setScissorRect` are accepted as no-ops for now.)
- `GPUCommandBuffer`.
- `GPUShaderModule` — `device.createShaderModule({ code })` compiles WGSL
  via wgpu-native's `naga`; `getCompilationInfo()`.
- `GPURenderPipeline` — `device.createRenderPipeline()` with `layout: 'auto'`,
  a `vertex` stage (`module`, `entryPoint`, and `buffers[]` —
  `arrayStride`/`stepMode`/`attributes[{format, offset, shaderLocation}]`),
  a `fragment` stage (`module`, `entryPoint`, `targets[{format}]`), and
  `primitive.topology`.

- `GPUBindGroupLayout` / `GPUPipelineLayout` / `GPUBindGroup` — buffer
  (uniform/storage), sampler, and texture-view bindings; `setBindGroup`;
  `pipeline.getBindGroupLayout()`.
- `GPUSampler` (`device.createSampler`), `GPUTexture` (`device.createTexture`,
  `createView`, `destroy`), `queue.writeTexture`,
  `queue.copyExternalImageToTexture` (uploads an image/`<canvas>`/
  `ImageBitmap`/video frame into a texture), depth/stencil and MSAA resolve
  attachments in render passes, `depthStencil` + `multisample` + `cullMode`
  pipeline state.
- `GPUBuffer.mapAsync()` (poll-driven), `GPUQuerySet` scaffolding
  (`device.createQuerySet`, `commandEncoder.resolveQuerySet` — occlusion is
  real, timestamps are accepted as no-ops so timing-instrumented apps don't
  crash).
- **Compute**: `device.createComputePipeline`, `commandEncoder.beginComputePass`,
  `setPipeline`/`setBindGroup`/`dispatchWorkgroups`/`end`, and
  `pipeline.getBindGroupLayout` — storage buffers in, results read back via
  `copyBufferToBuffer` + `mapAsync`. three.js GPU-compute examples
  (`webgpu_compute_birds`, `webgpu_compute_particles`) run on it.
- Colour-target **blend state** (`blend.color`/`blend.alpha` factors and
  operations) and `writeMask`, so transparent materials composite correctly;
  `texture.createView` **descriptors** (`format`, `dimension` incl. cube /
  2d-array / 3d, `aspect`, base/count mip + array layers).
- `GPUBuffer.getMappedRange()` / `unmap()` (for `mappedAtCreation`),
  `commandEncoder.copyTextureToTexture` / `copyBufferToBuffer`,
  `device.pushErrorScope`/`popErrorScope`, a non-fatal device
  uncaptured-error handler, and the `GPUBufferUsage`/`GPUTextureUsage`/
  `GPUShaderStage`/`GPUColorWrite`/`GPUMapMode` global flag namespaces.

So the full draw path works: WGSL shader modules → bind groups → render
pipeline (with depth) → render pass → `setPipeline`/`setBindGroup`/
`setVertexBuffer`/`draw` → submit → composited to the canvas.

**three.js's `WebGPURenderer` runs on this backend** (no WebGL2 fallback):
the `webgpu_camera` example renders its scene — wireframe spheres, the
camera-frustum helper, the axis gizmo, and the point-cloud starfield — on
real WebGPU through wgpu-native.

A configured canvas owns an offscreen target texture
(`RENDER_ATTACHMENT | COPY_SRC`); each paint, the renderer copies it back
(`copyTextureToBuffer` → map → cairo `ARGB32`, BGRA/RGBA aware,
opaque/premultiplied alpha aware) exactly like the WebGL compositor. So a
page that clears its canvas via a render pass shows the GPU-produced result.

Promise-returning calls (`requestAdapter`, `requestDevice`, buffer mapping)
are resolved by polling wgpu-native's event loop synchronously, so
`await navigator.gpu.requestAdapter()` works without integrating with the
page event loop.

### Not yet implemented

What remains: real timestamp queries, render bundles, explicit blend state,
storage textures, and 3D/cube/array texture-view descriptors (views default
to 2D). Geometry, vertex colours, uniforms/transforms, texture-mapped
geometry, and GPU compute all work; heavier three.js material examples (PBR
clearcoat, environment maps) still drive the software backend into feature
paths that `wgpu-native` itself panics on, which need the missing pieces
above. This document and feature-detection reflect exactly what runs.

## Architecture & security notes

- The implementation talks to the standard multi-vendor `webgpu.h` C ABI, so
  it is backend-agnostic; wgpu-native selects a backend (Vulkan / Metal /
  D3D12 / GL) at runtime and falls back to software (llvmpipe / lavapipe)
  when no GPU is present.
- wgpu-native is, by a wide margin, the largest dependency the project can
  pull in (tens of MB, plus a Rust toolchain to build from source) and is
  not auditable by a single maintainer. This is the core reason it stays
  opt-in and out of the default build.
- Real (hardware) GPU backends need device access and a large driver attack
  surface that the sandboxed renderer otherwise denies; running WebGPU
  against a hardware backend inside the sandbox is an open design question
  (a GPU-broker process is the likely answer). The software backend used for
  development does not need device access.

## Quick check

```sh
LD_LIBRARY_PATH=/path/to/release/lib \
  ./builddir/src/gtk/nordstjernen --headless --dump=none --enable-webgpu \
  --eval='navigator.gpu.requestAdapter().then(a=>a.info.device)' about:blank
```
