@echo off
rem Start the DSH <-> Dial bridge. Keep this window open while you want the
rem desk pet to mirror DSH's work status; close it to fall back to auto mode.
setlocal
cd /d "%~dp0"
node dsh-pet-dial-bridge.mjs %*
pause
