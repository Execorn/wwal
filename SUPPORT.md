# Support & Troubleshooting

## Documentation

After installing `wwal`, read the manual pages for full CLI flag and configuration documentation:

- `man 1 wwal` (client commands and flags)
- `man 1 wwald` (daemon options and IPC socket lifecycle)
- `man 5 wwal-config` (`~/.config/wwal/config.toml` schema)

## Common Diagnostic Steps

1. **Inspect live daemon state and GPU modifiers**:
   ```bash
   wwal stats
   wwal query
   ```
   `wwal stats` prints current daemon RSS/PSS memory, active VRAM DMA-BUF surface count, the negotiated DRM FourCC format and modifier, VBlank PLL timing jitter, and the active power-governor `pause_mask`.

2. **Run the daemon in the foreground with verbose logs**:
   ```bash
   wwal kill
   wwald --verbose
   ```

3. **Hardware video decoding (`VA-API`) issues**:
   Verify that `vainfo` succeeds on your GPU:
   ```bash
   vainfo --display drm --device /dev/dri/renderD128
   ```
   On multi-GPU laptops or NVIDIA systems using `nvidia-vaapi-driver`, you can set `LIBVA_DRIVER_NAME=iHD` (Intel), `LIBVA_DRIVER_NAME=radeonsi` (AMD), or `LIBVA_DRIVER_NAME=nvidia` before starting `wwald`.

4. **Wayland protocol debugging**:
   Capture the exact Wayland wire traffic between `wwald` and your compositor:
   ```bash
   WAYLAND_DEBUG=1 wwald --verbose
   ```

## Getting Help

- **Questions, custom shaders, and configuration sharing**: Open a thread in [GitHub Discussions](https://github.com/Execorn/wwal/discussions).
- **Reproducible bugs**: Open a report using the [Bug Report template](https://github.com/Execorn/wwal/issues/new/choose).
