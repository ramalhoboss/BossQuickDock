BossQuickDock Receiver 1.0
==========================

Hear your PS5 on this PC. Same look as the BossQuickDock control page.

Start
-----
Unzip the folder anywhere and double-click "BossQuickDock Receiver.cmd".
A window opens; the instructions are on its Home screen and in Help.

1. On the PS5, open BossQuickDock 1.6.1 (or 1.5.1 or newer).
2. Connect this PC to the same network as the PS5.
3. Press "Find PS5" (or type the PS5 IP and press Save).
4. Press "Listen on PC".
5. To go back to your headphones, press "Back to Bluetooth".

The first time, if Windows asks about the firewall, choose Allow.
Closing the window stops the sound on the PC and on the PS5.

Needs
-----
- Windows 10/11 (64-bit) with Microsoft Edge, which draws the window.
- The download already includes Python (python\) and its packages (deps\):
  nothing to install.
- BossQuickDock 1.6.1 on the PS5 (or 1.5.1-stable9 or newer). It sends the
  console audio as Opus over UDP port 18098. Versions 1.1.x do not.

From source (no python\ folder): Python 3.12 64-bit and
"pip install --target deps -r requirements.txt".

Files
-----
app.py              the app (local page on 127.0.0.1:18099, audio, PS5 control)
receiver_core.py    Opus/UDP receiver and jitter buffer
console_control.py  BossQuickDock console API client
web\                the page and its images, font (Inter, OFL) and icon
python\, deps\      Python 3.12 and PyAV / sounddevice (in the download only)

Settings are kept in %LOCALAPPDATA%\BossQuickDock Receiver.

GPL-3.0-or-later. Developed by RamalhoBoss, based on HearBridge PS5 by
X-F1REBALL-X. See LICENSE and NOTICE.
