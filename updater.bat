@echo off
rem Update this Super_RCT checkout from its git remote. Works on Windows.
rem   updater.bat           check only: fetch and list what would change
rem   updater.bat --apply   fast-forward to the upstream branch
rem
rem Python interpreter, first match wins:
rem   1. PYTHON= in python.env in this folder (optional file)
rem   2. environment variable 2PY2 (the "second Python": portable or embeddable builds)
rem   3. the py launcher (py -3), then python, from PATH
rem Wraps tools\update.py. Does not delete itself or any files.
setlocal
cd /d "%~dp0"
set "PY="
set "PYARGS="
set "PYSRC="

if exist python.env for /f "usebackq tokens=1,* delims==" %%a in ("python.env") do if /i "%%a"=="PYTHON" call :set_py "%%b" "python.env"

if defined PY goto check_py
for /f "tokens=1,* delims==" %%a in ('set') do if /i "%%a"=="2PY2" call :set_py "%%b" "environment variable 2PY2"
if defined PY goto check_py

where py >nul 2>nul
if not errorlevel 1 (
    set "PY=py"
    set "PYARGS=-3"
    set "PYSRC=py launcher"
    goto check_py
)
where python >nul 2>nul
if not errorlevel 1 (
    set "PY=python"
    set "PYSRC=PATH"
    goto check_py
)
echo updater.bat: no Python found. Set PYTHON= in python.env, set 2PY2, or install Python 3. 1>&2
goto fail

:check_py
if exist "%PY%" goto run
where "%PY%" >nul 2>nul
if errorlevel 1 (
    echo updater.bat: Python not found at "%PY%" ^(from %PYSRC%^). 1>&2
    goto fail
)

:run
"%PY%" %PYARGS% tools\update.py %*
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%

:fail
pause
exit /b 1

:set_py
rem Sets PY from the first source that has a value. %~1 = value, %~2 = source name.
if "%~1"=="" exit /b 0
if defined PY exit /b 0
set "PY=%~1"
set "PYSRC=%~2"
exit /b 0
