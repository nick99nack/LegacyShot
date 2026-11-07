@echo off
rem Builds LegacyShot.exe with the Visual C++ 6.0 command-line tools.
rem Run VC98\Bin\VCVARS32.BAT first (or use a "VC++ 6.0 command prompt").
rem Also works with later Visual Studio versions from a 32-bit (x86) prompt.

if not exist Release mkdir Release
rc /l 0x409 /fo Release\LegacyShot.res LegacyShot.rc
if errorlevel 1 goto failed
cl /nologo /MT /W3 /O2 /D WIN32 /D NDEBUG /D _WINDOWS /D _MBCS /FoRelease\ /FeRelease\LegacyShot.exe main.c capture.c imgenc.c lsup.c lsupcrypto.c Release\LegacyShot.res /link /subsystem:windows kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib comdlg32.lib wsock32.lib
if errorlevel 1 goto failed

rem Console test of the protocol and encoders (optional)
cl /nologo /MT /W3 /O2 /D WIN32 /FoRelease\ /FeRelease\test_lsup.exe test\test_lsup.c lsup.c lsupcrypto.c imgenc.c wsock32.lib
cl /nologo /MT /W3 /O2 /D WIN32 /FoRelease\ /FeRelease\test_img.exe test\test_img.c imgenc.c lsupcrypto.c

echo.
echo Built Release\LegacyShot.exe
goto end
:failed
echo.
echo BUILD FAILED
:end
