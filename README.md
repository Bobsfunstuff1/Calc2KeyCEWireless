# Calc2KeyCE

This is a C# program that reads usb input from a TI-84 Plus CE calculator and allows the user to bind calculator keys to keyboard keys or mouse actions. It can also cast your screen to your calculator's screen.

### Make sure to backup ram before running
### Press [on] key to exit

## Repo Notes

This repo is intended to be portable between machines. Machine-specific values such as Pi hostnames, SSH usernames, SSH keys, and remote relay paths should be supplied at runtime, not hardcoded into source.

For the Windows bridge host:

- The build uses standard CMake dependency discovery for Leptonica.
- Set `LEPTONICA_ROOT` to the dependency prefix, or add that prefix to `CMAKE_PREFIX_PATH`.
- If the runtime DLL is not on `PATH`, set `LEPTONICA_BIN` before launching `Launch-Calc2KeyBridge.ps1`.
- `CALC2KEY_PI_USER` can be used to prefill the default SSH username in the launcher and bridge host UI.

Third Party Libraries:
+ zx0: https://github.com/einar-saukas/ZX0
+ leptonica: https://github.com/DanBloomberg/leptonica
+ libwdi: https://github.com/pbatard/libwdi
