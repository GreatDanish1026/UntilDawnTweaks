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
