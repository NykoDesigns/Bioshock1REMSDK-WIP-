# re_tool — BioShock1 SDK Reverse-Engineering CLI

A single command-line entry point that hooks into the three RE tools used by
this project so queries can be scripted and their output consumed as JSON/text:

- **Ghidra** (headless) — decompile, struct dumps, xrefs, string/byte search
- **umodel / UE Viewer** — list and export package objects
- **UE Explorer** — (paths wired in `config.json` for script-package work)

## Setup

All paths live in `config.json`. Defaults for this machine:

| Key | Value |
|-----|-------|
| `ghidra_install` | `Z:\Ghidra\ghidra_12.0.4_PUBLIC` |
| `ghidra_project_dir` / `_name` | `Z:\Bioshock1SDK\tools\level_editor` / `ghidra_project` |
| `ghidra_program` | `BioshockHD.exe` (already imported + analyzed) |
| `umodel_exe` | `Z:\UEViewer\umodel_64.exe` |
| `umodel_export_dir` | `Z:\UEViewer\export` |
| `game_content_dir` | `...\BioShock Remastered\ContentBaked\pc` |

Requires **Python 3** on PATH. No pip dependencies.

## Ghidra commands

The Ghidra side is powered by `ghidra_scripts/GhidraQuery.java` — a **Java**
GhidraScript (not Python), so it runs under the stock `analyzeHeadless.bat`
with no PyGhidra/Jython setup. Each command runs headless against the already
analyzed program with `-noanalysis` (fast, no re-analysis).

```sh
# Decompile a function by name or address
python re_tool.py decompile 0x14012ab30
python re_tool.py decompile SomeNamedFunction

# List functions whose name contains a substring (stripped binary => few hits)
python re_tool.py func LightMap

# Dump a struct / datatype layout (offset, size, field, type)
python re_tool.py struct FLightMapIndex

# References to a named symbol
python re_tool.py xref SomeSymbol

# Find an ASCII string in memory + xrefs to it (the workhorse for stripped exes)
python re_tool.py search LightMapScale

# List defined strings containing a substring
python re_tool.py strings lightmap

# Hex dump N bytes at an address
python re_tool.py data 0x116ef93c 64
```

> **Important:** Ghidra headless takes an exclusive lock on the project.
> Close the Ghidra GUI before running these, or you'll get a lock error.

### Typical stripped-binary workflow
The retail exe has no internal symbol names (`FUN_xxxxxxxx`). To locate a
routine like the lightmap serializer:
1. `strings <keyword>` → find the relevant string + its address
2. `search <keyword>` → find the string bytes + xrefs (callers)
3. `decompile <caller_addr>` → read the routine that uses it

## umodel commands (UE Viewer)

BioShock Remastered uses the umodel game tag **`bio`** (set in `config.json`).

```sh
# List the objects inside a package (resolved against the content dir)
python re_tool.py umodel-list 1-Medical.bsm

# Export everything (or filter by class / name) into the export dir
python re_tool.py umodel-export 1-Medical.bsm
python re_tool.py umodel-export 1-Medical.bsm --type Texture
python re_tool.py umodel-export 1-Medical.bsm --type Texture --name Texture49
```

## UnrealScript commands (UE Explorer replacement)

UE Explorer is GUI-only. Instead, `re_tool` drives the headless UELib-based
decompiler at `tools/decompiler/bin/Release/net48/decompile.exe` (same library
UE Explorer is built on). This turns every `.U` script package into `.uc`
source under `docs/reverse-engineering/decompiled/` that can be grepped.

```sh
# Decompile all .U packages (Core, Engine, ShockGame, ShockAI, ...) to .uc
python re_tool.py uscript-export

# Grep the decompiled source (regex, case-insensitive) -> file:line: text
python re_tool.py uscript "LightMap"
python re_tool.py uscript "function .*Plasmid"
```

If `decompile.exe` is missing, build it with:
`dotnet build tools\decompiler -c Release`

## Files
- `re_tool.py` — the CLI dispatcher
- `config.json` — tool paths (edit per machine)
- `ghidra_scripts/GhidraQuery.java` — parameterized headless query script
- `ghidra_scripts/GhidraQuery.py` — Python/PyGhidra reference port (unused by default)
