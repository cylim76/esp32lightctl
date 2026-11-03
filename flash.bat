@echo off
setlocal
set "PORT=%~1"
set "STAGE=%~2"
if "%PORT%"=="" goto usage
if "%STAGE%"=="" set "STAGE=1"
if not "%STAGE%"=="1" if not "%STAGE%"=="2" if not "%STAGE%"=="3" if not "%STAGE%"=="4" goto usage
where idf.py >nul 2>nul || goto no_idf
echo Flashing esp32lightctl Stage %STAGE% to %PORT%...
if exist "build-stage%STAGE%\esp32c6_light_gateway.bin" (
  idf.py -B build-stage%STAGE% -D LIGHTGW_STAGE=%STAGE% -p %PORT% flash
) else if exist "firmware\stage%STAGE%\esp32c6_light_gateway.bin" (
  python -m esptool --chip esp32c6 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 firmware\stage%STAGE%\bootloader.bin 0x8000 firmware\stage%STAGE%\partition-table.bin 0x10000 firmware\stage%STAGE%\esp32c6_light_gateway.bin
) else (
  goto no_build
)
exit /b %errorlevel%
:usage
echo Usage: flash.bat ^<COM port^> [stage]
echo Example: flash.bat COM7 1
exit /b 2
:no_idf
echo ERROR: idf.py was not found. Run ESP-IDF 5.5.1 export.bat first.
exit /b 3
:no_build
echo ERROR: neither build-stage%STAGE% nor firmware\stage%STAGE% exists.
exit /b 4
