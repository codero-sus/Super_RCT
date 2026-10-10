@echo off
rem Update this Super_RCT checkout from its git remote.
rem   updater.bat           check only: fetch and list what would change
rem   updater.bat --apply   fast-forward to the upstream branch
rem The Python interpreter comes from PYTHON= in python.env (see python.env.example).
rem Without python.env, the py launcher or python is used from PATH.
rem Wraps tools\update.py. Does not delete itself or any files.
setlocal
cd /d "%~dp0"
set "PY="
set "PYARGS="
if exist python.env (
    for /f "usebackq tokens=1,* delims==" %%a in ("python.env") do (
        if /i "%%a"=="PYTHON" set "PY=%%b"
    )
    if not defined PY (
        echo updater.bat: python.env has no PYTHON= value. 1>&2
        pause
        exit /b 1
    )
) else (
    where py >nul 2>nul
    if not errorlevel 1 (
        set "PY=py"
        set "PYARGS=-3"
    ) else (
        where python >nul 2>nul
        if not errorlevel 1 (
            set "PY=python"
        ) else (
            echo updater.bat: no python.env and no Python 3 on PATH. 1>&2
            pause
            exit /b 1
        )
    )
)
"%PY%" %PYARGS% tools\update.py %*
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
