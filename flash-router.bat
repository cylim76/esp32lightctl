@echo off
setlocal
set "PORT=%~1"
set "MODE=%~2"
if "%PORT%"=="" goto usage
where idf.py >nul 2>nul && goto have_idf
if exist "%~dp0..\esp-idf-5.5.1\export.bat" call "%~dp0..\esp-idf-5.5.1\export.bat"
where idf.py >nul 2>nul || goto no_idf
:have_idf
for /f "delims=" %%I in ('where ninja.exe') do if not defined NINJA_EXE set "NINJA_EXE=%%I"
if not defined NINJA_EXE goto no_ninja
for %%I in ("%NINJA_EXE%") do set "NINJA_BIN=%%~dpI"
set "NINJA_BIN=%NINJA_BIN:~0,-1%"
for /f "delims=" %%I in ('where riscv32-esp-elf-gcc.exe') do if not defined RISCV_GCC set "RISCV_GCC=%%I"
for /f "delims=" %%I in ('where riscv32-esp-elf-g++.exe') do if not defined RISCV_GXX set "RISCV_GXX=%%I"
if not defined RISCV_GCC goto no_toolchain
if not defined RISCV_GXX goto no_toolchain
for %%I in ("%RISCV_GCC%") do set "RISCV_BIN=%%~dpI"
set "RISCV_BIN=%RISCV_BIN:~0,-1%"
set "PATH=%NINJA_BIN%;%RISCV_BIN%;%PATH%"
if exist "%~dp0router_firmware\managed_components\espressif__esp-zigbee-lib\CMakeLists.txt" (
  set "IDF_COMPONENT_MANAGER=0"
) else (
  set "IDF_COMPONENT_MANAGER=1"
)
if /I "%MODE%"=="fresh" goto fresh
echo Flashing Router firmware without erasing existing data on %PORT%...
if exist "build-router\esp32c6_zigbee_router.bin" (
  idf.py --no-ccache -D "CMAKE_MAKE_PROGRAM=%NINJA_EXE%" -D "ROUTER_TOOLCHAIN_BIN=%RISCV_BIN%" -C router_firmware -B build-router -p %PORT% flash
) else if exist "firmware\router\esp32c6_zigbee_router.bin" (
  python -m esptool --chip esp32c6 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 firmware\router\bootloader.bin 0x8000 firmware\router\partition-table.bin 0x10000 firmware\router\esp32c6_zigbee_router.bin
) else (
  goto no_build
)
exit /b %errorlevel%

:fresh
echo WARNING: fresh mode erases the entire connected board before installing Router firmware.
if exist "build-router\esp32c6_zigbee_router.bin" (
  idf.py --no-ccache -D "CMAKE_MAKE_PROGRAM=%NINJA_EXE%" -D "ROUTER_TOOLCHAIN_BIN=%RISCV_BIN%" -C router_firmware -B build-router -p %PORT% erase-flash flash
) else if exist "firmware\router\esp32c6_zigbee_router.bin" (
  python -m esptool --chip esp32c6 -p %PORT% --before default_reset erase_flash
  if errorlevel 1 exit /b %errorlevel%
  python -m esptool --chip esp32c6 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 firmware\router\bootloader.bin 0x8000 firmware\router\partition-table.bin 0x10000 firmware\router\esp32c6_zigbee_router.bin
) else (
  goto no_build
)
exit /b %errorlevel%

:usage
echo Usage: flash-router.bat ^<COM port^> [fresh]
echo First conversion from gateway to router: flash-router.bat COM12 fresh
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

:no_build
echo ERROR: neither build-router nor firmware\router contains a complete Router image.
exit /b 6
