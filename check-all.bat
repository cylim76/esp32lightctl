@echo off
setlocal
cd /d "%~dp0"

if not defined IDF_PATH (
  if exist "D:\RPA\esp-idf-5.5.1\export.bat" call "D:\RPA\esp-idf-5.5.1\export.bat"
)
if not defined IDF_PATH (
  echo ERROR: ESP-IDF environment is not active.
  exit /b 2
)

python tests\static_storage_check.py || exit /b 1
python tests\test_web_storage_contract.py || exit /b 1
python tests\test_variant_contract.py || exit /b 1
python tests\test_backup_contract.py || exit /b 1
python tests\test_backup_restore.py || exit /b 1
python tests\test_ci_workflow.py || exit /b 1
python tests\simulate_time_scheduler.py || exit /b 1

if defined NODE_EXE goto have_node
where node >nul 2>nul || goto no_node
set "NODE_EXE=node"
:have_node
"%NODE_EXE%" tests\check_web_js.js || exit /b 1

pushd tests\phase_a_app
idf.py -B build-quality-c6 -DIDF_TARGET=esp32c6 build || (popd & exit /b 1)
popd

for %%S in (1 2 3 4 5) do (
  idf.py -B build-quality-stage%%S -D LIGHTGW_STAGE=%%S build || exit /b 1
  idf.py -B build-quality-stage%%S size || exit /b 1
  idf.py -B build-quality-stage%%S size-components || exit /b 1
)

for %%V in (CORE CORE_SENSOR CORE_CLOUD FULL) do (
  idf.py -B build-quality-variant-%%V -D LIGHTGW_VARIANT=%%V build || exit /b 1
  idf.py -B build-quality-variant-%%V size || exit /b 1
  idf.py -B build-quality-variant-%%V size-components || exit /b 1
)

python tests\report_build_metrics.py || exit /b 1

echo CHECK_ALL PASS
endlocal
exit /b 0

:no_node
echo ERROR: Node.js is required for the embedded Web JavaScript syntax check.
echo Set NODE_EXE to the full path of node.exe and run check-all.bat again.
exit /b 2
