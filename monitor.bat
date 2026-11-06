@echo off
setlocal
set "PORT=%~1"
set "STAGE=%~2"
if "%PORT%"=="" goto usage
if "%STAGE%"=="" set "STAGE=1"
if not "%STAGE%"=="1" if not "%STAGE%"=="2" if not "%STAGE%"=="3" if not "%STAGE%"=="4" if not "%STAGE%"=="5" goto usage
where idf.py >nul 2>nul && goto have_idf
if exist "%~dp0..\esp-idf-5.5.1\export.bat" call "%~dp0..\esp-idf-5.5.1\export.bat"
where idf.py >nul 2>nul || goto no_idf
:have_idf
if exist "build-stage%STAGE%\esp32c6_light_gateway.elf" (
  idf.py -B build-stage%STAGE% -p %PORT% monitor
) else (
  echo No ELF build directory found; starting plain 115200-baud serial monitor.
  echo Press Ctrl+] to exit.
  python -m serial.tools.miniterm %PORT% 115200
)
exit /b %errorlevel%
:usage
echo Usage: monitor.bat ^<COM port^> [stage]
echo Example: monitor.bat COM7 1
exit /b 2
:no_idf
echo ERROR: idf.py was not found and ..\esp-idf-5.5.1\export.bat is unavailable.
echo Install ESP-IDF 5.5.1 beside this project or run its export.bat first.
exit /b 3
