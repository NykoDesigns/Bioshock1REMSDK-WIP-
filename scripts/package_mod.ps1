#!/usr/bin/env pwsh
# ═══════════════════════════════════════════════════════════════════════
# BS1SDK Mod Packager — Creates a distributable mod zip
# ═══════════════════════════════════════════════════════════════════════
# 
# Usage: .\scripts\package_mod.ps1 [-Name "MyModPack"] [-IncludeBSM] [-BSMDir "path"]
#
# Output: dist/<Name>.zip containing:
#   winmm.dll          — Proxy loader (auto-loads BS1SDK.dll)
#   BS1SDK.dll         — Main mod DLL
#   mod_config.json    — Mod settings (editable by end user)
#   scripts/           — Lua scripts (if any)
#   INSTALL.txt        — Installation instructions
#
# Optional: patched .bsm files if -IncludeBSM is specified

param(
    [string]$Name = "BS1SDK_Mod",
    [switch]$IncludeBSM,
    [string]$BSMDir = ""
)

$ErrorActionPreference = "Stop"

$BuildDir = "$PSScriptRoot\..\build\bin\Release"
$DistDir = "$PSScriptRoot\..\dist"
$StageDir = "$DistDir\$Name"

Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "  BS1SDK Mod Packager" -ForegroundColor Cyan
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host ""

# Check build exists
if (-not (Test-Path "$BuildDir\BS1SDK.dll")) {
    Write-Host "ERROR: BS1SDK.dll not found. Build the project first:" -ForegroundColor Red
    Write-Host "  cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path "$BuildDir\winmm.dll")) {
    Write-Host "ERROR: winmm.dll (proxy) not found. Build winmm_proxy target:" -ForegroundColor Red
    Write-Host "  cmake --build build --config Release --target winmm_proxy" -ForegroundColor Yellow
    exit 1
}

# Clean staging area
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Path $StageDir | Out-Null

Write-Host "  Staging to: $StageDir" -ForegroundColor Gray

# Copy core files
Copy-Item "$BuildDir\winmm.dll" "$StageDir\winmm.dll"
Copy-Item "$BuildDir\BS1SDK.dll" "$StageDir\BS1SDK.dll"
Write-Host "  [+] winmm.dll (proxy loader)" -ForegroundColor Green
Write-Host "  [+] BS1SDK.dll (mod engine)" -ForegroundColor Green

# Generate default config if not present
$ConfigPath = "$StageDir\mod_config.json"
@"
{
  "_comment": "BS1SDK Mod Configuration - edit values and restart game",

  "autoInitMods": true,
  "showOverlay": true,

  "decoyTeleport": true,

  "friendlyBots": true,
  "friendlyBotLimit": 3,

  "rivetPistol": false,

  "splicerFactions": false,

  "chainLightning": true,
  "chainRadius": 500.0,
  "chainMaxJumps": 3,
  "chainDamageFalloff": 0.5
}
"@ | Set-Content $ConfigPath -Encoding UTF8
Write-Host "  [+] mod_config.json (settings)" -ForegroundColor Green

# Copy Lua scripts if any exist
$ScriptsDir = "$PSScriptRoot\..\scripts"
$LuaFiles = Get-ChildItem "$ScriptsDir\*.lua" -ErrorAction SilentlyContinue
if ($LuaFiles) {
    New-Item -ItemType Directory -Path "$StageDir\scripts" | Out-Null
    foreach ($f in $LuaFiles) {
        Copy-Item $f.FullName "$StageDir\scripts\"
    }
    Write-Host "  [+] scripts/ ($($LuaFiles.Count) Lua files)" -ForegroundColor Green
}

# Copy patched BSM files if requested
if ($IncludeBSM -and $BSMDir) {
    $BSMFiles = Get-ChildItem "$BSMDir\*.bsm" -ErrorAction SilentlyContinue
    if ($BSMFiles) {
        $BSMStage = "$StageDir\Maps"
        New-Item -ItemType Directory -Path $BSMStage | Out-Null
        foreach ($f in $BSMFiles) {
            Copy-Item $f.FullName "$BSMStage\"
        }
        Write-Host "  [+] Maps/ ($($BSMFiles.Count) patched .bsm files)" -ForegroundColor Green
    }
}

# Generate INSTALL.txt
@"
╔══════════════════════════════════════════════════════════════════╗
║  BS1SDK Gameplay Mod Pack — Installation Guide                  ║
╚══════════════════════════════════════════════════════════════════╝

REQUIREMENTS:
  - BioShock Remastered (Steam)

INSTALLATION:
  1. Navigate to your BioShock Remastered game folder:
     Steam > Right-click BioShock Remastered > Manage > Browse Local Files
     Then go to: Build\Final\

  2. Copy these files into that folder:
     - winmm.dll
     - BS1SDK.dll
     - mod_config.json

  3. (Optional) Copy the 'scripts' folder if included.

  4. (Optional) If 'Maps' folder is included, copy its .bsm files to:
     ContentBaked\pc\Maps\
     (BACK UP your originals first!)

  5. Launch the game normally through Steam.

USAGE:
  - Press F1 to toggle the mod overlay
  - Press ~ (tilde) to open the console
  - Type 'help' for all available commands
  - Type 'mods' to see active mod status
  - Edit mod_config.json to change settings (restart required)

ACTIVE MODS:
  - Decoy Plasmid → Teleportation (use Decoy to teleport)
  - Chain Lightning (Electro Bolt chains to nearby enemies)
  - Friendly Security Bots (Security Command spawns allied bots, max 3)
  - Rivet Pistol (toggle: 'rivets on' in console)
  - Splicer Factions (toggle: 'factions on', then 'tag 1'/'tag 2')

UNINSTALL:
  Delete winmm.dll, BS1SDK.dll, and mod_config.json from Build\Final\
  Restore any backed-up .bsm files if applicable.

"@ | Set-Content "$StageDir\INSTALL.txt" -Encoding UTF8
Write-Host "  [+] INSTALL.txt" -ForegroundColor Green

# Create zip
$ZipPath = "$DistDir\$Name.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath }
Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath
Write-Host ""
Write-Host "  Package created: $ZipPath" -ForegroundColor Cyan

$Size = [math]::Round((Get-Item $ZipPath).Length / 1KB, 1)
Write-Host "  Size: ${Size} KB" -ForegroundColor Gray
Write-Host ""
Write-Host "  Users just extract to Build\Final\ and launch the game!" -ForegroundColor Yellow
Write-Host ""
