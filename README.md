# Calc2KeyCEWireless

Calc2KeyCEWireless is a multi-part project built around TI-84 Plus CE calculators.

The repo now contains the full stack:

- Windows bridge host for receiving frames/input over the network
- Raspberry Pi relay for USB communication with the calculator
- Linux desktop app for local calculator input and screen streaming
- Calculator-side CE programs

At a high level, the remote bridge flow is:

1. The Windows host listens for a relay connection.
2. The Raspberry Pi relay talks to the calculator over USB and forwards data to the Windows host.
3. The calculator runs the matching CE program.

### Make sure to backup ram before running
### Press [on] key to exit

## Repo Layout

- `Calc2KeyCE.BridgeHostWin/`: Windows bridge host
- `Calc2KeyCE.PiRelay/`: Raspberry Pi relay programs
- `Calc2KeyCE.Fast/`: Linux desktop application
- `Calc2KeyCE.Calc/`: main calculator program
- `_vendor/`: vendored third-party components used by the Windows display workflow

## Repo Notes

This repo is intended to be portable between machines. Machine-specific values such as Pi hostnames, SSH usernames, SSH keys, and remote relay paths should be supplied at runtime, not hardcoded into source.

For the Windows bridge host:

- The build uses standard CMake dependency discovery for Leptonica.
- Set `LEPTONICA_ROOT` to the dependency prefix, or add that prefix to `CMAKE_PREFIX_PATH`.
- If the runtime DLL is not on `PATH`, set `LEPTONICA_BIN` before launching `Launch-Calc2KeyBridge.ps1`.
- `CALC2KEY_PI_USER` can be used to prefill the default SSH username in the launcher and bridge host UI.

## Quickstart

### Fastest path for a new user

1. Get a Raspberry Pi on Wi-Fi and confirm SSH access.
2. Build the Windows host on a Windows machine.
3. Build or install the Pi relay on the Raspberry Pi.
4. Build the calculator program with CEdev.
5. Start the Windows host.
6. Start the Pi relay with the Windows host IP.
7. Transfer the calculator binary to the TI-84 Plus CE and run it.

### Minimum commands

Windows host:

```powershell
cmake -S Calc2KeyCE.BridgeHostWin -B Calc2KeyCE.BridgeHostWin/build -G "Visual Studio 17 2022" -A x64
cmake --build Calc2KeyCE.BridgeHostWin/build --config Release
powershell -ExecutionPolicy Bypass -File .\Launch-Calc2KeyBridge.ps1
```

Pi relay:

```bash
git clone https://github.com/Bobsfunstuff1/Calc2KeyCEWireless.git
cd Calc2KeyCEWireless
chmod +x tools/install-pi-relay.sh
./tools/install-pi-relay.sh <windows-host-ip> 28400
```

Calculator program:

```bash
cd Calc2KeyCE.Calc
make
```

Release packaging:

```powershell
.\tools\package-windows-host-release.ps1
.\tools\package-calculator-release.ps1
```

```bash
./tools/package-pi-relay-release.sh
```

## Build Guide

### 1. Windows Bridge Host

Source:

- `Calc2KeyCE.BridgeHostWin/`

Build requirements:

- CMake
- MSVC / Visual Studio C++ toolchain
- Leptonica headers and libraries
- DirectX 11-capable Windows build environment

Example build:

```powershell
cmake -S Calc2KeyCE.BridgeHostWin -B Calc2KeyCE.BridgeHostWin/build -G "Visual Studio 17 2022" -A x64
cmake --build Calc2KeyCE.BridgeHostWin/build --config Release
```

If Leptonica is not in a standard location, set `LEPTONICA_ROOT` first.

Launcher:

```powershell
powershell -ExecutionPolicy Bypass -File .\Launch-Calc2KeyBridge.ps1
```

You will need to enter your own Pi host, SSH user, SSH key path, and bridge host IP in the launcher UI.

### 2. Raspberry Pi Relay

Source:

- `Calc2KeyCE.PiRelay/`

First-time headless Pi setup:

If the Pi is not on the network yet, get Wi-Fi working before building the relay.

Option A: Raspberry Pi Imager

1. Flash Raspberry Pi OS with Raspberry Pi Imager.
2. In the Imager advanced options, set:
   - hostname
   - username/password
   - Wi-Fi SSID/password
   - locale and SSH enabled
3. Boot the Pi and connect over SSH.

Option B: manual first-boot Wi-Fi on the SD card

1. Flash Raspberry Pi OS to the SD card.
2. Mount the `boot` partition on your computer.
3. Create an empty file named `ssh` in the root of the `boot` partition to enable SSH on first boot.
4. Copy [pi-firstboot-wpa-supplicant.conf.example](/C:/Users/matth/source/repos/Calc2KeyCE%20-%20linux/tools/pi-firstboot-wpa-supplicant.conf.example) to `wpa_supplicant.conf` in the `boot` partition.
5. Replace the placeholder SSID/password and set the correct `country` code.
6. Insert the card into the Pi and boot it.

After first boot, find the Pi IP from your router, `ping <hostname>.local`, or use `ssh <user>@raspberrypi.local` if mDNS is available.

Build requirements on the Pi:

- CMake
- g++
- pkg-config
- `libusb-1.0`

Example dependency install:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libusb-1.0-0-dev
```

Example build:

```bash
cmake -S Calc2KeyCE.PiRelay -B Calc2KeyCE.PiRelay/build
cmake --build Calc2KeyCE.PiRelay/build -j
```

Example full Pi setup after SSH is working:

```bash
git clone https://github.com/Bobsfunstuff1/Calc2KeyCEWireless.git
cd Calc2KeyCEWireless
sudo apt update
sudo apt install -y build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S Calc2KeyCE.PiRelay -B Calc2KeyCE.PiRelay/build
cmake --build Calc2KeyCE.PiRelay/build -j
```

Quick install script:

```bash
chmod +x tools/install-pi-relay.sh
./tools/install-pi-relay.sh <windows-host-ip> 28400
```

Optional systemd service:

```bash
sudo cp tools/calc2key-pirelay.service.example /etc/systemd/system/calc2key-pirelay.service
sudo systemctl daemon-reload
sudo systemctl enable --now calc2key-pirelay
sudo systemctl status calc2key-pirelay
```

Expected outputs:

- `Calc2KeyPiRelay`
- `Calc2PiConsoleRelay`

Example run:

```bash
./Calc2KeyCE.PiRelay/build/Calc2KeyPiRelay --bridge <windows-host-ip>:28400
```

Console relay example:

```bash
./Calc2KeyCE.PiRelay/build/Calc2PiConsoleRelay --shell /bin/bash
```

### 3. Linux Desktop App

Source:

- `Calc2KeyCE.Fast/`

Build requirements on Linux:

- CMake
- g++
- pkg-config
- SDL2
- GLEW
- OpenGL
- libusb-1.0
- X11 / Xtst / Xext development packages
- Leptonica

Typical Ubuntu/Debian packages:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libsdl2-dev libglew-dev libusb-1.0-0-dev libleptonica-dev libx11-dev libxtst-dev libxext-dev libgl1-mesa-dev
```

Example build:

```bash
cmake -S Calc2KeyCE.Fast -B Calc2KeyCE.Fast/build
cmake --build Calc2KeyCE.Fast/build -j
```

### 4. Calculator Programs

Sources:

- `Calc2KeyCE.Calc/`

These use the CEdev toolchain.

Build requirements:

- CEdev installed and configured
- `cedev-config` available on `PATH`

Example builds:

```bash
cd Calc2KeyCE.Calc
make
```

Outputs are the calculator binaries generated by the CEdev makefiles.

## Suggested Setup Flow

For the full Windows host + Pi + calculator path:

1. Bring the Pi online over Wi-Fi and confirm SSH access.
2. Build the Windows bridge host.
3. Build `Calc2KeyPiRelay` on the Raspberry Pi.
4. Build the calculator program in `Calc2KeyCE.Calc/`.
5. Launch the Windows host.
6. Run the Pi relay with the Windows host address.
7. Transfer and run the calculator program on the TI-84 Plus CE.

## Release Checklist

If you want this repo to be easier for external users, publish these as GitHub Releases:

- Windows host executable build output
- Pi relay binary for Raspberry Pi
- calculator binary from `Calc2KeyCE.Calc/`
- a short release note saying which files go on Windows, Pi, and calculator

Recommended release artifact layout:

- `windows/Calc2KeyCEBridgeHostWin.exe`
- `windows/Launch-Calc2KeyBridge.ps1`
- `pi/Calc2KeyPiRelay`
- `pi/Calc2PiConsoleRelay`
- `calculator/Calc2Key.8xp`

This repo also includes:

- a GitHub Actions workflow at `.github/workflows/release-artifacts.yml`
- local packaging scripts under `tools/`
- calculator packaging scripts for both Windows and Linux CI via `tools/package-calculator-release.ps1` and `tools/package-calculator-release.sh`

Third Party Libraries:
+ zx0: https://github.com/einar-saukas/ZX0
+ leptonica: https://github.com/DanBloomberg/leptonica
+ libwdi: https://github.com/pbatard/libwdi
