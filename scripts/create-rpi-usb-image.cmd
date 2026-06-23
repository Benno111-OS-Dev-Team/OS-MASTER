@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%create-rpi-usb-image-windows.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" (
  echo.
  echo Raspberry Pi image launcher exited with code %EXIT_CODE%.
)
echo.
pause
exit /b %EXIT_CODE%
