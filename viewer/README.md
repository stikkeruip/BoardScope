# BoardScope viewer

Shows the iPhone's camera on the PC over the USB cable, with zoom and focus that stay in step
with the phone. Whichever side moves a control last wins. One file, nothing to install beside it.

## Using it

1. Plug the iPhone into the PC with a cable, unlock it, and tap **Trust** if it asks.
2. Open BoardScope on the iPhone.
3. Start `BoardScope.exe`. It finds the phone on its own and reconnects if the cable or app drops.

Windows needs Apple's USB service, which comes with **Apple Devices** (Microsoft Store) or iTunes.
The `.exe` isn't signed, so SmartScreen asks once: **More info → Run anyway**.

| Keys | Does |
| --- | --- |
| `+` / `-`, mouse wheel | zoom |
| `1` to `8` | zoom 1x to 8x |
| `Up` / `Down` | focus, fine step |
| `Shift` + `Up` / `Down` | focus, coarse step |
| `R` / `Shift` + `R` | rotate the picture right / left (remembered) |
| `Tab` | hide or show the controls |
| `F11` | full screen (`Esc` leaves it) |

The status line shows the connection, frames per second, round trip to the phone, and the phone's heat and battery.

## Building

Needs CMake 3.24+ and a C++20 compiler. Dependencies download at configure time, pinned by hash:
SDL 3.4.18, Dear ImGui 1.92.9b and libjpeg-turbo 3.2.0, all linked statically.
[NASM](https://nasm.us) gives libjpeg-turbo its fast x86 decoder; without it the build still works, decoding more slowly.

Windows `.exe`, cross-compiled on a Mac (`brew install mingw-w64 nasm`):

```bash
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-windows -j
```

The result is `build-windows/BoardScope.exe`.

Native build, for testing on the Mac with the phone plugged in:

```bash
cmake -B build-mac
cmake --build build-mac -j
./build-mac/BoardScope
```

## How it talks to the phone

The phone app listens on TCP port 7700 on its loopback interface only, so it can't be reached over Wi-Fi.
The viewer reaches that port through Apple's USB multiplexer (`/var/run/usbmuxd` on macOS,
`127.0.0.1:27015` on Windows), which `src/usbmux.cpp` speaks directly.

Each message is a type byte, a 4-byte big-endian length, then the payload. `src/wire.h` and
`ios/BoardScope/Wire.swift` define the same messages: JPEG frames, frame acknowledgements, commands,
state, and ping/pong. The phone keeps at most two frames unacknowledged and skips camera frames
rather than queueing them, so a slow link drops frames instead of falling behind.
