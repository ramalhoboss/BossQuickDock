@echo off
rem BossQuickDock Receiver: starts the app without a console window.
setlocal
set "APP=%~dp0app.py"
rem 1) A Python bundled beside the app, 2) the Codex runtime on this PC, 3) Python 3.12 from python.org.
for %%P in ("%~dp0python\pythonw.exe" "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe") do (
 if exist %%P (
  start "" %%P "%APP%"
  exit /b
 )
)
where pyw >nul 2>nul && (
 start "" pyw -3.12 "%APP%"
 exit /b
)
echo BossQuickDock Receiver needs Python 3.12 (64-bit).
echo Install it from https://www.python.org/downloads/ and open this file again.
pause
