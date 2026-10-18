# UntilDawnRT

A ReShade add-on to make ray tracing visible, and HDR work, in Until Dawn (2024 remake,
Unreal Engine 5, DirectX 12). The game's ray tracing is compiled in and switched on, but its
own settings keep it from showing: most lights opt out of RT shadows, and reflections are
limited to mirror-like surfaces in curated areas. Turning HDR on shows the UI over a black
scene. See [docs/FINDINGS.md](docs/FINDINGS.md).

## HDR fix

The black screen comes from the game's own HDR output path (its "Uniform Colour Pipeline").
Unreal's standard HDR output works. To use it, add this to the game's `Engine.ini`, in
`Documents\My Games\Bates\Saved\Config\Windows` (if Windows keeps your Documents in OneDrive,
that is `OneDrive\Documents\My Games\...`; it is the folder that already has an `Engine.ini`):

```ini
[SystemSettings]
r.UCP.HDR=0
```

It has no effect in SDR. With it, the game's HDR brightness sliders do nothing; the add-on's
**HDR brightness** sliders replace them.

## The add-on

**Status: 0.0.5.**

**HDR brightness** (on by default, saved in `ReShade.ini`, applied at every start):

- **Peak brightness** (nits): set it to your display's peak. The game builds its tone curve
  for 1000 nits otherwise.
- **Paper white** (nits): overall scene brightness; sets 18 % grey to 0.18 × this.
- **UI brightness** (nits): menus and subtitles.

These start at the game's own values, so nothing changes until you move one.

Session-only switches for testing (nothing forced at start):

- **Enhanced RT mode** (`r.RayTracing.Enable.BMEnhancedMode`): 0 off, 1 lights marked
  enhanced-only, 2 every light (the game warns this is very slow).
- **Allow RT reflections** (`r.BM.AllowRTReflections`): 0 off, 1 where supported.
- **RT reflections** (`r.RayTracing.Reflections`): the game holds it at 0.
- **Reflections max roughness** (`r.RayTracing.Reflections.MaxRoughness`): the game uses 0.01.
- **Virtual shadow maps** (`r.Shadow.Virtual.Enable`): to test whether they hide RT shadows.
- **Uniform Colour Pipeline HDR** (`r.UCP.HDR`): the game's own HDR output (on by default;
  off fixes the black screen).
- **Output device** (`r.HDR.Display.OutputDevice`): 3/4 ACES PQ, 5 scRGB, 11 UCP HDR (the game's
  choice), 12 UCP scRGB, 0/10 SDR.
- **Colour gamut** (`r.HDR.Display.ColorGamut`), **UI composite mode** (`r.HDR.UI.CompositeMode`).
- **Calibration screen** (`r.HDRCalibrateScreen.Mode`, `.ShowGame`).
- **UCP exposure** (`r.UCP.ExposureAdjust`) and **max luminance** (`r.UCP.HDR.BMMaxLuminanceOverride`).

It also shows the swap chain's format and colour space, the renderer's values of the RT and
HDR cvars, and the ray tracing work the GPU does each frame.

## Install
Needs ReShade 6.8+ *with full add-on support* (works next to RenoDX).
Copy `bin\UntilDawnRT.addon64` next to `Bates-Win64-Shipping.exe`
(`Windows\Bates\Binaries\Win64`), or run `.\build.ps1 -Install`.

## Use
Open ReShade (Home), go to the **UntilDawnRT** tab. Pick a value for a switch, or "Game"
to hand it back. "Ray tracing work" lists the last frame's DispatchRays calls by
pipeline (its shaders' names), so you can see which RT effects actually run. "Related
values" shows the renderer's current values of the other RT cvars, and **Snapshot cvars**
logs what changed since the last snapshot (e.g. after changing a menu option). The log,
`UntilDawnRT.log`, is next to the exe.

## Build
Visual Studio 2022+ with the C++ x64 tools: `.\build.ps1`.

## Tools
| Tool | What |
|---|---|
| `tools/pak.py` | List and extract files from the game's `.pak` (needs an Oodle DLL) |
| `tools/syms.py` | Look up symbols and vtables in the game's PDB (via `llvm-pdbutil`) |
