# How to Build the Level Editor

## Prerequisites

1. **Visual Studio 2022** (free Community Edition)
   - Download: https://visualstudio.microsoft.com/downloads/
   - During install, check **"Desktop development with C++"**
   - Wait for install to finish

2. **CMake** (3.20 or newer)
   - Download: https://cmake.org/download/
   - Get the **Windows x64 Installer** (.msi)
   - During install, select **"Add CMake to the system PATH"**

## Build Steps

1. Open **Command Prompt** (search "cmd" in Start Menu)

2. Navigate to the level editor folder:
   ```
   cd path\to\Bioshock1REMSDK\tools\level_editor
   ```
   For example if you downloaded to your Downloads folder:
   ```
   cd %USERPROFILE%\Downloads\Bioshock1REMSDK-WIP--main\Bioshock1REMSDK-WIP--main\tools\level_editor
   ```

3. Run these two commands (copy-paste one at a time):
   ```
   cmake -B build -A Win32
   ```
   Wait for it to finish (downloads SDL2 and ImGui automatically, may take 1-2 minutes).

   ```
   cmake --build build --config Release
   ```
   Wait for compilation to finish (may take 2-5 minutes).

4. Your .exe is now at:
   ```
   build\Release\BS1LevelEditor.exe
   ```

## Troubleshooting

**"cmake is not recognized"**
- You didn't add CMake to PATH during install. Reinstall CMake and check "Add to PATH", or use the full path: `"C:\Program Files\CMake\bin\cmake.exe"`

**"No generators found" or "Visual Studio not found"**
- Make sure Visual Studio is fully installed with the C++ workload
- Restart your command prompt after installing VS

**Downloads fail during cmake configure**
- Check your internet connection. CMake downloads SDL2 and ImGui from GitHub during the first step.

**Build errors about missing headers**
- Delete the `build` folder and re-run both commands from scratch:
  ```
  rmdir /s /q build
  cmake -B build -A Win32
  cmake --build build --config Release
  ```
