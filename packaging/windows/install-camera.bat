@echo off
rem Installs the "rng-lens" virtual camera (DirectShow) for 64-bit and 32-bit apps. Needs admin once.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "DEST=%ProgramFiles%\rng-lens"
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\x86" mkdir "%DEST%\x86"
copy /y "%~dp0rnglens_vcam.dll" "%DEST%\rnglens_vcam.dll" >nul
copy /y "%~dp0x86\rnglens_vcam.dll" "%DEST%\x86\rnglens_vcam.dll" >nul
"%SystemRoot%\System32\regsvr32.exe" /s "%DEST%\rnglens_vcam.dll"
if exist "%SystemRoot%\SysWOW64\regsvr32.exe" "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%DEST%\x86\rnglens_vcam.dll"
echo.
echo rng-lens camera installed. Restart Zoom / Discord / OBS / your browser and pick "rng-lens".
echo.
pause
