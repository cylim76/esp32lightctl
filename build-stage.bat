@echo off
setlocal
set "STAGE=%~1"
if "%STAGE%"=="" set "STAGE=4"
if not "%STAGE%"=="1" if not "%STAGE%"=="2" if not "%STAGE%"=="3" if not "%STAGE%"=="4" if not "%STAGE%"=="5" goto usage
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
echo Building esp32lightctl Stage %STAGE% in build-stage%STAGE%...
if exist "%~dp0managed_components\espressif__mdns\CMakeLists.txt" if exist "%~dp0managed_components\espressif__esp-zigbee-lib\CMakeLists.txt" (
  set "IDF_COMPONENT_MANAGER=0"
) else (
  set "IDF_COMPONENT_MANAGER=1"
)
idf.py --no-ccache -D "CMAKE_MAKE_PROGRAM=%NINJA_EXE%" -D "LIGHTGW_TOOLCHAIN_BIN=%RISCV_BIN%" -B build-stage%STAGE% -D LIGHTGW_STAGE=%STAGE% build
exit /b %errorlevel%
:usage
echo Usage: build-stage.bat ^<1^|2^|3^|4^|5^>
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
