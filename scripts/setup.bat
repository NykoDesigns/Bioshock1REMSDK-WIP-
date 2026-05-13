@echo off
echo ================================================
echo  BS1SDK Dependency Setup
echo ================================================
echo.

REM Check for git
where git >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: git not found in PATH
    exit /b 1
)

echo [1/3] Fetching MinHook...
if not exist "external\minhook" (
    git clone https://github.com/TsudaKageworthy/minhook.git external/minhook 2>nul
    if %ERRORLEVEL% NEQ 0 (
        git clone https://github.com/TsudaKageworthy/minhook.git external/minhook
    )
) else (
    echo   Already present, skipping.
)

echo [2/3] Fetching Dear ImGui...
if not exist "external\imgui" (
    git clone --branch v1.90 --depth 1 https://github.com/ocornut/imgui.git external/imgui
) else (
    echo   Already present, skipping.
)

echo [3/3] Fetching Lua 5.4...
if not exist "external\lua" (
    git clone --branch v5.4.6 --depth 1 https://github.com/lua/lua.git external/lua
) else (
    echo   Already present, skipping.
)

echo.
echo ================================================
echo  Setup complete! Now run:
echo    cmake -B build -A Win32
echo    cmake --build build --config Release
echo ================================================
