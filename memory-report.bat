@echo off
setlocal
set "STAGE=%~1"
if "%STAGE%"=="" goto usage
if not "%STAGE%"=="1" if not "%STAGE%"=="2" if not "%STAGE%"=="3" if not "%STAGE%"=="4" if not "%STAGE%"=="5" goto usage
where idf.py >nul 2>nul || goto no_idf
if not exist "build-stage%STAGE%\esp32c6_light_gateway.map" goto no_build
echo === esp32lightctl Stage %STAGE%: idf.py size ===
idf.py -B build-stage%STAGE% size || exit /b %errorlevel%
echo === esp32lightctl Stage %STAGE%: idf.py size-components ===
idf.py -B build-stage%STAGE% size-components
exit /b %errorlevel%
:usage
echo Usage: memory-report.bat ^<1^|2^|3^|4^|5^>
exit /b 2
:no_idf
echo ERROR: idf.py was not found. Run ESP-IDF 5.5.1 export.bat first.
exit /b 3
:no_build
echo ERROR: build-stage%STAGE% does not contain a completed build. Run build-stage.bat %STAGE% first.
exit /b 4
