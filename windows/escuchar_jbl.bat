@echo off
cd /d "%~dp0"

python3 receiver.py --host 0.0.0.0 --port 19752 --device JBL --buffer-ms 120 --max-buffer-ms 240

pause