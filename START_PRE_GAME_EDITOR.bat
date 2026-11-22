@echo off
where py >nul 2>nul
if errorlevel 1 (
    python "%~dp0scripts\pregame_editor.py"
) else (
    py -3 "%~dp0scripts\pregame_editor.py"
)
if errorlevel 1 pause
