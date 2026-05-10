# Axiom bgfx Shaders

Source files for the bgfx backend's render pipelines (Stage 2 of the bgfx
port — see `Graphics/RenderApi.hpp`). Authored in bgfx's HLSL-flavoured
shading dialect and compiled offline through bgfx's `shaderc` tool to
per-platform binary blobs the engine loads at runtime.

The runtime currently expects compiled binaries under
`AxiomAssets/Shaders/bgfx/bin/<profile>/<stage>_<name>.bin`, e.g.:
```
AxiomAssets/Shaders/bgfx/bin/dx11/vs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/dx11/fs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/glsl/vs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/glsl/fs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/spirv/vs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/spirv/fs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/metal/vs_sprite.bin
AxiomAssets/Shaders/bgfx/bin/metal/fs_sprite.bin
```

The Shader_Bgfx.cpp loader picks the right profile based on
`bgfx::getRendererType()`.

## Compiling shaders

Stage 2 of the bgfx port did **not** integrate `shaderc.exe` into premake.
The tool itself is a separate target inside `External/bgfx/tools/` that
needs to be built once per dev machine. Until that integration lands, run
the compile manually after editing any `.sc` file:

### One-time: build shaderc

From the repo root:
```sh
cd External/bgfx
make tools-windows  # or tools-linux / tools-osx
# Produces External/bgfx/.build/win64_vs2022/bin/shadercRelease.exe
```
(See <https://bkaradzic.github.io/bgfx/build.html#tools> for the full list
of platform targets.)

### Per shader: compile

For each `.sc` file, run shaderc once per backend you care about. Example
for `vs_sprite.sc` on Windows D3D11:

```sh
shadercRelease.exe \
    -f AxiomAssets/Shaders/bgfx/vs_sprite.sc \
    -o AxiomAssets/Shaders/bgfx/bin/dx11/vs_sprite.bin \
    --platform windows --type vertex --profile s_5_0 \
    --varyingdef AxiomAssets/Shaders/bgfx/varying.def.sc \
    -i External/bgfx/src
```

Per-profile flags (matching bgfx's own examples):
| Backend  | --platform | --type   | --profile  |
|----------|------------|----------|------------|
| D3D11    | windows    | v/f      | `s_5_0`    |
| OpenGL   | linux      | v/f      | `120`      |
| Vulkan   | linux      | v/f      | `spirv`    |
| Metal    | osx        | v/f      | `metal`    |

The `compile-shaders.bat` / `.sh` helpers (TODO) wrap the above for the
full shader matrix.

## Files

- `varying.def.sc` — vertex / fragment varying declarations shared by all
  sprite shaders.
- `vs_sprite.sc` / `fs_sprite.sc` — the sprite pipeline (Renderer2D's
  instanced quad submit).

Future Stage 2 sub-passes will add:
- `vs_text.sc` / `fs_text.sc` — single-channel atlas sampling for
  TextRenderer's glyph quads.
- `vs_gizmo.sc` / `fs_gizmo.sc` — line submission for GizmoRenderer.
- `vs_ui.sc` / `fs_ui.sc` — UI quad shader for GuiRenderer (may share
  the sprite shader; TBD when the UI submit lands).
