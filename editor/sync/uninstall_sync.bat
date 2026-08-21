@echo off
:: Removes the MicroSlate Sync startup shortcut.

set "SHORTCUT=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\MicroSlate Sync.lnk"

if exist "%SHORTCUT%" (
    del "%SHORTCUT%"
    echo MicroSlate Sync startup shortcut removed.
) else (
    echo No startup shortcut found — nothing to remove.
)
echo.
pause
