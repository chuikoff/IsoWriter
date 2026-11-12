@echo off
setlocal enabledelayedexpansion

set "CMAKE_EXE=cmake"
where cmake >nul 2>nul
if %errorlevel% neq 0 (
    set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not exist "%CMAKE_EXE%" (
    echo Error: cmake.exe not found!
    exit /b 1
)

echo Using CMake: "%CMAKE_EXE%"

echo.
echo ==============================================
echo [1/4] Building 64-bit plugin (IsoWriter.wfx64)
echo ==============================================
"%CMAKE_EXE%" -B build64 -S . -A x64
if %errorlevel% neq 0 exit /b %errorlevel%
"%CMAKE_EXE%" --build build64 --config Release
if %errorlevel% neq 0 exit /b %errorlevel%

echo.
echo ==============================================
echo [2/4] Building 32-bit plugin (IsoWriter.wfx)
echo ==============================================
"%CMAKE_EXE%" -B build32 -S . -A Win32
if %errorlevel% neq 0 exit /b %errorlevel%
"%CMAKE_EXE%" --build build32 --config Release
if %errorlevel% neq 0 exit /b %errorlevel%

echo.
echo ==============================================
echo [3/4] Copying files to dist/
echo ==============================================
if not exist dist mkdir dist
copy /Y build64\Release\IsoWriter.wfx64 dist\IsoWriter.wfx64
copy /Y build32\Release\IsoWriter.wfx dist\IsoWriter.wfx
copy /Y pluginst.inf dist\pluginst.inf
copy /Y res\IsoWriter.ico dist\IsoWriter.ico

echo.
echo ==============================================
echo [4/4] Creating Total Commander installation package (IsoWriter.zip)
echo ==============================================
powershell -NoProfile -Command "Compress-Archive -Path dist\* -DestinationPath IsoWriter.zip -Force"

echo.
echo ==============================================
echo SUCCESS! Plugin built and packaged:
echo - dist\IsoWriter.wfx64 (64-bit)
echo - dist\IsoWriter.wfx   (32-bit)
echo - dist\IsoWriter.ico   (Plugin Icon)
echo - IsoWriter.zip (TC auto-install package)
echo ==============================================
