# From Dust SkoFix

A fix for modern systems.

<p>
  <a href="docs/preview.webp"><img src="docs/preview.webp" alt="Preview" width="49%"></a>
  <a href="docs/screenshot.webp"><img src="docs/screenshot.webp" alt="Screenshot" width="49%"></a>
</p>

## Features

**Performance**
- 60 FPS support (up to 120) with original simulation speed
- Frame rate independent game logic and effects
- Accurate frame pacing
- Deferred water and terrain simulation
- Hybrid CPU support (performance cores)

**Input**
- Raw mouse input without acceleration, smoothing or minimum step
- Screen-space cursor movement
- Optional standard mouse pointer
- Unrestricted camera rotation
- Removed object snapping and terrain slowdown for mouse
- Configurable edge panning and edge push-back
- Adjustable cursor and camera speed

**Graphics**
- Maximum model detail at all distances (terrain, vegetation, objects, villagers)
- Extended sun shadow distance
- Increased shadow resolution (2x, 4x)
- Shadows for small and distant objects
- Improved water: screen-space reflections, sun specular, detail ripples, smooth normals, full resolution refraction
- Adjustable water shimmer speed

**System**
- Borderless full screen with instant Alt+Tab
- High DPI support
- Skippable intro, videos and cutscenes
- Offline start without Ubisoft Connect
- Job queue race condition fix

**Extras**
- Developer world editor (F2)
- World powers on the numeric keypad (experimental)
- Unlockable story maps and challenges

All features can be configured in `FromDust.SkoFix.ini`.

## Installation

1. Download the latest [release](https://github.com/skoinks/FromDust.SkoFix/releases).
2. Extract the archive into the game folder, next to `From_Dust.exe`.

To uninstall, delete `dinput8.dll` and `FromDust.SkoFix.*`.

## Compatibility

| Version | Status |
|---|---|
| Ubisoft Connect | Supported |
| Steam | Untested |

Tested on Windows 10. `From_Dust.exe` SHA-1: `8b9288f81c084853a77b4993a9fd8a968a3198fc`

## Reporting issues

Open an [issue](https://github.com/skoinks/FromDust.SkoFix/issues) and attach `FromDust.SkoFix.log`.

## Building

Requires Visual Studio 2022 with the C++ tools.

| Script | Purpose |
|---|---|
| `build.bat` | Builds `build/dinput8.dll` |
| `release.bat` | Packs `dist/FromDust.SkoFix.zip` |
| `shaders/build.py` | Compiles the water shaders (Windows SDK, Python) |
| `tools/watertest.bat` | Compares the water shaders with the originals |
| `tools/hang.py` | Analyzes a hang dump |

## License

[MIT](LICENSE)

Not affiliated with Ubisoft. From Dust is a trademark of Ubisoft Entertainment.
