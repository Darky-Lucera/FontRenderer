@echo off
rem Runs bin\benchmarkDraw.exe and saves its results in bin, named after the system, the compiler and the processor.
rem
rem Usage: benchmark.bat [benchmarkDraw arguments...]
rem Build benchmarkDraw first, in Release, with -DFONTRENDERER_BUILD_BENCHMARKS=ON. The script already adds --csv.

setlocal

set "SCRIPTS=%~dp0"
cd /d "%~dp0..\..\bin"
if not exist benchmarkDraw.exe (
    echo There is no bin\benchmarkDraw.exe. Build it first, with -DFONTRENDERER_BUILD_BENCHMARKS=ON.
    exit /b 1
)

for /f "delims=" %%n in ('.\benchmarkDraw.exe --name') do (
    set "NAME=%%n"
)
if not defined NAME (
    echo bin\benchmarkDraw.exe gave no name. It may be older than this script: build it again.
    exit /b 1
)

echo Running the benchmark. It writes bin\benchmark_%NAME%.txt when it ends.
.\benchmarkDraw.exe --csv benchmark_%NAME%.csv %* > benchmark_%NAME%.txt
if errorlevel 1 (
    type benchmark_%NAME%.txt
    exit /b 1
)

python "%SCRIPTS%plot_benchmark.py" benchmark_%NAME%.csv -o %NAME%.html
