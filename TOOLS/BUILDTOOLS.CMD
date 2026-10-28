@echo off
rem ======================================================================
rem  BUILDTOOLS.CMD - build the SNBETNWS command-line test tool
rem
rem  Output: BUILD\TOOLS\BNFETCH.EXE
rem  Needs Visual Studio 2022 (or Build Tools) with the C++ workload.
rem ======================================================================
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\BUILD\TOOLS

rem Locate Visual Studio and load the 32-bit (x86) compiler environment.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio was not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

if not exist "%OUT%" mkdir "%OUT%"

rem /J matches the module build (char is unsigned); /W4 for extra checking.
cl /nologo /W4 /J /O2 /MT /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\SRC" /I"%ROOT%\SRC\SHARED" ^
   /Fo"%OUT%\\" /Fe"%OUT%\BNFETCH.EXE" ^
   "%ROOT%\TOOLS\BNFETCH.C" "%ROOT%\SRC\BNNNTP.C" "%ROOT%\SRC\BNART.C" ^
   "%ROOT%\SRC\BNSPOOL.C" "%ROOT%\SRC\BNWORK.C" ^
   "%ROOT%\SRC\SHARED\SBNET.C" "%ROOT%\SRC\SHARED\SBUTIL.C" ^
   "%ROOT%\SRC\SHARED\SBMAIL.C" "%ROOT%\SRC\SHARED\IEMSG.C" ^
   "%ROOT%\SRC\SHARED\IETEXT.C" ^
   ws2_32.lib secur32.lib bcrypt.lib || exit /b 1

if exist "%ROOT%\TOOLS\BNFETCH.INI" copy /y "%ROOT%\TOOLS\BNFETCH.INI" "%OUT%\" >nul
echo Built %OUT%\BNFETCH.EXE
