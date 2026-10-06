@echo off
rem Builds benchmarkDraw for Android, runs it on the phone that adb sees, and leaves the results in bin.
rem
rem Usage: benchmarkAndroid.bat [--abi abi] [--core N] [benchmarkDraw arguments...]
rem   --abi   Processor of the build: arm64-v8a by default, armeabi-v7a for a phone of 32 bits.
rem   --core  Core where the benchmark runs. By default, the one with the highest maximum frequency.
rem   The other arguments go to benchmarkDraw. The script already adds --csv.
rem
rem It needs the Android SDK with an NDK and a CMake (Android Studio, SDK Manager, SDK Tools).
rem With several phones connected, set ANDROID_SERIAL to the serial number that "adb devices" shows.

setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "ROOT=%%~fp"
set "DEVICE_DIR=/data/local/tmp/fontrenderer"

set "ABI=arm64-v8a"
set "CORE=fastest"
set "ARGS="
set "NEXT="
for %%a in (%*) do (
    if "!NEXT!" == "abi" (
        set "ABI=%%~a"
        set "NEXT="
    ) else if "!NEXT!" == "core" (
        set "CORE=%%~a"
        set "NEXT="
    ) else if "%%~a" == "--abi" (
        set "NEXT=abi"
    ) else if "%%~a" == "--core" (
        set "NEXT=core"
    ) else (
        rem adb joins its arguments with spaces and the shell of the phone splits them again,
        rem so the quotes keep a scenario like "14 px" in one argument.
        set "ARGS=!ARGS! '%%~a'"
    )
)

set "SDK=%ANDROID_HOME%"
if not defined SDK (
    set "SDK=%ANDROID_SDK_ROOT%"
)
if not defined SDK (
    set "SDK=%LOCALAPPDATA%\Android\Sdk"
)

set "NDK=%ANDROID_NDK_HOME%"
if not defined NDK (
    set "NDK=%ANDROID_NDK_ROOT%"
)
if not defined NDK (
    for /f "delims=" %%d in ('dir /b /ad /on "%SDK%\ndk" 2^>nul') do (
        if exist "%SDK%\ndk\%%d\build\cmake\android.toolchain.cmake" (
            set "NDK=%SDK%\ndk\%%d"
        )
    )
)
if not defined NDK (
    echo No NDK in "%SDK%\ndk". Install one with the SDK Manager or set ANDROID_NDK_HOME.
    exit /b 1
)

rem The CMake of the SDK, because it comes with Ninja and this computer may have no other.
set "CMAKE_BIN="
for /f "delims=" %%d in ('dir /b /ad /on "%SDK%\cmake" 2^>nul') do (
    if exist "%SDK%\cmake\%%d\bin\ninja.exe" (
        set "CMAKE_BIN=%SDK%\cmake\%%d\bin"
    )
)
if not defined CMAKE_BIN (
    echo No CMake in "%SDK%\cmake". Install one with the SDK Manager.
    exit /b 1
)

adb get-state >nul
if errorlevel 1 (
    echo adb sees no phone. Connect it and enable USB debugging.
    exit /b 1
)

for /f "delims=" %%d in ('adb shell getprop ro.product.device') do (
    set "DEVICE=%%d"
)
rem The device name alone can be a code name, such as citrine for a Xiaomi.
for /f "delims=" %%m in ('adb shell "getprop ro.product.manufacturer | tr A-Z a-z"') do (
    set "MAKER=%%m"
)

set "BUILD=%ROOT%\build\android-%ABI%"

echo NDK:   %NDK%
echo CMake: %CMAKE_BIN%
echo Build: %BUILD%
echo Phone: %MAKER% %DEVICE%
echo.

"%CMAKE_BIN%\cmake.exe" -S "%ROOT%" -B "%BUILD%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%CMAKE_BIN:\=/%/ninja.exe" ^
    -DCMAKE_TOOLCHAIN_FILE="%NDK:\=/%/build/cmake/android.toolchain.cmake" ^
    -DANDROID_ABI=%ABI% ^
    -DANDROID_PLATFORM=android-21 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DFONTRENDERER_BUILD_BENCHMARKS=ON ^
    -DFONTRENDERER_BUILD_EXAMPLES=OFF ^
    -DFONTRENDERER_BUILD_TESTS=OFF
if errorlevel 1 (
    exit /b 1
)

"%CMAKE_BIN%\cmake.exe" --build "%BUILD%" --target benchmarkDraw
if errorlevel 1 (
    exit /b 1
)

adb shell mkdir -p %DEVICE_DIR%/resources
adb push "%BUILD%\benchmarkDraw" %DEVICE_DIR%/benchmarkDraw
adb push "%ROOT%\benchmarks\scripts\runOnDevice.sh" %DEVICE_DIR%/runOnDevice.sh
adb push "%ROOT%\bin\resources\." %DEVICE_DIR%/resources/
if errorlevel 1 (
    exit /b 1
)

rem The name comes from the core the benchmark will run on, so it is asked on that core. The phone is added because
rem two phones can have the same chip.
for /f "delims=" %%n in ('adb shell sh %DEVICE_DIR%/runOnDevice.sh %CORE% --name') do (
    set "NAME=%%n_%MAKER%-%DEVICE%"
)

echo.
echo Running the benchmark. It writes bin\benchmark_%NAME%.txt when it ends.
adb shell sh %DEVICE_DIR%/runOnDevice.sh %CORE% --csv benchmark.csv%ARGS% > "%ROOT%\bin\benchmark_%NAME%.txt"
if errorlevel 1 (
    type "%ROOT%\bin\benchmark_%NAME%.txt"
    exit /b 1
)

adb pull %DEVICE_DIR%/benchmark.csv "%ROOT%\bin\benchmark_%NAME%.csv"
python "%~dp0plot_benchmark.py" "%ROOT%\bin\benchmark_%NAME%.csv" -o "%ROOT%\bin\%NAME%.html"
