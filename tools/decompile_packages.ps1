#!/usr/bin/env pwsh
# Batch decompiler for BioShock Remastered .U packages using UE Explorer
#
# Usage: .\decompile_packages.ps1
#
# This script opens each .U package in UE Explorer for manual decompilation.
# UE Explorer is a GUI app — use "Export All Scripts" from the File menu.
#
# For automated batch decompilation, use the C# script decompile_batch.csx
# with dotnet-script or build the UELib console tool.

$scriptDir = "D:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BakedScripts\pc"
$ueExplorer = "z:\Bioshock1SDK\tools\UE-Explorer\UEExplorer.exe"
$outputDir = "z:\Bioshock1SDK\docs\reverse-engineering\decompiled"

# Create output directory
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

# Priority order — decompile these first
$packages = @(
    "ShockGame.U",    # All gameplay: weapons, plasmids, enemies, items, doors
    "ShockAI.U",      # All AI: perception, targeting, combat states
    "Engine.U",       # Base engine: Actor, Pawn, Controller, Mover
    "Core.U",         # UObject system
    "Scripting.U",    # Kismet scripting
    "VengeanceShared.U", # Vengeance engine extensions
    "Tyrion.U"        # Behavior trees
)

Write-Host "=== BioShock Remastered Script Package Decompiler ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Script packages found at:" -ForegroundColor Yellow
Write-Host "  $scriptDir" -ForegroundColor Gray
Write-Host ""
Write-Host "UE Explorer will open each package for decompilation." -ForegroundColor Yellow
Write-Host "In UE Explorer:" -ForegroundColor Yellow
Write-Host '  1. File > Open > select the .U file' -ForegroundColor Gray
Write-Host '  2. Wait for parsing (ShockGame.U takes ~30s)' -ForegroundColor Gray
Write-Host '  3. File > Export All Scripts' -ForegroundColor Gray
Write-Host "  4. Save to: $outputDir\<PackageName>\" -ForegroundColor Gray
Write-Host ""

foreach ($pkg in $packages) {
    $fullPath = Join-Path $scriptDir $pkg
    if (Test-Path $fullPath) {
        $size = [math]::Round((Get-Item $fullPath).Length / 1024)
        Write-Host "[$pkg] ${size} KB — Ready" -ForegroundColor Green
    } else {
        Write-Host "[$pkg] NOT FOUND" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "Opening UE Explorer..." -ForegroundColor Cyan

# Open UE Explorer with the first (most important) package
$firstPkg = Join-Path $scriptDir $packages[0]
Start-Process $ueExplorer -ArgumentList "`"$firstPkg`""

Write-Host ""
Write-Host "TIP: After exporting, the decompiled .uc files contain the full" -ForegroundColor Yellow
Write-Host "UnrealScript source for every class, function, and state machine." -ForegroundColor Yellow
Write-Host "This is the complete game logic source code." -ForegroundColor Yellow
