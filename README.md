# WayWal (`wwal`)

[![CI](https://github.com/Execorn/wwal/actions/workflows/ci.yml/badge.svg)](https://github.com/Execorn/wwal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Execorn/wwal?color=007ec6)](https://github.com/Execorn/wwal/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

<p align="center">
  <a href="https://raw.githubusercontent.com/Execorn/wwal/assets/demo.mp4">
    <img src="https://raw.githubusercontent.com/Execorn/wwal/assets/demo.webp" alt="WayWal Demo" width="100%">
  </a>
</p>

**WayWal** is a fast, all-in-one wallpaper, live shader, Steam Wallpaper Engine, and Material Design 3 color theme tool for Linux desktops (Hyprland, Sway, Niri, and other Wayland setups).

Most people install five or six separate tools (`swww`/`awww`, `mpvpaper`, `linux-wallpaperengine`, `wpaperd`, `matugen`/`wallust`/`pywal`, and custom picker scripts) just to get pictures, video loops, Steam Wallpaper Engine scenes, music bars, and matching desktop colors. WayWal does all of that in one tiny C23 program (`1.66 MB` stripped total). It is easy to use and light on your battery.

---

## Key Features

- **Every Wallpaper Type (`wwal img`)**: Universal `wwal img` auto-detects and runs normal pictures (`PNG`, `JPG`, `WebP`, `AVIF`, `JXL`, `SVG`, `QOI`, `PPM`, `HEIC`), `GIF`s, 4K videos (`MP4`, `WebM`, `MKV`, `MOV`), live shaders (`.frag`, `.comp`), **10 built-in shader presets** (`wwal shader preset <name>`), **Steam Wallpaper Engine** scenes (IDs, folders, or preview symlinks), GNOME `.xml` dynamic timed wallpapers, slideshow directories, and solid hex colors.
- **18 Smooth Effects**: Switch wallpapers with GPU compute effects like `ripple`, `grow`, `wave`, `burn`, `glitch`, `page_curl`, `light_leak`, and `fade`, with directional crop anchors (`--crop-gravity top|center|tl|...`).
- **Instant Dual-Mode Color Themes & Template Engine**: Extracts both `dark` and `light` Material Design 3 palettes simultaneously in **`0.44 ms`**, supports ranked seed selection (`--source-color-index 0..3`, `--seed-color`), renders declarative `[template."<name>"]` files in **`0.05 ms`** using **Matugen + Wallust + Pywal** syntax and 7 pipe filters (`lighten`, `darken`, `saturate`, `alpha`, `blend`, `invert`, `complementary`), exports `~/.cache/wal/*` for Pywalfox, and saves `~/.cache/wwal/current_wallpaper.png` (`<= 1280x720`) for `hyprlock` / `swaylock` even when your wallpaper is a video, shader, or Workshop scene.
- **Solar & Time-of-Day Dynamic Wallpapers**: Built-in astronomical NOAA solar elevation calculator (`[solar]`, `[[schedule]]`, `--auto-day-night`) transitions wallpapers and switches dark/light theme polarity automatically across dawn, day, dusk, and night.
- **Smart Pause & Battery Saver**: Drops to **`0.00%` CPU and GPU** when a screen is covered by a fullscreen or maximized window, when your workspace is fully tiled, when your screen is idle, or when your laptop runs on battery.
- **Smart Sound & Workshop Layers**: Plays video and Steam Workshop `"sound"` multi-track ambient audio layers, renders Workshop `"text"` clock/label layers, and automatically ducks volume whenever another app (Firefox, Spotify, or a game) plays audio.
- **Built-In Picker & Drop-In `swww` / `awww` / `wpaperd` Compatibility**: Includes `wwal pick` (`rofi`, `fuzzel`, `walker`, `dmenu`, `json`) with `< 1 ms` warm thumbnail cache, `wwal workshop sync-previews` for GUI pickers like `waypaper`, full `swww`/`awww` v0.12.1 CLI + `SWWW_*`/`AWWW_*` env var + `~/.cache/{swww,awww}/` compatibility, and `wpaperd` config compatibility (`[default]`/`[any]`, `"30m"` intervals, `--sort`).

---

## Speed & Resource Use

WayWal is written in C23 and runs directly on your graphics card:

- **Binary footprint**: **`1.34 MB`** daemon (`wwald`) + **`0.32 MB`** client (`wwal`) stripped (`0` heavy optional `DT_NEEDED` libraries).
- **Still pictures**: `0.00%` CPU and `0.00%` GPU once the picture is on screen.
- **4K video loops**: Hardware VA-API video decoding (`~0.3%` CPU).
- **When covered by a window**: Drops to strictly **`0.00%` CPU and GPU** (`0 wakeups/sec`).
- **Dual-mode (`dark`+`light`) HCT palette + 7-filter template render**: **`0.49 ms`** end-to-end (`0.44 ms` extraction + `0.05 ms` template render).
- **Unified picker (`wwal pick --menu json`)**: **`0.93 ms`** warm cache (`61.2 ms` cold thumbnail generation).
- **Lockscreen PNG frame export (`1280×720`)**: **`13.0 ms`** native (`29.4 ms` with 4K box-filter downscale).
- **Transitions**: **1,668 – 2,734 FPS** at 4K (`3840×2160`) on GPU (and **1,176 – 24,377 FPS** at 720p on AVX2 CPU).

Want the deep technical details? Read [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md).

---

## Install

### Arch Linux (AUR)
```bash
# Prebuilt binary (fast install)
yay -S wwal-bin

# Or build latest from source
yay -S wwal-git
```

### Fedora (Copr)
```bash
sudo dnf copr enable execorn/wwal
sudo dnf install wwal
```

### openSUSE Tumbleweed (Copr)
```bash
sudo zypper addrepo https://download.copr.fedorainfracloud.org/results/execorn/wwal/opensuse-tumbleweed-\$basearch/ execorn-wwal
sudo zypper --gpg-auto-import-keys refresh
sudo zypper install wwal
```

### Ubuntu / Debian (`.deb`)
```bash
curl -LO https://github.com/Execorn/wwal/releases/latest/download/wwal_1.1.0_amd64.deb
sudo apt install ./wwal_1.1.0_amd64.deb
```

### NixOS / Nix (`flake.nix`)
```bash
nix profile install github:Execorn/wwal
```

### Build from Source (Ubuntu, Fedora, Debian, Arch, Void, Alpine)

1. Install your system build tools (`meson`, `ninja`, `gcc` or `clang`, `pkg-config`) and base libraries (`wayland`, `wayland-protocols`, `libdrm`, `mesa`, `libva`, `liburing`, `libseccomp`, `libpng`, `libjpeg-turbo`).
2. Build and install:
```bash
git clone https://github.com/Execorn/wwal.git
cd wwal
meson setup build --prefix="$HOME/.local" --buildtype=release -Doptimization=3 -Db_lto=true
meson compile -C build
meson install -C build
```

---

## Popular Commands

Start the background service once (or add `wwald &` / `swww-daemon &` to your desktop startup file):
```bash
wwald &
```

### 1. Set Any Wallpaper & Extract Colors
```bash
# Set a wallpaper with a ripple effect starting at your mouse cursor
wwal img ~/Pictures/Wallpapers/mountain.jpg --transition-type ripple --transition-pos cursor

# Keep portrait subjects framed at the top and pick the secondary accent seed (0..3)
wwal img ~/Pictures/Wallpapers/portrait.png --crop-gravity top --theme --source-color-index 1

# Universal auto-dispatch: pass a video, shader, Workshop ID, GNOME .xml, or directory to `wwal img`
wwal img 2841902341
```

### 2. Built-In Icon-Grid Wallpaper Picker
```bash
# Open Rofi / Fuzzel / Walker with cached thumbnails for images, videos, shaders & Workshop items
wwal pick

# Output structured JSON metadata + thumbnail paths for Quickshell / Eww / AGS widgets
wwal pick --menu json
```

### 3. Play a Video or GIF
```bash
# Play a looping 4K video wallpaper
wwal video ~/Videos/rain.mp4

# Play a video at 40% volume, and auto-duck it when other apps play sound
wwal video ~/Videos/scenic.mp4 --volume 0.4 --mute-on-audio
```

### 4. Use Steam Wallpaper Engine
If you own Wallpaper Engine on Steam, `wwal` discovers and renders your Workshop wallpapers natively:
```bash
# List your installed Steam Workshop wallpapers
wwal workshop list

# Apply a Workshop wallpaper by its ID or folder path
wwal workshop apply 2841902341

# Export human-readable preview symlinks (~/.cache/wwal/workshop-previews/) for Waypaper / Azote
wwal workshop sync-previews
```

### 5. Built-In Shader Presets & Mixed-Media Slideshows
```bash
# List all 10 built-in procedural shaders and launch one with PipeWire audio reactivity
wwal shader list
wwal shader preset audio_bars --audio

# Start a mixed-media slideshow with human-readable intervals and jump forward/backward
wwal slideshow ~/Pictures/Wallpapers --interval 30m --sort mtime
wwal next
wwal prev
```

### 6. Useful Controls
```bash
wwal restore       # Restore your last wallpaper on login
wwal pause         # Pause active video, shader, or Workshop animation
wwal toggle        # Toggle pause and play
wwal clear 1e1e2e  # Set a zero-RAM solid color background
wwal status        # Show current wallpaper, screen info, and active colors
```

---

## Config File (Optional)

WayWal works right out of the box with no setup. If you want to save default settings, templates, or solar schedules, copy [`contrib/config.toml.example`](contrib/config.toml.example) to `~/.config/wwal/config.toml`:

```toml
[daemon]
battery_policy = "reduce-fps"     # "ignore", "reduce-fps", or "pause-video"
auto_pause_fullscreen = true      # Pause when a window is fullscreen (0.00% CPU/GPU)
idle_timeout_sec = 300            # Pause after 5 minutes of no input
crop_gravity = "center"           # "center", "top", "bottom", "left", "right", "tl", "tr", "bl", "br"
mute_on_audio = true              # Mute wallpaper when another app plays sound

[theme]
enabled = true                    # Update desktop colors when wallpaper changes
mode = "dark"                     # Default theme polarity ("dark", "light", or "auto")
auto_day_night = true             # Auto-switch dark/light based on [solar] / time of day
source_color_index = 0            # 0..3 ranked K-Means++ seed cluster selection
caelestia_sync = true             # Sync colors with Caelestia / Quickshell
pywal_compat = true               # Write ~/.cache/wal/* for Pywalfox & Pywal tools

[template."kitty"]
input_path = "~/.config/wwal/templates/kitty.conf"
output_path = "~/.config/kitty/colors-wwal.conf"
post_hook = "killall -SIGUSR1 kitty || true"

[solar]
latitude = 37.7749
longitude = -122.4194
auto_theme_mode = true

[transition]
type = "grow"
duration = 0.8
fps = 144

[default]
path = "~/Pictures/Wallpapers"
duration = "30m"
sort = "mtime"
```

---

## More Docs

- [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md) — Wayland protocols, GPU shaders, video decoding, Workshop runtime, template syntax, and all CLI flags.
- [**Example Config**](contrib/config.toml.example) — Every `config.toml` option explained.
- [**Support & Troubleshooting**](SUPPORT.md) — Fixes for common setup questions.
- [**Contributing**](CONTRIBUTING.md) — How to build, test, and send changes.
