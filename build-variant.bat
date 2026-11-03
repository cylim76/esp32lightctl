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
where idf.py >nul 2>nul || goto no_idf
echo Building esp32lightctl %VARIANT% in build-variant-%VARIANT%...
idf.py -B build-variant-%VARIANT% -D LIGHTGW_VARIANT=%VARIANT% build
exit /b %errorlevel%

:usage
echo Usage: build-variant.bat ^<CORE^|CORE_SENSOR^|CORE_CLOUD^|FULL^>
exit /b 2

:no_idf
echo ERROR: idf.py was not found. Run ESP-IDF 5.5.1 export.bat first.
exit /b 3
