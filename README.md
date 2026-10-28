# WayWal (`wwal`)

[![CI](https://github.com/Execorn/wwal/actions/workflows/ci.yml/badge.svg)](https://github.com/Execorn/wwal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Execorn/wwal?color=007ec6)](https://github.com/Execorn/wwal/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

<p align="center">
  <a href="https://raw.githubusercontent.com/Execorn/wwal/assets/demo.mp4">
    <img src="https://raw.githubusercontent.com/Execorn/wwal/assets/demo.webp" alt="WayWal Demo" width="100%">
  </a>
</p>

Most Wayland setups are held together with duct tape. You end up installing five or six separate tools (`swww` or `awww`, `mpvpaper`, `linux-wallpaperengine`, `glpaper`, `wpaperd`, plus `matugen` or `pywal`) just to get wallpapers, video loops, Steam Workshop scenes, music bars, and matching system colors.

**WayWal** replaces the whole pile with one tiny C23 binary (`1.66 MB` total). Drop in a photo, a 4K video, a live shader, or a Steam Wallpaper Engine scene, and your whole desktop updates its color theme in half a millisecond. And the instant a window covers your screen, WayWal drops to `0.0%` CPU and GPU so your battery stays untouched.

---

## Why use WayWal?

- **One command for everything (`wwal img`)**: Point `wwal img` at a picture, a GIF, a 4K video, a Shadertoy file, a Steam Workshop ID, or a folder. WayWal detects the format on its own. Then it blends the new wallpaper onto your screen at your monitor's full refresh rate.
- **Native Steam Wallpaper Engine**: Run your Steam Workshop scenes straight on Wayland. Puppet meshes, particles, scripts, clocks, and sound layers all work natively. No Wine, no Proton, and no heavy wrapper.
- **Whole-desktop colors in `0.5 ms`**: Change your wallpaper and your whole system shifts colors to match in half a millisecond. WayWal builds dark and light palettes at once and updates your terminal, bar, browser, and lockscreen right away. Existing `matugen`, `wallust`, and `pywal` templates work as-is.
- **Built-in shaders & audio visualizers**: Ships with 10 ready-to-run GPU shaders, including live PipeWire audio bars and rings that move to your music.
- **Sun-tracked day and night wallpapers**: Give it your location (or a GNOME timed wallpaper) and your desktop shifts from dawn to noon to night with the real sun, flipping your apps between light and dark mode along the way.
- **Zero wasted battery (`0.0%` when covered)**: Still images use `0.0%` CPU and `0.0%` GPU once drawn. Videos use hardware VA-API decoding (`~0.3%` CPU). Open a window full screen or launch a game, and WayWal pauses right away. It mutes background sound too, then wakes back up when you can see your desktop again.
- **Drop-in replacement for `swww`, `awww`, and `wpaperd`**: Keep your old scripts and keybinds. WayWal reads `swww` and `awww` CLI flags directly, loads `wpaperd` configs, and includes `wwal pick` so you can browse thumbnails in `rofi`, `fuzzel`, or `walker` in under `1 ms`.

---

## Speed & Resource Use

WayWal is written in C23 and talks directly to Wayland, DRM/KMS, and your GPU:

- **Binary size**: **`1.34 MB`** daemon (`wwald`) + **`0.32 MB`** client (`wwal`) stripped.
- **Still pictures**: **`0.00%` CPU and `0.00%` GPU** once the frame is on screen.
- **Covered by a window**: **`0.00%` CPU, `0.00%` GPU**, and **`0` wakeups/sec**.
- **4K video loops**: **`~0.3%` CPU** via hardware VA-API decoding.
- **Theme extraction + template render**: **`0.49 ms`** end-to-end (`0.44 ms` palette + `0.05 ms` templates).
- **Wallpaper picker (`wwal pick`)**: **`0.93 ms`** warm cache (`61.2 ms` first-run thumbnail build).
- **Lockscreen frame export (`1280×720`)**: **`13.0 ms`** ready for `hyprlock` or `swaylock`.
- **4K GPU transitions**: **`1,668 – 2,734 FPS`** at `3840×2160`.

See [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md) for the full benchmark suite and internals.

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

Start the background daemon once (or add `wwald &` / `swww-daemon &` to your compositor startup file):
```bash
wwald &
```

### 1. Set Any Wallpaper & Extract Colors
```bash
# Set a wallpaper with a ripple effect starting at your mouse cursor
wwal img ~/Pictures/Wallpapers/mountain.jpg --transition-type ripple --transition-pos cursor

# Frame portrait art from the top and pick the secondary accent color (0..3)
wwal img ~/Pictures/Wallpapers/portrait.png --crop-gravity top --theme --source-color-index 1

# Pass a photo, video, shader, Workshop ID, GNOME .xml, or folder to `wwal img`
wwal img 2841902341
```

### 2. Built-In Wallpaper Picker
```bash
# Open Rofi / Fuzzel / Walker with thumbnails for images, videos, shaders & Workshop items
wwal pick

# Output JSON metadata + thumbnail paths for Quickshell / Eww / AGS widgets
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
If you own Wallpaper Engine on Steam, `wwal` finds and runs your Workshop library natively:
```bash
# List your installed Steam Workshop wallpapers
wwal workshop list

# Apply a Workshop wallpaper by its ID or folder path
wwal workshop apply 2841902341

# Create preview symlinks (~/.cache/wwal/workshop-previews/) for GUI pickers like Waypaper
wwal workshop sync-previews
```

### 5. Built-In Shaders & Slideshows
```bash
# List all 10 built-in shaders and launch one with live PipeWire audio reactivity
wwal shader list
wwal shader preset audio_bars --audio

# Run a mixed-media slideshow and step forward or backward
wwal slideshow ~/Pictures/Wallpapers --interval 30m --sort mtime
wwal next
wwal prev
```

### 6. Everyday Controls
```bash
wwal restore       # Restore your last wallpaper on login
wwal pause         # Pause active video, shader, or Workshop animation
wwal toggle        # Toggle pause and play
wwal clear 1e1e2e  # Set a zero-RAM solid color background
wwal status        # Show current wallpaper, screen info, and active colors
```

---

## Configuration

WayWal works out of the box with no setup. If you want to save default transitions, app color templates, battery rules, or sun schedules, copy the example file to `~/.config/wwal/config.toml`:

```bash
mkdir -p ~/.config/wwal
cp contrib/config.toml.example ~/.config/wwal/config.toml
```

- [**Annotated Example Config (`contrib/config.toml.example`)**](contrib/config.toml.example): Ready-to-copy `config.toml` with comments for every setting.
- [**Configuration Manual (`docs/man/wwal-config.5.scd`)**](docs/man/wwal-config.5.scd): Full guide for all TOML sections, template variables, pipe filters, and sun schedules (also available via `man 5 wwal-config`).

---

## More Docs

- [**Technical Architecture & Full CLI Reference**](docs/ARCHITECTURE.md): Wayland protocols, GPU shaders, video decoding, Workshop runtime, and all CLI flags.
- [**Support & Troubleshooting**](SUPPORT.md): Quick fixes for common setup questions.
- [**Contributing**](CONTRIBUTING.md): How to build, run tests, and send changes.
