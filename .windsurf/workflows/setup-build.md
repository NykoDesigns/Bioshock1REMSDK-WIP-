---
description: Build and deploy BS1SDK on a fresh PC (co-op testing setup)
---

# BS1SDK Fresh PC Build & Deploy

## Context for AI

This is a BioShock Remastered modding SDK. It's a C++ project using CMake that builds:
- `winmm.dll` — DLL proxy that auto-loads the SDK when the game starts
- `BS1SDK.dll` — The main SDK with ImGui overlay, ProcessEvent hooks, gameplay mods, and **co-op networking**

The game is **32-bit (x86)** so all builds MUST use `-A Win32`.

Dependencies (auto-fetched by CMake via FetchContent):
- imgui (DirectX 11 backend)
- minhook (function hooking)
- lua (scripting)

## Prerequisites

- Visual Studio 2022 with "Desktop development with C++" workload
- CMake 3.20+ (comes with VS or install separately)
- Git (to clone the repo)

## Build Steps

```powershell
# 1. Navigate to repo root
# 2. Configure (MUST be Win32 - game is 32-bit)
cmake -B build -A Win32

# 3. Build Release
cmake --build build --config Release
```

Output files will be in: `build\bin\Release\`

## Deploy to Game

Copy these files to BioShock Remastered's game folder:

```
build\bin\Release\winmm.dll   → <Steam>\steamapps\common\BioShock Remastered\Build\Final\winmm.dll
build\bin\Release\BS1SDK.dll  → <Steam>\steamapps\common\BioShock Remastered\Build\Final\BS1SDK.dll
```

Default Steam path: `C:\Program Files (x86)\Steam\steamapps\common\BioShock Remastered\Build\Final\`

## Testing Co-op

This PC is the SECOND player for co-op testing. The first player (host) is on another PC.

### Connection steps:
1. Launch BioShock Remastered normally via Steam
2. Load into a level (same level as the host!)
3. Press INSERT to toggle the SDK overlay
4. Open console (~ key or type in the console input)
5. Type: `join <HOST_IP>` (e.g., `join 192.168.1.100`)
6. Type: `netstatus` to verify connection

### The host (other PC) does:
1. Load same level
2. Type: `host`

### Network requirements:
- Both PCs on same LAN (or port-forward UDP 27015)
- Firewall must allow UDP port 27015

### Verify it works:
- `netstatus` — should show "Connected" + remote player position
- Move around — other player sees cyan diamond marker at your position
- `chat hello` — sends text message to partner

## Project Structure (key files)

```
src/
  core/          — logging, config
  engine/        — UObject, GObjects, ProcessEvent hooks
  render/        — ImGui overlay (DX11), console commands
  hooks/         — DLL hooking infrastructure
  gameplay/      — teleport plasmid, gameplay mods
  network/       — CO-OP NETWORKING
    net_common.h    — packet protocol (magic 0x42533153, 30Hz state sync)
    udp_socket.*    — Winsock2 UDP wrapper
    net_manager.*   — host/join, handshake, peer tracking
    coop_bridge.*   — high-level API, ProcessEvent tick hook
    coop_render.*   — 3D overlay marker for remote player
    coop_sync.*     — damage + world state synchronization
external/        — third-party libs (imgui sources)
tools/           — bsm_tool, ini_tool
```

## Console Commands Available

| Command | What it does |
|---------|-------------|
| `host [port]` | Start hosting co-op (default port 27015) |
| `join <ip> [port]` | Connect to host |
| `netstatus` | Show connection info |
| `disconnect` | Leave session |
| `chat <msg>` | Send message to partner |
| `teleport` | Toggle teleport plasmid |
| `god` | God mode |
| `noclip` | Fly through walls |
| `save_config` | Save mod settings |

## Troubleshooting

- **"Cannot find compiler"** → Install VS 2022 C++ workload
- **"Generator not found"** → Make sure cmake is in PATH, or use VS Developer PowerShell
- **Game crashes on launch** → Check that both DLLs are 32-bit (built with -A Win32)
- **Overlay doesn't appear** → Press INSERT key; check that winmm.dll is in the game's Build\Final folder
- **Can't connect** → Check firewall, verify both on same level, try `ping` between PCs first
- **Level mismatch warning** → Both players must load the same map before connecting
