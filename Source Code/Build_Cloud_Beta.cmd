@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>&1
if errorlevel 1 (
    echo CMake is missing. Run this command from a RosBE build prompt.
    exit /b 1
)
where ninja >nul 2>&1
if errorlevel 1 (
    echo Ninja is missing. Run this command from a RosBE build prompt.
    exit /b 1
)
cmake -S . -B build_cloud_beta -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build_cloud_beta
if errorlevel 1 exit /b 1
echo.
echo Native Google Drive Beta 1.5.1 (dialog UI) compiled. Start: build_cloud_beta\wordpad.exe
exit /b 0
