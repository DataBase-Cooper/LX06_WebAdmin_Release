@echo off
setlocal EnableExtensions
cd /d "%~dp0"
python scripts\build_release.py
exit /b %errorlevel%
