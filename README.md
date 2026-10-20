<div align="center">

<img src="docs/logo.jpg" width="300" alt="BossQuickDock">

# BossQuickDock

**Bluetooth headphones and speakers on a jailbroken PS5 — game and system audio, no dongle.**

Developed by **RamalhoBoss** · based on **[HearBridge PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5) by X-F1REBALL-X**

</div>

<p align="center"><img src="docs/screenshot.jpg" alt="BossQuickDock control page"></p>

## Credits

All credit for the core of this project goes to **X-F1REBALL-X** and his work on **HearBridge PS5** — the Bluetooth stack, A2DP/SBC streaming, the control page and everything that makes PS5 audio reach your headphones is his. Thank you for releasing the source code. 🙏

BossQuickDock is a modified build of HearBridge PS5, released under the same license (GPL-3.0-or-later). See [NOTICE](NOTICE).

## What's different from HearBridge

**Bug fixes**

- **DualSense stuck on a solid white light** after it disconnected while the app had the Bluetooth chip open — it now reconnects without rebooting the console. The USB transport no longer cancels pending transfers (which hung the system's Bluetooth stack until a reboot): extra reads are completed with harmless HCI / L2CAP echo traffic instead.
- **System replies taken by the app** are given back: if the app reads an HCI Command Complete/Status meant for the console, it steps aside and replays a rejected copy of that command so the system gets its answer.
- **Audio stutter while streaming** — all reads stay armed during playback, so credits are never starved.
- **Incoming headset / speaker connections timing out** — a device that connects to the console is now recognised as the app's own link.
- **Log file growing forever** (134 MB+) — capped, and turned off in the release build.

**New**

- Brand-new control page with a PlayStation-style look, live waveform, tabs for Home / Devices / Games / System
- New name, icon and home-screen tile

## Install

1. Jailbreak your PS5 and run an ELF loader (tested on firmware 13.60).
2. Send `BossQuickDock-1.3.6.elf` to the console (for example, copy it to `/data/pldmgr/payloads/local` and launch it from your payload manager).
3. Open the **BossQuickDock** tile on the home screen, or browse to `http://<console-ip>:8090` from a phone or PC.
4. Put your headphones in pairing mode and press **Scan for devices**.

## Build

Requires the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).

```bash
python3 scripts/build_ui.py
python3 scripts/gen_webpage.py src/web/index.html src/webpage.h src/web/i18n.json
make ps5 PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

The ELF is written to `dist/BossQuickDock-<version>.elf`. Build with `-DBQD_LOG_TO_FILE=1` to get a debug log in `/data/hearbridge/hearbridge.log`.

## License

GPL-3.0-or-later, like the original HearBridge PS5. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
