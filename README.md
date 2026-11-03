# UntilDawnTweaks 1.2.0

Graphics add-on for **Until Dawn** (2024 remake, PC, Steam), for ReShade with add-on support. It
makes the game's ray tracing work, raises detail beyond what its menu offers, and replaces the
HDR brightness sliders that stop working once HDR's black screen is fixed. Changes apply live,
from an in-game overlay.

- **Ray tracing fix**: the game's ray tracing is there, but its own settings keep it from
  showing. Most lights opt out of ray traced shadows, and ray traced reflections only appear in
  a few areas the designers marked, and only on mirror-like surfaces. UntilDawnTweaks gives
  every light ray traced shadows (or only the lights marked "enhanced"), turns reflections on
  everywhere, and lets them reach rougher surfaces (0.4; the game uses 0.01). The quality
  presets leave this alone.
- **Ray tracing quality**: the game traces 1 ray per pixel and leans on its denoisers, which
  shows as noise. High traces 2 rays with stronger denoising, Ultra 4.
- **Detail**: texture memory sized to your graphics card (the game's 950 MB makes textures
  stream at lower resolution at 4K), 16x anisotropic filtering, sharper shadows, finer
  volumetric fog and flashlight beams, Cinematic view distance and Cinematic effects (depth of
  field, screen space reflections, translucency lighting). Most of it is the game's own
  Cinematic tier, which its menu doesn't offer.
- **Camera**: switches for depth of field, lens flares and vignette. Film grain, motion blur,
  chromatic aberration and sharpening are in the game's own graphics menu, so they aren't here
  (the game's Image Sharpening is strong at 4K: try 10-20).
- **HDR brightness**: peak brightness, paper white and UI brightness, in nits.
- **Quality presets**: *Ultra+* (High ray tracing quality, Ultra fog, sharp shadows and the
  Detail options) and *Cinematic* (adds Ultra ray tracing quality and Cinematic shadows; about
  30% slower than Game on an RTX 5080 at 4K). They leave the ray tracing fix, Camera and HDR
  alone. **Measure preset costs** and **Measure each option** check them on your PC.
- **F8** switches every tweak off and on, to compare.

## Requirements

- Until Dawn for PC (Steam version tested). If the game's code doesn't match, the add-on changes
  nothing and says so in the overlay and in `UntilDawnTweaks.log`; HDR brightness needs the
  tested game version.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `Bates-Win64-Shipping.exe` with DirectX 12.
- For ray tracing: a GPU with hardware ray tracing, and the game's own **Ray Tracing** option on
  in its graphics menu (with it off, the game keeps every RT effect off; the Ray tracing tab says
  so). RT reflections and RT ambient occlusion also follow their own menu options.

## HDR fix

Turning HDR on shows the menus over a black game. The cause is the game's own HDR output path
(its "Uniform Color Pipeline"); Unreal's standard HDR output works. To use it, add this to the
game's `Engine.ini` in `Documents\My Games\Bates\Saved\Config\Windows` (if Windows keeps your
Documents in OneDrive, `OneDrive\Documents\My Games\...`: the folder that already has an
`Engine.ini`):

```ini
[SystemSettings]
r.UCP.HDR=0
```

It has no effect in SDR. With it, the game's own HDR sliders do nothing; the HDR tab replaces
them.

## Install

1. Install ReShade with add-on support into `...\Until Dawn\Windows\Bates\Binaries\Win64`.
2. Copy `UntilDawnTweaks.addon64` into the same folder. Delete an older `UntilDawnRT.addon64`
   there if you have one.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under UntilDawnTweaks.

Settings are saved in `ReShade.ini` under `[UntilDawnTweaks]`; settings from 1.x carry over. A
new install starts with the ray tracing fix on, the Ultra+ preset and HDR brightness at the
game's own values. Nothing else is written to disk; deleting the `.addon64` file removes the mod
(the two Engine.ini lines are the HDR fix: without them HDR is a black screen again).

## Overlay

- **Quality preset** at the top, with the frame time and the two measuring buttons.
- **Ray tracing**: shadows, reflections, reflection roughness, quality, and whether the GPU is
  tracing rays right now.
- **Detail**: texture memory, filtering, shadows, fog, view distance, effects.
- **Camera**: depth of field, lens flares, vignette (Game / Off).
- **HDR**: HDR brightness (Game / Set here) with peak, paper white and UI sliders.
- **Advanced**: status, a probe that writes every relevant setting to `UntilDawnTweaks-probe.txt`,
  and a console variable field for experiments.

`*` marks an option changed from the game's value; the **Game** choice shows the game's own
value. Hover **(?)** for details. The log, `UntilDawnTweaks.log`, is next to the game's exe.

## Building

`.\build.ps1` builds `bin\UntilDawnTweaks.addon64` (needs Visual Studio 2022 with the C++
tools). `.\build.ps1 -Install` also copies it into the game folder (close the game first),
`-Package` makes the release zip. See `docs/FINDINGS.md` for how the game's settings were found.

| Tool | What |
|---|---|
| `tools/pak.py` | List and extract files from the game's `.pak` (needs an Oodle DLL) |
| `tools/syms.py` | Look up symbols and vtables in the game's PDB (via `llvm-pdbutil`) |
| `tools/iostore.py` | List and extract files from the game's IoStore container (`.utoc`/`.ucas`), and print each texture's size and pixel format (needs an Oodle DLL) |

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
