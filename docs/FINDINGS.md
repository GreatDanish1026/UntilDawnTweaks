# Findings

Until Dawn (2024 remake, Ballistic Moon), Steam build with PE timestamp `0xcc7d6d54`.
The game ships `Bates-Win64-Shipping.pdb`, so engine and game symbols are available.
The exe has a SteamStub `.bind` section, but `.text` on disk is **not** encrypted, so it
can be disassembled directly (unlike the Dark Pictures games).

## Engine and project setup

- Unreal Engine 5. It has `r.RayTracing.Enable` and `r.ReflectionMethod 3` ("RT Reflections... Deprecated"),
  so the legacy (pre-Lumen) ray tracing effects are compiled in.
- `Bates/Config/DefaultEngine.ini` (in `Bates-Windows.pak`, read with `tools/pak.py`):
  - `r.RayTracing=True`, `r.DynamicGlobalIlluminationMethod=0` (no Lumen GI),
    `r.ReflectionMethod=3` (legacy RT reflections).
  - `r.RayTracing.Shadows=1`, `r.Raytracing.AmbientOcclusion=1`, `r.RayTracing.Shadows.Lights.Rect=0`,
    `r.Raytracing.Culling.Radius=5000`.
  - `r.RayTracing.Reflections.MaxRoughness=0.01`: only near-perfect mirrors get ray traced
    reflections (UE's default is 0.6). This alone could explain "RT reflections do nothing".
  - `r.Nanite=False`.
  - `[/Script/Engine.BMCoreEngineSettings] DefaultLightingRaytracedShadows=Disabled`: new
    lights default to `CastRaytracedShadow = Disabled`.
- `DefaultDeviceProfiles.ini` turns `r.RayTracing.Enable` off only for the Steam Deck profile.

## Ballistic Moon's ray tracing layer

Game-specific cvars (help text from the exe):

| cvar | values |
|---|---|
| `r.RayTracing.Enable.BMEnhancedMode` | 0 off (default); 1 effects set to "enhanced only" activate; 2 forced on everywhere ("even normal shadows... VERY SLOW") |
| `r.BM.AllowRTReflections` | UI setting: 0 always off; 1 on in areas where supported |
| `r.BM.AllowVirtualShadowMapsMode` | -1 Unreal default; 0 no VSMs; 1 curated (only when a post-process volume allows); 2 always |
| `r.BM.RayTracing.AsyncShadows` | RT shadows on async compute |

`ECastRayTracedShadow` has a game-added value, `EnabledEnhancedOnly` (3), next to
Disabled (0), UseProjectSetting (1) and Enabled (2).

`FBMSceneRenderingUtils::IsRayTracingLightAllowed(ECastRayTracedShadow)` (exe+0x1845870)
reads the renderer's copy of `BMEnhancedMode`:

```
if mode == 2: return true            # every light
if cast == Disabled: return false
if cast == EnabledEnhancedOnly: return mode != 0
return true
```

It runs every frame, from `ShouldRenderRayTracingShadowsForLight` (both overloads) and from
reflection, water-reflection and sky light setup, so changing the mode live should take
effect at once. With the project default (Disabled) on most lights, RT shadows render on
almost nothing unless the mode is 2.

Menu (`UBatesGameUserSettings`): `SetUseRaytracing`, `SetUseRaytraceReflections`,
`SetUseRayTracedAO` and `SetUseEnhancedRaytracingMode` (enum
`EBatesPCRaytracingEnhancedGraphicsMode`: Off, Enhanced, Ultra). The enhanced mode row is
shown only if `UBMUISettingBPLibrary::CanShowEnhancedRaytracingMode` (a global bool) is
true. `ABatesLightRig` has a per-rig `bAllowRaytracing`.

To confirm in game (phase 0): what each menu option writes (snapshot before/after), and
whether `BMEnhancedMode`, `AllowRTReflections` and `MaxRoughness` change the picture.

## Console variables (UE 5, from the PDB)

Same storage as UE 4.26, but different vtable details:

- `FConsoleManager::FindConsoleVariable` is at +0xb8 (4.26: +0xb0).
- `IConsoleObject` slots: IsVariableBool +0x30, IsVariableInt +0x38, IsVariableFloat +0x40,
  IsVariableString +0x48, AsVariableBool +0x50, AsVariableInt +0x58, AsVariableFloat +0x60.
  The type comes from the IsVariable* slots. DarkPicturesRT's tricks (inspect GetFloat's
  first bytes; GetBool == GetInt means a bool Ref) no longer work: the getters now start
  with a thread check.
- `IConsoleVariable`: Set +0x80, GetBool +0x88, GetInt +0x90, GetFloat +0x98.
- Data cvars (`TConsoleVariableData<T>` at +0x48): main value first, renderer's copy right
  after (+0x4c for int/float; bool data cvars exist in UE 5, 1 byte each at +0x48/+0x49).
- Ref cvars: main value at +0x50, pointer to the global the renderer reads at +0x48.
- `Set` stores the main value, then `OnCVarChange` copies it to the renderer's — only
  if `IsInGameThread()` (exe+0xc216f0). ReShade's present runs on the RHI thread, so the
  add-on still mirrors the value itself.
- `IConsoleManager::Singleton` is at exe+0x871caa8. The `r.ScreenPercentage` scan finds it
  at 7 of 8 matches (one unrelated hit), so the scan takes a majority vote.

The probe compares the renderer value it reads against the engine's own `GetFloat` for
every census cvar (off the game thread GetFloat returns the renderer's copy) and logs the
match count. In game (0.0.1): 267 match, 0 mismatch, so the layout is right.

SetBy priorities were renumbered too. `ProcessUserConsoleInput` calls Set with
`0x0b000000` (console); 4.26's console value, `0x09000000`, is below what the game uses
from code here (`0x0a000000`), so 0.0.1's writes to code-set cvars were refused.

## Phase 0, first test (0.0.1)

- `BMEnhancedMode` 1 and 2 reached the renderer (priority before: `0x02`, GameSetting, so
  the menu writes it), but the picture didn't change.
- `r.BM.AllowRTReflections` stayed 0: the game holds it at `0x0a` (code), above 0.0.1's
  `0x09`. Same for `r.RayTracing.Reflections = 0`, `r.Shadow.Virtual.Enable = 1` and
  `r.BM.AllowVirtualShadowMapsMode = 2`.
- `MaxRoughness` alone can't show anything while `r.RayTracing.Reflections` is 0.
- Other values: `r.RayTracing.Enable 1`, `r.RayTracing 1`, `r.RayTracing.Shadows 1`
  (all light types 1 except Rect), `r.RayTracing.AmbientOcclusion 1`, `r.ReflectionMethod 3`,
  `r.RayTracing.ForceAllRayTracingEffects` default.

`ShouldRenderRayTracingShadowsForLight` checks, in order: `IsRayTracingLightAllowed`
(the BM gate), `ShouldRenderRayTracingEffect(true, 3)` (RT enabled, pipeline support,
`ForceAllRayTracingEffects`), the per-light-type cvar, then `r.RayTracing.Shadows`. All
pass with mode 2, so either the shadows render and look like the virtual shadow maps they
replace, or VSMs take precedence. 0.0.2 counts DispatchRays per pipeline to tell, and can
force VSMs off.

## Online reports

At launch (October 2024), reviewers saw no visible difference with RT on, and patch 1.05
listed "Raytracing tweaks". Players later reported RT as subtle or noisy in dark scenes.
The settings above are consistent with that: reflections limited to mirrors in curated
areas, and lights opted out of RT shadows.

## HDR (black screen)

Turning HDR on in the menu calls `UBatesGameUserSettings::SetHDREnabled` (exe+0x4594e20) →
`UGameUserSettings::EnableHDRDisplayOutput(on, HDRDisplayOutputNits)` → `r.HDR.EnableHDROutput`,
then `HDRConfigureCVars` (exe+0x22119e0) picks the output device:

```
device, gamut = 0, 0
if on and GPU is AMD/NVIDIA: device = (nits == 1000) ? 3 : 4; gamut = 2   # ACES PQ, Rec2020
if device == 0 and r.UCP.SDR: device = 10        # Uniform Colour Pipeline SDR
if device in (3, 4) and r.UCP.HDR: device = 11   # UCP HDR
if device in (5, 6) and r.UCP.HDR: device = 12   # UCP scRGB
r.HDR.Display.OutputDevice = device (priority 0x05); ColorGamut = gamut
then builds the ACES tone curve (init_TsParams) from MinLuminanceLog10, nits, MidLuminance
```

`r.UCP.HDR` defaults to 1 (set by nothing else; priority 0x00), so HDR always runs through the
game's own Uniform Colour Pipeline (device 11), which is not stock UE. Intel GPUs get device 0
(SDR encoding in an HDR swap chain).

The menu's HDR sliders (`UBatesGameUserSettings` +0x4e8..+0x4f8, percentages) are mapped by
`UpdateHDRValues` (exe+0x459a520) with `MapRangeClamped(x, 0, 100, lo, hi)`:

| field | cvar | range | default |
|---|---|---|---|
| +0x4e8 exposure | `r.UCP.ExposureAdjust` | 0 – 10 | 50 % (5) |
| +0x4ec min lum | `r.UCP.HDR.BMMinLuminanceOverride`, `r.HDRCalibrateScreen.SetLum.Min` | 0 – 0.1, 0 – 1 | 0 |
| +0x4f0 max lum | `r.UCP.HDR.BMMaxLuminanceOverride` (PQ), `r.HDRCalibrateScreen.SetLum.Max` | 0.5 – 1, 0 – 1 | 50 % (PQ 0.75 ≈ 1000 nits) |
| +0x4f4 shadow cutoff | `r.UCP.DisplayGrade.ShadowCutoff` | 0 – 1 | 10 % |
| +0x4f8 shadow gamma | `r.UCP.DisplayGrade.ShadowGamma` | 0 – 4 | 33 % (1.32) |

Defaults come from `SetToDefaults`. Other UCP cvars: `r.UCP.SDROutputTransfer`,
`r.UCP.HDRCalibrationMinLumAdjust`, `r.UCP.DisplayGrade`, `r.HDR.UI.CompositeMode`.

The FSR 3 swap-chain wrapper (`FFXD3D12SwapChain`) forwards `SetColorSpace1` and
`SetHDRMetaData` to the real swap chain correctly (vtable +0x130/+0x140).

First test: `r.UCP.HDR=0` in the user Engine.ini
(`Documents\My Games\Bates\Saved\Config\Windows\Engine.ini`, `[SystemSettings]`), so HDR uses
stock ACES 1000-nit PQ (device 3).

First test result (2026-10-18): `r.UCP.HDR=0` in the user Engine.ini made no difference (not
confirmed that the game read it). A ReShade screenshot with HDR on shows the menu UI drawn
normally (PQ-encoded, so grey in a PNG) over a fully black scene. The swap chain and the UI
pass work; the scene is lost in tonemapping or in the scene/UI composite. 0.0.4 adds live HDR
switches (UCP, output device, gamut, UI composite mode, calibration screen) to find which.

**Cause found (0.0.4, 2026-10-18):** forcing `r.UCP.HDR` to 0 live brings the scene back with
HDR on; no other switch (output device, gamut, UI composite mode, calibration screen, UCP
exposure or max luminance) does. The game's Uniform Colour Pipeline HDR path is what blacks
out the scene; UE's stock ACES PQ path works. The first Engine.ini test had no effect because
it went to `Documents\...`, while this PC's Documents folder is redirected to OneDrive: the game
reads `%USERPROFILE%\OneDrive\Documents\My Games\Bates\Saved\Config\Windows\Engine.ini`. The
fix is `r.UCP.HDR=0` under `[SystemSettings]` there.

## HDR brightness (stock ACES path)

With `r.UCP.HDR=0` the game's HDR sliders do nothing (they only drive `r.UCP.*`). The stock
tone curve is rebuilt by `HDRSettingChangedSinkCallback` (exe+0x35d3dd0, a cvar sink on the
game thread) whenever the HDR state, `r.HDR.Display.MinLuminanceLog10`, `.MidLuminance`,
`.MaxLuminance`, `r.HDR.Aces.SceneColorMultiplier`, `r.UCP.HDR`/`SDR`, or
`UGameUserSettings::GetCurrentHDRDisplayNits()` changes. It calls
`HDRConfigureCVars(hdr, nits, false)` and caches the nits at exe+0x85d3140.

- The peak is that nits argument, not a cvar: `HDRConfigureCVars` builds the curve from it and
  then sets `r.HDR.Display.MaxLuminance` to it.
- `GetCurrentHDRDisplayNits` returns `HDRDisplayOutputNits` (+0x110) only if
  `bUseHDRDisplayOutput` (+0x10c) is set, else 0, and the sink then uses 1000. The game
  leaves the flag at 0 even with HDR on, so the curve is always built for 1000 nits.
- `GEngine` is exe+0x8994070; `UEngine::GameUserSettings` is at +0x2a0.
- Paper white: `r.HDR.Display.MidLuminance` is nits at 18 % grey (default 15, i.e. about
  83-nit paper white). UI: `r.HDR.UI.Luminance` (default 300).

0.0.5 writes the nits and sets the flag while HDR output is on (restoring both when turned
off), and forces `MaxLuminance`, `MidLuminance` and `UI.Luminance` at console priority.
`IConsoleVariable::Set` from the present (RHI) thread still makes the game thread run the
sinks. Confirmed in game: the cached nits follow the slider (3700) and highlights brighten.

## RT menu options → cvars

The menu setters only store fields in `UBatesGameUserSettings` (+0x4ae ray tracing, +0x4af
RT reflections, +0x4b0 RT AO, +0x4b1 enhanced mode). `ApplyNonResolutionSettings`
(exe+0x457ea00) turns them into cvars:

| field | cvar | value, priority |
|---|---|---|
| ray tracing | `r.RayTracing.ForceAllRayTracingEffects` | -1 (per effect) if on and `GRHISupportsRayTracing`, else **0 (all RT effects off)**; 0x02 |
| RT reflections | `r.BM.AllowRTReflections` | 0/1; 0x0a |
| enhanced mode | `r.RayTracing.Enable.BMEnhancedMode` | 0/1/2; 0x02 |
| RT AO (only if RT on and supported) | `r.Raytracing.AmbientOcclusion` | 0/1; 0x02 |

`r.RayTracing.Enable` is never touched (stays 1). The support flag (exe+0x88f1ccc) is the
engine's own, set at D3D12 init and read by `ShouldRenderRayTracingEffect`. Compiled ray-gen
shaders: `OcclusionRGS` (RT shadows, 192 permutations), `AmbientOcclusionRGS`,
`RayTracingReflectionsRGS`, `RayTracingDeferredReflectionsRGS`, plus PSAcoustics' audio rays
(`AudioReflectionRGSBackwardTrace`, `AudioObstructionRayRGS`).

## Why RT shows so little (2026-10-18)

- The probe's command counters were first sampled per present and read 0 draws; over one
  second (146 presents) they read 24747 draws, 33300 compute dispatches, 1898 BLAS and 146
  TLAS builds, so the hooks work and the game keeps an RT scene.
- With the menu's Ray Tracing, RT reflections and RT AO on (enhanced mode 0), exactly one RT
  pipeline dispatches: 73 times a second (every other frame) at the render resolution
  (2228x1253 at 4K output). It is a linked pipeline (built with `AddToStateObject`, which
  ReShade doesn't report), so it has no name from creation.
- RT reflections are gated per frame in `UWorld::AddPostProcessingSettings`
  (exe+0x3637c82): `r.RayTracing.Reflections` is set at priority 0x0a to 1 only when a
  blended post-process value is above 0.7 (designer-marked volumes) **and**
  `r.BM.AllowRTReflections` is 1, else 0. Even then `MaxRoughness` 0.01 limits them to
  mirrors. `r.Shadow.Virtual.Enable` gets the same per-volume treatment right after.
- RT shadows need `BMEnhancedMode` 2 (lights default to `CastRaytracedShadow = Disabled`).

## Result (0.0.6)

Forcing at console priority (0x0b, above the game's per-frame 0x0a writes), with the menu's
Ray Tracing option on: `BMEnhancedMode` 2 (RT shadows on every light) and
`r.RayTracing.Reflections` 1 (following `r.BM.AllowRTReflections`) with `MaxRoughness` 0.4.
Confirmed in game: RT shadows and RT AO visibly work, reflections run (a new pipeline,
765 dispatches/s as a 2867200x1 = 2240x1280 sorted-ray dispatch), and the settings apply by
themselves after a restart. Cost in one test at 4K output: 146 → 90 fps with everything on.
The linked pipelines' ray generation exports are renamed, so `GetShaderIdentifier` with the
library names finds none.

## RT noise (0.0.8)

As shipped, every RT effect traces 1 ray per pixel and relies on UE's denoisers:
`r.RayTracing.Shadows.SamplesPerPixel`, `r.RayTracing.Reflections.SamplesPerPixel` and
`r.RayTracing.AmbientOcclusion.SamplesPerPixel` are all -1 (light / post-process volume
decides, normally 1); `r.Shadow.Denoiser` and `r.Reflections.Denoiser` are 2 (default denoiser)
with 8 reconstruction samples, `r.AmbientOcclusion.Denoiser` 2 with 16. With RT shadows on
every light and reflections at roughness 0.4, that shows as noise. The RT quality setting
forces 2 (High) or 4 (Ultra) rays per pixel for all three and 16 reconstruction samples for
the shadow and reflection denoisers. Confirmed in game: the noise cleans up at an acceptable
cost.

## Textures, skin and detail (1.1.0)

`tools/iostore.py` reads `Bates-Windows.utoc` (TOC version 5, compressed and indexed, not
encrypted; 212,276 files) and each texture's cooked size and pixel format. The 8.4 GB `.pak`
holds audio, configs and `AssetRegistry.bin`, not textures.

- Character textures (2,659 under `Characters/`): 2,089 are 2048x2048, about 570 are 1024 or
  smaller, one reports 4096 but ships only 2048 data. Colour, roughness and masks are BC1
  (DXT1), normals BC5. Faces and heads are 2048 too.
- `DefaultDeviceProfiles.ini` caps every texture LOD group (Character included) at
  `MaxLODSize=2048`. The cook drops mips above that, so 4K sources (where they existed) are
  not shipped: a 4K mod would have to upscale.
- Skin: the game moves SSS into its own `ShadingQuality` scalability group; on the top preset
  it is already at maximum (`r.SSS.Quality` 1, `HalfRes` 0, `SampleSet` 2, `Scale` 1,
  `Burley.Quality` 1, `Checkerboard` 2 = automatic).
- Sharpening: the menu's Image Sharpening (0-100) is `r.Tonemapper.Sharpen` × 10, written from
  `UBatesGameUserSettings` +0x460 in `ApplyNonResolutionSettings` at priority 0x0a.
- Headroom, live values: `r.Streaming.PoolSize` 950 MB (`LimitPoolSizeToVRAM` 0),
  `r.MaxAnisotropy` 8, `r.Shadow.Virtual.ResolutionLodBias{Directional,Local}` 0. The Detail
  settings raise these to 3072 MB, 16 and -1; confirmed in game to help at an acceptable cost.

## Cinematic tier and the rest of 1.1.0

`DefaultScalability.ini` has an "@Cine" level for every group, above the "@3" the menu offers
(values: `ShadowQuality@Cine` sun VSM bias -1.5 also while moving, `MaxPhysicalPages` 8192,
SMRT 16 rays x 8 samples vs 8 x 4; `ViewDistanceQuality@Cine` `r.ViewDistanceScale` 2,
`r.StaticMeshLODDistanceScale` 0.7; `FoliageQuality@Cine` `foliage.MinimumScreenSize` 0.0001;
`PostProcessQuality@Cine` `r.DepthOfFieldQuality` 4; `ReflectionQuality@Cine` `r.SSR.Quality`
4). Volumetric fog sits in `GlobalIlluminationQuality`: 8 px x 128 slices at both @3 and @Cine.

- The game also sets `r.VolumetricFog.UseGridMultiplier` / `GridMultiplier` ("fog multiplier
  for dynamically adjusting volumetric fog quality") every frame at 0x0a in
  `UWorld::AddPostProcessingSettings`, from post-process volumes. Not changed; whether it
  scales the grid finer or coarser wasn't worked out.
- `SetDepthOfFieldQualities` sets the game's own DOF kernel and gather cvars; raising
  `r.DepthOfFieldQuality` to 4 on top looked fine in game.
- Film grain, motion blur strength and chromatic aberration are `UBatesGameUserSettings`
  menu options (`SetFilmGrain`, `SetMotionBlurStrength`, `SetChromaticAbberation`), so the
  add-on leaves them to the menu.
- The streaming pool follows the card's dedicated memory (DXGI `DedicatedVideoMemory` of the
  D3D12 device's adapter, via `EnumAdapterByLuid`): 1024 / 2048 / 2560 / 3072 / 4096 / 6144 MB
  for 6 or less / 8 / 10 / 12 / 16 / 20+ GB.
- Confirmed in game (RTX 5080, 4K): Sharp and Cinematic shadows, High and Ultra fog,
  Cinematic view distance and the Cinematic effects all look right at an acceptable cost.
