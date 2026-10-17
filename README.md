# UntilDawnRT

A ReShade add-on to make ray tracing visible in Until Dawn (2024 remake, Unreal Engine 5,
DirectX 12). The game's ray tracing is compiled in and switched on, but its own settings
keep it from showing: most lights opt out of RT shadows, and reflections are limited to
mirror-like surfaces in curated areas. See [docs/FINDINGS.md](docs/FINDINGS.md).

**Status: 0.0.3, phase 0 probe.** It confirms the console variable layout, shows the ray
tracing work the GPU does each frame, and lets you force these cvars live:

- **Enhanced RT mode** (`r.RayTracing.Enable.BMEnhancedMode`): 0 off, 1 lights marked
  enhanced-only, 2 every light (the game warns this is very slow).
- **Allow RT reflections** (`r.BM.AllowRTReflections`): 0 off, 1 where supported.
- **RT reflections** (`r.RayTracing.Reflections`): the game holds it at 0.
- **Reflections max roughness** (`r.RayTracing.Reflections.MaxRoughness`): the game uses 0.01.
- **Virtual shadow maps** (`r.Shadow.Virtual.Enable`): to test whether they hide RT shadows.

Overrides are session-only: nothing is forced at start.

## Install
Needs ReShade 6.8+ *with full add-on support* (works next to RenoDX).
Copy `bin\UntilDawnRT.addon64` next to `Bates-Win64-Shipping.exe`
(`Windows\Bates\Binaries\Win64`), or run `.\build.ps1 -Install`.

## Use
Open ReShade (Home), go to the **UntilDawnRT** tab. Pick a value for a switch, or "Game"
to hand it back. "Ray tracing work" lists the last frame's DispatchRays calls by
pipeline (its shaders' names), so you can see which RT effects actually run. "Related values" shows the renderer's current values of the other RT
cvars, and **Snapshot cvars** logs what changed since the last snapshot (e.g. after
changing a menu option). The log, `UntilDawnRT.log`, is next to the exe.

## Build
Visual Studio 2022+ with the C++ x64 tools: `.\build.ps1`.

## Tools
| Tool | What |
|---|---|
| `tools/pak.py` | List and extract files from the game's `.pak` (needs an Oodle DLL) |
| `tools/syms.py` | Look up symbols and vtables in the game's PDB (via `llvm-pdbutil`) |
