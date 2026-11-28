# Modules

Extension modules for the Pilot framework. Each folder contains one module: sources, build script, and a `module.json` describing it (name, description, version, ABI, devices, provided `.so` files, and load-order requirements).

Compatibility: module ABI 2, Sony ILCE-6700 firmware 2.00. Install by copying the `.so` files to `/pilot/` on the SD card and listing them in `/pilot/modules.json` — required modules must appear **before** the modules that depend on them. See [docs/USAGE.md](../docs/USAGE.md); per-module details in each folder's README (中文).

| Module | Provides | Description | Version | Requires | Release |
|---|---|---|---|---|---|
| [menu_ui](menu_ui/) | `pilot_menu_ui.so` | Pilot entry in the camera custom-settings menu: version, module list, enable state, paths | 0.3.0 | — | [menu-ui-v0.3.0](https://github.com/Himpq/Pilot/releases/tag/menu-ui-v0.3.0) |
| [desqueeze](desqueeze/) | `pilot_desqueeze.so` | Hardware VDF anamorphic de-squeeze: 7 squeeze factors × 16:9 / OpenGate, independent LCD / EVF / HDMI switches | 0.1.0 | menu_ui | [desqueeze-v0.1.0](https://github.com/Himpq/Pilot/releases/tag/desqueeze-v0.1.0) |
| [pilot_canvas](pilot_canvas/) | `pilot_canvas.so` | Live view native canvas engine with real-time waveform / scope overlay widgets | 0.2.0 | menu_ui (first) | [pilot-canvas-v0.2.0](https://github.com/Himpq/Pilot/releases/tag/pilot-canvas-v0.2.0) |
| [video_modes](video_modes/) | `pilot_video_modes.so` | Unified video mode switcher: Stock / 2K OpenGate / 6K 420 (24-50p) / 1080p (6K oversampled) | 0.1.2 | menu_ui ≥ 0.2.0 (first) | [video-modes-v0.1.2](https://github.com/Himpq/Pilot/releases/tag/video-modes-v0.1.2) |
| [6k](6k/) | `pilot_6k.so` | 5760×3240 23.976p in-camera recording in the XAVC HS 4K 24p slot | 0.1.0 | — | [6k-v0.1.0](https://github.com/Himpq/Pilot/releases/tag/6k-v0.1.0) |
