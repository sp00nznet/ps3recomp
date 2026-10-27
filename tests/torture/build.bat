@echo off
REM Build the lifter torture-test harness with clang-cl inside the MSVC environment.
REM Usage: build.bat [configure|build|all]   (default: all)

set "LLVM=C:\Program Files\LLVM\bin"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" || exit /b 1
set "PATH=%LLVM%;%PATH%"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

cd /d "%~dp0"

if not "%ACTION%"=="build" (
    cmake -B build -G Ninja ^
        -DCMAKE_C_COMPILER=clang-cl ^
        -DCMAKE_CXX_COMPILER=clang-cl ^
        -DCMAKE_BUILD_TYPE=Release || exit /b 1
)

if not "%ACTION%"=="configure" (
    cmake --build build || exit /b 1
)
