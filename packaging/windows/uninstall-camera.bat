@echo off
rem Removes the "rng-lens" virtual camera. Needs admin.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "DEST=%ProgramFiles%\rng-lens"
if exist "%DEST%\rnglens_vcam.dll" "%SystemRoot%\System32\regsvr32.exe" /u /s "%DEST%\rnglens_vcam.dll"
if exist "%DEST%\x86\rnglens_vcam.dll" "%SystemRoot%\SysWOW64\regsvr32.exe" /u /s "%DEST%\x86\rnglens_vcam.dll"
rmdir /s /q "%DEST%"
echo rng-lens camera removed.
pause
