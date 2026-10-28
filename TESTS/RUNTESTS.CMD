@echo off
rem ======================================================================
rem  RUNTESTS.CMD - build and run the SNBETNWS unit tests (no BBS needed)
rem
rem  Output: BUILD\TESTS\BNTEST.EXE.  Exit code 0 = all tests passed.
rem ======================================================================
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\BUILD\TESTS

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio was not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /W4 /J /MT /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\SRC" /I"%ROOT%\SRC\SHARED" ^
   /Fo"%OUT%\\" /Fe"%OUT%\BNTEST.EXE" ^
   "%ROOT%\TESTS\BNTEST.C" "%ROOT%\SRC\BNNNTP.C" "%ROOT%\SRC\BNART.C" ^
   "%ROOT%\SRC\BNSPOOL.C" "%ROOT%\SRC\BNWORK.C" ^
   "%ROOT%\SRC\SHARED\SBNET.C" "%ROOT%\SRC\SHARED\SBUTIL.C" ^
   "%ROOT%\SRC\SHARED\SBMAIL.C" "%ROOT%\SRC\SHARED\IEMSG.C" ^
   "%ROOT%\SRC\SHARED\IETEXT.C" ^
   ws2_32.lib secur32.lib bcrypt.lib || exit /b 1

"%OUT%\BNTEST.EXE"
