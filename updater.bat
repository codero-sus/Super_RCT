@echo off
rem Update this Super_RCT checkout from its git remote.
rem   updater.bat           check only: fetch and list what would change
rem   updater.bat --apply   fast-forward to the upstream branch
rem Wraps tools\update.py. Does not delete itself or any files.
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 tools\update.py %*
) else (
    where python >nul 2>nul
    if errorlevel 1 (
        echo updater.bat: Python 3 was not found on PATH. 1>&2
        exit /b 1
    )
    python tools\update.py %*
)
set RC=%errorlevel%
echo.
pause
exit /b %RC%
