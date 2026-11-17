# PS2 Audio Bridge — GitHub Actions prototype

Experimental PlayStation 2 Slim -> Ethernet -> Windows -> JBL Flip 5 audio transport.

## Current capability

- **Windows UDP receiver is verified** with local simulated PCM audio (tone heard on JBL).
- **PS2 sender (`ps2/`) is UNCOMPILED and UNTESTED on PS2**. It transmits a synthetic tone, **not game audio**.
- No SPU2 capture, no runtime OPL modifications, and no guarantee of game compatibility.

## Network (user's working OPL network)

- PS2: `192.168.137.2`
- Windows SMB host: `192.168.137.1` (Ethernet 2)
- SMB: `1024` TCP (handled by OPL Server)
- Audio: UDP `19752` -> Windows, separate from SMB

`ps2/config.h` already matches this configuration. Do not change OPL's network settings for this experiment.

## GitHub Actions build

1. Create a GitHub repository, e.g. `ps2-audio-bridge` (private or public).
2. Upload **the contents of this folder** to the repository root; `.github/workflows/build-ps2.yml` is essential.
3. Open **Actions** -> **Build PS2 ELF** -> choose the run for your push (or **Run workflow**).
4. If green, download the `ps2-audio-bridge-elf` artifact and extract `ps2_audio_bridge_test.elf`.
5. If red, copy the full error log. Build is **not yet confirmed**; API/toolchain adjustments may be necessary.

Workflow pulls official prebuilt PS2SDK container from GHCR. It does not install WSL on Windows.

## Windows receiver

```powershell
cd windows
python3 -m pip install -r requirements.txt
python3 receiver.py --device JBL
```

Keep receiver open when eventually running the ELF. The original loopback tone test can be repeated using:

```powershell
python3 simulator.py --host 127.0.0.1 --port 19752 --seconds 20
```

Windows Firewall may need to allow incoming UDP port 19752 on the appropriate network profile. Avoid disabling the firewall entirely.

## PS2 test (only after ELF compiles)

Use a homebrew launcher to run the ELF as a separate application (e.g. uLaunchELF). **Do not overwrite OPL or Free McBoot.** The PS2 prototype must initialize its own network and cannot run concurrently with a game. The next milestone is verifying UDP packet reception from actual PS2 hardware.

## Notes

- `ps2/main.c` uses PS2SDK functions that have not yet been verified by a real build; expect possible compiler errors and resolve from GitHub Actions logs.
- The current 5 ms scheduling is approximate and can jitter.
- The SPU2 audio interception concept remains research, not an implemented feature.
- GitHub Actions official image reference: https://github.com/ps2dev/ps2sdk/pkgs/container/ps2sdk
