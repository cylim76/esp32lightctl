@echo off
setlocal
set "STAGE=%~1"
if "%STAGE%"=="" set "STAGE=4"
if not "%STAGE%"=="1" if not "%STAGE%"=="2" if not "%STAGE%"=="3" if not "%STAGE%"=="4" goto usage
where idf.py >nul 2>nul || goto no_idf
echo Building esp32lightctl Stage %STAGE% in build-stage%STAGE%...
idf.py -B build-stage%STAGE% -D LIGHTGW_STAGE=%STAGE% build
exit /b %errorlevel%
:usage
echo Usage: build-stage.bat ^<1^|2^|3^|4^>
exit /b 2
:no_idf
echo ERROR: idf.py was not found. Run ESP-IDF 5.5.1 export.bat first.
exit /b 3
