# purzOS OFX plugins

A collection of native **OpenFX** video plugins for DaVinci Resolve (and any
other OFX host) — retro/analog looks, glitch, and CRT/VHS signal effects. Each
plugin is a single self-contained `.cpp` compiled against the official OpenFX
C++ Support library. Several are ports of the [purzOS](https://github.com/purzbeats/purzOS)
web effects; the rest are OFX-native.

All effects are **deterministic** — any randomness is hashed from
`(seed, frame, position)`, never `rand()` — so a render is byte-for-byte
identical on any machine and safe to distribute across a render farm.

## Install

Download the archive for your platform from the [**Releases**](../../releases)
page — `purzos-ofx-windows-x64.zip`, `purzos-ofx-macos-arm64.zip`, or
`purzos-ofx-linux-x64.zip` — unzip it, and drop the `*.ofx.bundle` folders into
your OFX plugins folder:

| OS | Folder |
|---|---|
| Windows | `C:\Program Files\Common Files\OFX\Plugins` |
| macOS | `/Library/OFX/Plugins` |
| Linux | `/usr/OFX/Plugins` |

Relaunch Resolve; the plugins show up under **OpenFX → purzOS**. To install
without admin rights, put the bundles anywhere and add that folder to the
`OFX_PLUGIN_PATH` environment variable instead (`;`-separated on Windows,
`:`-separated elsewhere).

- macOS builds are **arm64 (Apple Silicon)** only, and unsigned — clear
  quarantine after copying: `xattr -dr com.apple.quarantine /Library/OFX/Plugins`

## Effects

### Ports of the purzOS web tools

| Plugin | What it does |
|---|---|
| ASCII | image rendered as ASCII cells (embedded public-domain 8x8 font) |
| Bayer Dither | ordered Bayer-matrix dithering |
| Pixel Sort | threshold-driven pixel sorting |

### Glitch (OFX-native)

`Hold frames` sets how many frames each glitch state lasts before re-rolling.

| Plugin | What it does |
|---|---|
| Slice Glitch | horizontal slice bands torn sideways (wrapping) with per-slice R/B split |
| Chroma Shift | directional RGB separation, jittering per horizontal band |

### Analog suite — one stage of an '80s composite/VHS signal path each

Real scanline signal processing in YIQ, driven by sines of (row, column, frame).
Stack them in signal-path order for the full effect: **Chroma Bleed → Luma Ring
→ Rainbow Phase → Tape Wow → Sync Drift**.

| Plugin | Signal stage |
|---|---|
| Chroma Bleed | tape chroma: delay-line offset + bandwidth crush + trailing colour smear (luma stays sharp) |
| Luma Ring | luma channel: band-limit softness, "detail knob" ringing halos, RF multipath ghost echo |
| Rainbow Phase | subcarrier: per-line hue drift that crawls, cross-colour rainbows on fine detail, dot crawl on chroma edges |
| Tape Wow | transport: slow wow + scanline flutter + frame bounce + bottom head-switch skew band |
| Sync Drift | the TV: h-hold diagonal shear, top-of-frame flagging, vertical roll with blanking bar |

### Looks & weird optics

Tone, glow, and self-sampling effects — deterministic, content-driven, no
randomness at all.

| Plugin | What it does |
|---|---|
| Halation | film-emulsion glow: thresholded highlights bloom back over the frame through a tint (film red, phosphor, amber…) |
| Duotone | luma through a two-ink ramp; presets (Cyanotype, Sepia, Midnight, Miami, Toxic, Heat, Gold Press, Chrome) + custom inks |
| Video Feedback | camera-at-its-own-monitor: nested zoomed/rotated copies with decay; Spin corkscrews the stack over time |
| Scan Warp | Rutt/Etra scan processor: the image's own brightness deflects the raster vertically — footage becomes terrain |
| Phosphor Melt | bright pixels burn in and drip decaying colour trails (down/up/left/right), like smeared phosphor burn |
| Vector Rescan | oscillographics: sparse scanlines ride the luma like terrain, drawn as a dotted beam with two-tier phosphor glow |

## Build from source

Prerequisites: **CMake 3.16+**, **Visual Studio (MSVC x64)**, and the OpenFX SDK
cloned into `openfx/` (it is git-ignored — this repo does not redistribute it):

```sh
git clone https://github.com/AcademySoftwareFoundation/openfx openfx
```

Then:

```powershell
.\build.ps1            # configure (first run) + build Release → build\bundles\<Name>.ofx.bundle
.\build.ps1 -Install   # also append build\bundles to the user OFX_PLUGIN_PATH
```

`-Install` appends to the `OFX_PLUGIN_PATH` user environment variable (never
overwriting), so no admin rights are needed. Relaunch Resolve afterward.

## License

Plugin source is **MIT** (see [LICENSE](LICENSE)). The OpenFX SDK it builds
against is BSD-3-Clause and **not included here** — it's cloned from upstream at
build time. Binary releases statically link the OpenFX Support library and ship
the required attribution in [THIRD-PARTY.md](THIRD-PARTY.md).
