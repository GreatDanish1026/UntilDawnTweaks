# UntilDawnTweaks

(Formerly UntilDawnRT.) A ReShade add-on that makes ray tracing visible, and HDR work, in Until Dawn (2024 remake,
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

**Version 1.1.0.** The overlay has four tabs: **Ray tracing**, **Detail** and **HDR** hold the
saved settings (on by default, saved in `ReShade.ini`, applied every time the game starts);
**Advanced** holds testing switches and diagnostics. Hover a setting's name for an
explanation; Ray tracing and Detail have a **Reset to defaults** button.

Many settings use the game's own **Cinematic** tier: values the developers tuned for this
game (`DefaultScalability.ini`, "@Cine") that its graphics menu doesn't offer.

**Ray tracing**. Turn the game's own **Ray Tracing** option on in its graphics menu: with it
off, the game keeps every RT effect off (the tab says so).

- **Shadows**: Game (few lights: most opt out of RT shadows) / Enhanced lights /
  **All lights** (default). The most visible change, and the most costly.
- **Reflections**: Game (only inside areas the designers marked, mirror-like surfaces only) /
  **Everywhere** (default). Follows the game's RT reflections menu option.
- **Reflection roughness**: 0.4 by default (the game uses 0.01). Higher reaches rougher
  surfaces, costs more and can look noisier.
- **Quality**: Game (1 ray per pixel) / **High** (default, 2) / Ultra (4), for RT shadows,
  reflections and AO, with the shadow and reflection denoisers taking 16 reconstruction
  samples instead of 8. Less noise; the cost of those effects goes up about as much.

RT ambient occlusion is the game's own and works once its Ray Tracing option is on. In one
test at 4K output, shadows on all lights, reflections everywhere and AO took the game from
146 to 90 fps (before RT quality).

**Detail**:

- **Video card memory**: **Auto** (default, read from the graphics card) or 6 GB or less / 8 /
  10 / 12 / 16 / 20 GB or more. Sets the texture streaming pool to about a quarter of it
  (1024 / 2048 / 2560 / 3072 / 4096 / 6144 MB). The game uses 950 MB, which at 4K makes
  textures stream at lower resolution.
- **Texture filtering**: Game (8x) / **16x** (default): sharper floors and walls at shallow
  angles, nearly free.
- **Shadows**: Game / **Sharp** (default) / Cinematic. Sharp: virtual shadow maps one
  resolution step higher for the sun, moon, lamps and flashlights, with the Cinematic tier's
  8192 shadow pages so they don't run out (blur or flicker in patches). Cinematic: the game's
  Cinematic shadows (sun -1.5, 16 soft-shadow rays with 8 samples). Costs the most.
- **Fog**: Game (8 px cells, 128 slices) / **High** (default, 6 px) / Ultra (4 px, 192
  slices, about 4x the game's fog work). Smoother volumetric fog and flashlight beams.
- **View distance**: Game / **Cinematic** (default): view distance x2, mesh LOD distance 0.7,
  foliage drawn down to 0.0001 of the screen. Less pop-in.
- **Cinematic effects**: **on** (default): depth of field quality 4, screen space reflections
  4, translucency lighting volume 128.

Not included, because the game already covers them: skin (subsurface scattering) is at its
maximum on the game's top shading preset; sharpening (Image Sharpening, 0-100; 35 is strong
at 4K, try 10-20), film grain, motion blur and chromatic aberration are in its own menu.

**HDR**:

- **Peak brightness** (nits): set it to your display's peak. The game builds its tone curve
  for 1000 nits otherwise.
- **Paper white** (nits): overall scene brightness; sets 18 % grey to 0.18 × this.
- **UI brightness** (nits): menus and subtitles.

These start at the game's own values, so nothing changes until you move one.

**Advanced**: session-only switches for testing, not saved; rows the saved settings control
are locked.

- **RT master switch** (`r.RayTracing.ForceAllRayTracingEffects`): what the menu's Ray Tracing
  option sets (-1 on, 0 every RT effect off).
- **RT ambient occlusion** (`r.RayTracing.AmbientOcclusion`).
- **Enhanced RT mode** (`r.RayTracing.Enable.BMEnhancedMode`): 0 off, 1 lights marked
  enhanced-only, 2 every light (the game warns this is very slow).
- **Allow RT reflections** (`r.BM.AllowRTReflections`): 0 off, 1 where supported.
- **RT reflections** (`r.RayTracing.Reflections`): the game holds it at 0.
- **Reflections max roughness** (`r.RayTracing.Reflections.MaxRoughness`): the game uses 0.01.
- **Shadow / reflection / AO rays per pixel** (`r.RayTracing.Shadows.SamplesPerPixel`,
  `r.RayTracing.Reflections.SamplesPerPixel`, `r.RayTracing.AmbientOcclusion.SamplesPerPixel`;
  -1 as shipped) and **shadow / reflection denoiser samples**
  (`r.Shadow.Denoiser.ReconstructionSamples`, `r.Reflections.Denoiser.ReconstructionSamples`;
  8 as shipped), to find which effect is noisy.
- **Virtual shadow maps** (`r.Shadow.Virtual.Enable`): to test whether they hide RT shadows.
- **Uniform Colour Pipeline HDR** (`r.UCP.HDR`): the game's own HDR output (on by default;
  off fixes the black screen).
- **Output device** (`r.HDR.Display.OutputDevice`): 3/4 ACES PQ, 5 scRGB, 11 UCP HDR (the game's
  choice), 12 UCP scRGB, 0/10 SDR.
- **Colour gamut** (`r.HDR.Display.ColorGamut`), **UI composite mode** (`r.HDR.UI.CompositeMode`).
- **Calibration screen** (`r.HDRCalibrateScreen.Mode`, `.ShowGame`).
- **UCP exposure** (`r.UCP.ExposureAdjust`) and **max luminance** (`r.UCP.HDR.BMMaxLuminanceOverride`).
- **Texture streaming pool** (`r.Streaming.PoolSize`), **anisotropic filtering**
  (`r.MaxAnisotropy`).
- **Virtual shadow maps**: resolution bias for the sun (also while moving) and local lights
  (`r.Shadow.Virtual.ResolutionLodBias*`), pages (`r.Shadow.Virtual.MaxPhysicalPages`), soft
  shadow rays and samples (`r.Shadow.Virtual.SMRT.*`).
- **Fog grid** (`r.VolumetricFog.GridPixelSize`, `GridSizeZ`), **view distance**
  (`r.ViewDistanceScale`, `r.StaticMeshLODDistanceScale`, `foliage.MinimumScreenSize`),
  **depth of field**, **SSR** and **translucency lighting** (`r.DepthOfFieldQuality`,
  `r.SSR.Quality`, `r.TranslucencyLightingVolumeDim`).

It also shows the swap chain's format and colour space, the HDR tone curve's peak, the
renderer's values of the RT and HDR cvars, and the GPU work of the last second: draws, compute
dispatches, acceleration structure builds and ray dispatches by pipeline.

## Install
Needs ReShade 6.8+ *with full add-on support* (works next to RenoDX).
Copy `bin\UntilDawnTweaks.addon64` next to `Bates-Win64-Shipping.exe`
(`Windows\Bates\Binaries\Win64`), or run `.\build.ps1 -Install`. Delete an older
`UntilDawnRT.addon64` there if you have one (`build.ps1 -Install` does); its saved settings
carry over.

## Use
Open ReShade (Home), go to the **UntilDawnTweaks** tab, and use its Ray tracing, Detail and
HDR tabs. Under **Advanced**, pick a value for a switch, or "Game" to hand it back.
"Ray tracing work" lists the last second's DispatchRays calls by
pipeline, so you can see which RT effects actually run (the game links its RT pipelines
with `AddToStateObject`, which ReShade doesn't report, so most show as unnamed). "Ray
tracing values" shows the renderer's current values of the other RT cvars, and **Snapshot
cvars** logs what changed since the last snapshot (e.g. after changing a menu option). The
log, `UntilDawnTweaks.log`, is next to the exe.

## Build
Visual Studio 2022+ with the C++ x64 tools: `.\build.ps1`.

## Tools
| Tool | What |
|---|---|
| `tools/pak.py` | List and extract files from the game's `.pak` (needs an Oodle DLL) |
| `tools/syms.py` | Look up symbols and vtables in the game's PDB (via `llvm-pdbutil`) |
| `tools/iostore.py` | List and extract files from the game's IoStore container (`.utoc`/`.ucas`), and print each texture's size and pixel format (needs an Oodle DLL) |
