# PhotoWidget

Minimal iOS-style borderless photo & animated GIF widget for the Windows 11 desktop, built in native C++ (Win32 + Direct2D + WIC).

## Features
- Borderless rounded cards (`WS_EX_LAYERED`, per-pixel alpha, centered aspect-fill)
- Anchored to desktop layer (`Progman` owner + `HWND_BOTTOM`, stays visible on `Win + D`)
- Supports `.png`, `.jpg`, `.jpeg`, `.webp`, `.bmp`, and animated `.gif`
- Free drag-to-move, edge/corner drag-to-resize, drag-and-drop image replacement
- Native Windows 11 dark-mode right-click menu:
  - Change photo / Duplicate widget (all cards run in a single process)
  - Size presets, lock position & size, auto-start with Windows (auto-syncs moved `.exe` path)
  - Automatic memory & working-set trim (`~10 MB` static photo, `~20 MB` GIF)

## Build
Requires `clang++` (e.g. from Windhawk's LLVM/MinGW toolchain):
```powershell
.\build.ps1
```
