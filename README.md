# WayWal (`wwal`)

[![CI](https://github.com/Execorn/wwal/actions/workflows/ci.yml/badge.svg)](https://github.com/Execorn/wwal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Execorn/wwal?color=007ec6)](https://github.com/Execorn/wwal/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

<p align="center">
  <a href="assets/demo.mp4">
    <img src="assets/demo.webp" alt="WayWal Demo" width="100%">
  </a>
</p>

**WayWal** is a fast, all-in-one wallpaper and color theme tool for Linux desktops (Hyprland, Sway, Niri, and other Wayland setups).

Most people install four or five separate tools just to get pictures, video loops, Steam Wallpaper Engine scenes, music bars, and matching desktop colors. WayWal does all of that in one tiny program (`< 2 MB`). It is easy to use and light on your battery.

---

## Key Features

- **Every Wallpaper Type**: Use normal pictures (`PNG`, `JPG`, `WebP`, `AVIF`, `JXL`, `SVG`), `GIF`s, 4K videos (`MP4`, `WebM`, `MKV`), live shaders, or **Steam Wallpaper Engine** scenes.
- **18 Smooth Effects**: Switch wallpapers with smooth effects like `ripple`, `grow`, `wave`, `burn`, `glitch`, `page_curl`, and `fade`.
- **Instant Color Themes**: Pulls matching colors from your wallpaper (or your playing song cover) and updates your desktop bar, shell, and terminal in under a millisecond. It keeps dark mode on by default so bright pictures never turn your apps white.
- **Smart Pause & Battery Saver**: Pauses when a screen is covered by a fullscreen or maximized window. It also pauses when your screen is idle or when your laptop runs on battery.
- **Smart Sound**: Plays sound from video or Workshop wallpapers when you want it. It mutes the wallpaper sound whenever another app (like Firefox, Spotify, or a game) plays audio.
- **3D Mouse & Workspace Motion**: Move your mouse or switch workspaces to see smooth 3D depth motion on your background.
- **Works with `swww` & `awww` Pickers**: Drop-in compatible with GUI pickers like `waypaper` and Rofi menus.

---

## Speed & Resource Use

WayWal is written in C and runs on your graphics card:

- **Still pictures**: `0%` CPU and `0%` GPU once the picture is on screen.
- **4K video loops**: Uses hardware video decoding (`~0.3%` CPU).
- **When covered by a window**: Drops to **`0.00%` CPU and GPU**.
- **Color theme update**: Under **`0.4 ms`** (no lag when changing backgrounds).
- **Transitions**: Over **2,400 FPS** at 4K on GPU (and **1,500+ FPS** on CPU).

Want the deep technical details? Read [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md).

---

## Install

### Arch Linux (AUR)
```bash
yay -S wwal-git
```

### Fedora (Copr)
```bash
sudo dnf copr enable execorn/wwal
sudo dnf install wwal
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

Start the background service once (or add `wwald &` to your desktop startup file):
```bash
wwald &
```

### 1. Set a Picture
```bash
# Set a wallpaper with a ripple effect starting at your mouse cursor
wwal img ~/Pictures/Wallpapers/mountain.jpg --transition-type ripple --transition-pos cursor

# Set a wallpaper and update your desktop colors (stays in dark mode by default)
wwal img ~/Pictures/Wallpapers/city.png --theme

# Stretch one wide picture across all screens
wwal img ~/Pictures/Wallpapers/ultrawide.jpg --scaling-mode span
```

### 2. Play a Video or GIF
```bash
# Play a looping video wallpaper
wwal video ~/Videos/rain.mp4

# Play a video at 40% volume, and mute it when other apps play sound
wwal video ~/Videos/scenic.mp4 --volume 0.4 --mute-on-audio
```

### 3. Use Steam Wallpaper Engine
If you own Wallpaper Engine on Steam, `wwal` finds your Workshop wallpapers on its own:
```bash
# List your installed Steam Workshop wallpapers
wwal workshop list

# Apply a Workshop wallpaper by its ID or folder path
wwal workshop apply 2841902341
```

### 4. Live Shaders & Slideshows
```bash
# Run a live shader that reacts to your music
wwal shader contrib/shaders/audio_spectrum.frag --audio

# Start a slideshow that picks a new picture every 10 minutes (600 seconds)
wwal slideshow ~/Pictures/Wallpapers --interval 600
```

### 5. Useful Controls
```bash
wwal restore       # Restore your last wallpaper on login
wwal pause         # Pause active video or animation
wwal toggle        # Toggle pause and play
wwal clear 1e1e2e  # Set a solid color background
wwal status        # Show current wallpaper, screen info, and active colors
```

---

## Config File (Optional)

WayWal works right out of the box with no setup. If you want to save default settings, copy [`contrib/config.toml.example`](contrib/config.toml.example) to `~/.config/wwal/config.toml`:

```toml
[daemon]
battery_policy = "reduce-fps"     # "ignore", "reduce-fps", or "pause-video"
auto_pause_fullscreen = true      # Pause when a window is fullscreen
auto_pause_maximized = false      # Pause when a window is maximized
idle_timeout_sec = 300            # Pause after 5 minutes of no input
mute_on_audio = true              # Mute wallpaper when another app plays sound

[theme]
enabled = true                    # Update desktop colors when wallpaper changes
mode = "dark"                     # Keep dark mode ("dark" or "light")
caelestia_sync = true             # Sync colors with Caelestia / Quickshell

[transition]
type = "grow"
duration = 1.0
fps = 120
```

---

## More Docs

- [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md) — Wayland protocols, GPU shaders, video decoding, Workshop runtime, and all CLI flags.
- [**Example Config**](contrib/config.toml.example) — Every `config.toml` option explained.
- [**Support & Troubleshooting**](SUPPORT.md) — Fixes for common setup questions.
- [**Contributing**](CONTRIBUTING.md) — How to build, test, and send changes.
