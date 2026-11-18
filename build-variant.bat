@echo off
setlocal
cd /d "%~dp0"
set "VARIANT=%~1"
if "%VARIANT%"=="" goto usage
if /i "%VARIANT%"=="CORE" goto build
if /i "%VARIANT%"=="CORE_SENSOR" goto build
if /i "%VARIANT%"=="CORE_CLOUD" goto build
if /i "%VARIANT%"=="FULL" goto build
goto usage

:build
where idf.py >nul 2>nul && goto have_idf
if exist "%~dp0..\esp-idf-5.5.1\export.bat" call "%~dp0..\esp-idf-5.5.1\export.bat"
where idf.py >nul 2>nul || goto no_idf
:have_idf
for /f "delims=" %%I in ('where ninja.exe') do if not defined NINJA_EXE set "NINJA_EXE=%%I"
if not defined NINJA_EXE goto no_ninja
for %%I in ("%NINJA_EXE%") do set "NINJA_BIN=%%~dpI"
set "NINJA_BIN=%NINJA_BIN:~0,-1%"
for /f "delims=" %%I in ('where riscv32-esp-elf-gcc.exe') do if not defined RISCV_GCC set "RISCV_GCC=%%I"
if not defined RISCV_GCC goto no_toolchain
for %%I in ("%RISCV_GCC%") do set "RISCV_BIN=%%~dpI"
set "RISCV_BIN=%RISCV_BIN:~0,-1%"
set "PATH=%NINJA_BIN%;%RISCV_BIN%;%PATH%"
echo Building esp32lightctl %VARIANT% in build-variant-%VARIANT%...
idf.py --no-ccache -D "CMAKE_MAKE_PROGRAM=%NINJA_EXE%" -D "LIGHTGW_TOOLCHAIN_BIN=%RISCV_BIN%" -B build-variant-%VARIANT% -D LIGHTGW_VARIANT=%VARIANT% build
exit /b %errorlevel%

:usage
echo Usage: build-variant.bat ^<CORE^|CORE_SENSOR^|CORE_CLOUD^|FULL^>
exit /b 2

:no_idf
echo ERROR: idf.py was not found. Run ESP-IDF 5.5.1 export.bat first.
exit /b 3

:no_ninja
echo ERROR: ninja.exe was not found in the ESP-IDF environment.
exit /b 4

:no_toolchain
echo ERROR: ESP32-C6 RISC-V compiler was not found in the ESP-IDF environment.
exit /b 5
