@echo off
SET CHROME_PATH=D:\git\chromium\src\out\win-relese\chrome.exe

del "%LOCALAPPDATA%\Chromium\User Data\chrome_debug.log" 2>nul

echo Starting Chrome with WebSocket logging...
echo.

start "" "%CHROME_PATH%" --enable-logging --v=1 --vmodule=*websocket*=2 --log-level=0 --json-websocket-port=7747

timeout /t 2 /nobreak >nul

echo ===== WebSocket Logs =====
echo.
powershell -Command "Get-Content '%LOCALAPPDATA%\Chromium\User Data\chrome_debug.log' -Wait -Tail 100 | Select-String 'websocket|WebSocket'"
