@echo off
rem ============================================================================
rem  Super_RCT - one-time setup for building the Android app on Windows.
rem
rem  Installs what is missing:
rem    - Java 17 (JDK), with winget if available
rem    - Android SDK: platform 34, build-tools 34.0.0, platform-tools (adb)
rem    - Gradle 8.9, used once to create the Gradle wrapper in android\
rem  Then check it with:   python builder.py --check
rem  Build the APK with:   python builder.py
rem
rem  Run this file by double-clicking it or from a terminal. Open a NEW terminal
rem  after it finishes, so the new PATH and ANDROID_HOME values are picked up.
rem  Set NOPAUSE=1 before running to skip the final pause (for scripts).
rem ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "SDK=%LOCALAPPDATA%\Android\Sdk"
set "GRADLE_VER=8.9"
set "GRADLE_HOME=%LOCALAPPDATA%\Gradle\gradle-%GRADLE_VER%"
set "CMDLINE_URL=https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"
set "GRADLE_URL=https://services.gradle.org/distributions/gradle-%GRADLE_VER%-bin.zip"
set "PS=powershell -NoProfile -ExecutionPolicy Bypass -Command"

echo.
echo === Super_RCT setup for Windows ===
echo.

rem ---- 1. Python 3.8+ (runs builder.py) ------------------------------------
echo [1/5] Checking Python...
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if errorlevel 1 goto :no_python
echo       OK

rem ---- 2. Java 17 ------------------------------------------------------------
echo [2/5] Checking Java 17...
java -version >nul 2>&1
if not errorlevel 1 goto :java_ok
where winget >nul 2>&1
if errorlevel 1 goto :no_java
echo       Installing Eclipse Temurin 17 with winget (this can take a few minutes)...
winget install --id EclipseAdoptium.Temurin.17.JDK -e --silent --accept-package-agreements --accept-source-agreements
rem winget installs for new terminals only, so ask for a restart and stop here.
echo.
echo       Java 17 was installed. Open a NEW terminal and run setup.bat again.
goto :finish_restart
:java_ok
echo       OK

rem ---- 3. Android SDK --------------------------------------------------------
echo [3/5] Android SDK...
if exist "%SDK%\cmdline-tools\latest\bin\sdkmanager.bat" goto :sdk_tools_ok
echo       Downloading Android command-line tools...
%PS% "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%CMDLINE_URL%' -OutFile '%TEMP%\srct-cmdline-tools.zip'"
if errorlevel 1 goto :fail
if exist "%TEMP%\srct-cmdline-tools" rmdir /s /q "%TEMP%\srct-cmdline-tools"
%PS% "Expand-Archive -Force -Path '%TEMP%\srct-cmdline-tools.zip' -DestinationPath '%TEMP%\srct-cmdline-tools'"
if errorlevel 1 goto :fail
if not exist "%SDK%\cmdline-tools" mkdir "%SDK%\cmdline-tools"
if exist "%SDK%\cmdline-tools\latest" rmdir /s /q "%SDK%\cmdline-tools\latest"
move "%TEMP%\srct-cmdline-tools\cmdline-tools" "%SDK%\cmdline-tools\latest" >nul
if errorlevel 1 goto :fail
:sdk_tools_ok

set "SDKMANAGER=%SDK%\cmdline-tools\latest\bin\sdkmanager.bat"
echo       Accepting the SDK licences...
(for /l %%i in (1,1,60) do @echo y) | call "%SDKMANAGER%" --licenses >nul
echo       Installing platform 34, build-tools 34.0.0 and platform-tools...
call "%SDKMANAGER%" "platforms;android-34" "build-tools;34.0.0" "platform-tools"
if errorlevel 1 goto :fail

rem Make ANDROID_HOME and platform-tools (adb) available in new terminals.
%PS% "[Environment]::SetEnvironmentVariable('ANDROID_HOME', '%SDK%', 'User')"
%PS% "$p = [Environment]::GetEnvironmentVariable('Path', 'User'); $add = '%SDK%\platform-tools'; if (-not (($p -split ';') -contains $add)) { [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $add), 'User') }"
echo       OK

rem ---- 4. Gradle 8.9 and the Gradle wrapper ---------------------------------
echo [4/5] Gradle %GRADLE_VER%...
if exist "%GRADLE_HOME%\bin\gradle.bat" goto :gradle_ok
echo       Downloading Gradle %GRADLE_VER%...
%PS% "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%GRADLE_URL%' -OutFile '%TEMP%\srct-gradle.zip'"
if errorlevel 1 goto :fail
%PS% "Expand-Archive -Force -Path '%TEMP%\srct-gradle.zip' -DestinationPath '%LOCALAPPDATA%\Gradle'"
if errorlevel 1 goto :fail
:gradle_ok
if exist "android\gradlew.bat" goto :wrapper_ok
echo       Creating the Gradle wrapper in android\ ...
call "%GRADLE_HOME%\bin\gradle.bat" -p android wrapper --gradle-version %GRADLE_VER% --console=plain
if errorlevel 1 goto :fail
:wrapper_ok
echo       OK

rem ---- 5. Check the result -----------------------------------------------------
echo [5/5] Checking the tools with builder.py...
python builder.py --check
if errorlevel 1 goto :fail

echo.
echo === Setup finished. ===
echo Next: build the app with   python builder.py
echo       install it on your phone with   python builder.py --install
echo (Open a NEW terminal first if this one still reports missing tools.)
goto :finish_ok

:no_python
echo.
echo Python 3.8 or newer is needed (builder.py runs on it).
echo Install it from https://www.python.org/downloads/ (tick "Add python.exe to PATH"),
echo then open a new terminal and run setup.bat again.
goto :finish_fail

:no_java
echo.
echo Java 17 is missing and winget is not available on this PC.
echo Install "Temurin 17 (JDK)" from https://adoptium.net/ , open a new terminal,
echo then run setup.bat again.
goto :finish_fail

:fail
echo.
echo Setup stopped because a step failed. Read the messages above, fix the problem
echo (for example, check the internet connection), then run setup.bat again.
goto :finish_fail

:finish_restart
if not defined NOPAUSE pause
endlocal
exit /b 0

:finish_ok
if not defined NOPAUSE pause
endlocal
exit /b 0

:finish_fail
if not defined NOPAUSE pause
endlocal
exit /b 1
