# BioShock Level Editor — Development Log

## Overview
Standalone GUI application for editing BioShock Remastered .bsm map files.
Full 3D viewport, property editing, actor placement, export/reimport.

## Tech Stack
- **Language:** C++17
- **Window/Input:** SDL2
- **Rendering:** OpenGL 3.3 (core profile)
- **GUI:** Dear ImGui (docking branch)
- **File I/O:** Existing BSM parser (bsm_tool/bsm_parser)
- **Build:** CMake

## Architecture
```
┌─────────────────────────────────────────────────┐
│  Level Editor Application                        │
├──────────────┬──────────────┬───────────────────┤
│ Scene Tree   │  3D Viewport │  Properties Panel  │
│ (ImGui)      │  (OpenGL)    │  (ImGui)          │
├──────────────┴──────────────┴───────────────────┤
│  BSM Document Layer (load/save/modify)           │
├─────────────────────────────────────────────────┤
│  BSM Parser (from bsm_tool — proven code)        │
└─────────────────────────────────────────────────┘
```

## Existing Foundation (from bsm_tool)
Already built and working:
- [x] Full .bsm package parser (names, imports, exports)
- [x] Property deserialization (all 16 UE2 types: Vector, Rotator, Float, Int, Bool, Object, Name, Array, Struct...)
- [x] Header skip auto-detection for serial data
- [x] Round-trip BSM writing (clone exports, patch header, update offsets)
- [x] Spawner cloning with position offsets
- [x] In-place property editing (Location, Rotation, Float, Int, Byte)
- [x] Search by name/class across imports/exports
- [x] CompactIndex read/write
- [x] Export entry serialization

## What Needs Building
- [ ] SDL2 + OpenGL window with ImGui
- [ ] Camera controller (orbit/fly)
- [ ] Actor rendering (boxes at Location with class-based colors)
- [ ] Scene tree (filter by class, select, focus)
- [ ] Property editor UI (bound to BSM property types)
- [ ] Transform gizmo (translate/rotate)
- [ ] Multi-select and batch operations
- [ ] Undo/redo system
- [ ] File browser for .bsm files
- [ ] BSP geometry extraction (for actual level visuals)
- [ ] Static mesh reference rendering (bounding boxes)

## Map Files
Located at: `D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\Maps\`
- Entry.bsm (20KB) — good test file
- 0-Lighthouse.bsm (187MB)
- 1-Medical.bsm (204MB)
- 1-Welcome.bsm (~200MB)
- etc.

## BSM Format Summary
- Magic: 0x9E2A83C1 (standard UE package)
- Version: 142, Licensee: 56 (Vengeance Engine / UE2.5)
- Names: UTF-16LE with CompactIndex length
- Exports: ClassIndex, SuperIndex, OuterIndex, FName, ObjectFlags(u64), SerialSize, SerialOffset
- Properties: UE1-style tags (NameRef + InfoByte + optional StructName + PackedSize + Value)
- Actors have Location (Vector), Rotation (Rotator), and class-specific properties

## Progress Log

### Session 1 (2026-05-18)
- Assessed existing codebase: 1563-line bsm_tool with full parse/write capability
- Designed architecture: SDL2 + OpenGL + ImGui
- Created project structure with all source files
- **BUILD SUCCESSFUL** — BS1LevelEditor.exe runs and opens window
- Features in this build:
  - SDL2 + OpenGL 3.3 core window (1600x900, maximized, resizable)
  - Dear ImGui docking layout (Scene Tree, Properties, Status panels)
  - 3D viewport with orbit camera (RMB: orbit, MMB: pan, scroll: zoom)
  - BSM document loader (parses actors with Location/Rotation properties)
  - Actor rendering as colored cubes (class-based color coding)
  - Scene tree with text filter and category toggles
  - Properties panel with Location/Rotation DragFloat3 editors
  - Click-to-select actor picking
  - F to focus camera on selected actor
  - Ctrl+O: native Windows file dialog (defaults to Maps folder)
  - Ctrl+S: save modified BSM back to disk
  - Ground grid (100x100, 1000 unit spacing)
- Build: `cmake -S tools/level_editor -B build_editor -G "Visual Studio 17 2022" -A x64`
- Run: `build_editor\Release\BS1LevelEditor.exe`
