# Iso Tunnel War

A native C++/Win32 prototype for a turn-based isometric tunnelling game.

The current build generates a bounded `24x24x24` cube world with procedural
heightfields carved into every face. A spherical player marker can be selected
with the mouse, then moved or tunnelled through the voxel terrain using
highlighted options.

## Build

Run this from the project folder in PowerShell:

```powershell
.\build-windows.cmd
```

The script expects Microsoft Visual C++ Build Tools or Visual Studio with the
C++ workload installed.

## Controls

- Click the sphere to select it.
- Click a highlighted green cell to move.
- Click a highlighted orange cell to tunnel one block.
- Press `WASD` to rotate the cube in 90 degree increments.
- Drag with the left mouse button to pan.
- Use the mouse wheel to zoom, up to `5x`.
- Press `R` for a new procedural seed.
- Press `C` to re-center on the player.
