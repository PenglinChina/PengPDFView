@echo off
rem ============================================================================
rem  build.bat - builds PengPDFView with the Microsoft C++ toolchain.
rem
rem    build.bat          build the application  -> bin\PengPDFView.exe
rem    build.bat test     build the parser tests -> bin\pdf_tests.exe
rem    build.bat clean    remove build artefacts
rem
rem  The Visual Studio installation is located through vswhere.exe, so the
rem  script works with any recent Visual Studio / Build Tools edition.
rem ============================================================================
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=build"

rem ---------------------------------------------------------------- toolchain
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

rem Fallback to a well known install location when vswhere is unavailable.
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSPATH if exist "D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=D:\Microsoft Visual Studio\18\Community"

if not defined VSPATH (
    echo [error] Visual Studio C++ build tools were not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [error] Could not initialise the Visual Studio environment.
    exit /b 1
)

cd /d "%ROOT%"
set "OBJ=%ROOT%\obj"
set "BIN=%ROOT%\bin"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"

set "CXXFLAGS=/nologo /std:c++20 /EHsc /MT /O2 /W4 /MP /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\src" /I "%ROOT%\res""
set "BASE_LIBS=user32.lib gdi32.lib msimg32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib comctl32.lib windowscodecs.lib windowsapp.lib"

if /i "%MODE%"=="clean" goto :clean
if /i "%MODE%"=="test" goto :test
if /i "%MODE%"=="build" goto :build

echo [error] Unknown mode "%MODE%" ^(use build, test or clean^).
exit /b 1

rem -------------------------------------------------------------------- build
:build
echo [1/3] Compiling resources...
rc /nologo /I "%ROOT%\res" /fo "%OBJ%\PengPDFView.res" "%ROOT%\res\PengPDFView.rc"
if errorlevel 1 goto :failed

echo [2/3] Compiling sources...
cl %CXXFLAGS% src\main.cpp src\Core\*.cpp src\Pdf\*.cpp src\Render\*.cpp src\Text\*.cpp src\App\*.cpp src\Ui\*.cpp "%OBJ%\PengPDFView.res" /Fo"%OBJ%\\" /Fe"%BIN%\PengPDFView.exe" /link /SUBSYSTEM:WINDOWS %BASE_LIBS%
if errorlevel 1 goto :failed

echo [3/3] Done.
echo.
echo   Executable: %BIN%\PengPDFView.exe
exit /b 0

rem --------------------------------------------------------------------- test
:test
echo [1/2] Compiling parser tests...
cl %CXXFLAGS% /I "%ROOT%\tests" tests\pdf_tests.cpp src\Core\*.cpp src\Pdf\*.cpp src\Text\*.cpp /Fo"%OBJ%\\" /Fe"%BIN%\pdf_tests.exe" /link /SUBSYSTEM:CONSOLE user32.lib ole32.lib uuid.lib shell32.lib
if errorlevel 1 goto :failed

echo [2/2] Done.
echo.
echo   Test executable: %BIN%\pdf_tests.exe
exit /b 0

rem -------------------------------------------------------------------- clean
:clean
echo Removing build artefacts...
if exist "%OBJ%" rmdir /s /q "%OBJ%"
if exist "%BIN%" rmdir /s /q "%BIN%"
echo Done.
exit /b 0

:failed
echo.
echo [error] Build failed.
exit /b 1
