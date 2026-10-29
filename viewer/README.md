# BoardScope viewer

Shows the iPhone's camera on the PC over the USB cable or Wi-Fi, with zoom, focus and the light
in step with the phone. Whichever side moves a control last wins. One file, nothing to install beside it.

## Using it

1. Open BoardScope on the iPhone.
2. Plug the iPhone into the PC with a cable (unlock it and tap **Trust** if it asks), or have both on the same Wi-Fi network.
3. Start `BoardScope.exe`. It finds the phone on its own and reconnects if the cable or app drops.

On **Automatic** the viewer uses the cable whenever it's plugged in, falls back to Wi-Fi, and moves back
to the cable when it's plugged in again. **Connection** in the panel can also keep it to one or the other.

Over Wi-Fi the phone asks for the 6-digit pairing code shown in BoardScope's Settings on the iPhone;
type it once under **Connection** and it's remembered. **New pairing code** on the phone locks out
viewers using the old one. If the viewer can't find the phone on a network that blocks Bonjour, type the
phone address from the same Settings page. The picture isn't encrypted on Wi-Fi; the code only keeps
other devices on the network from connecting. Lowering the JPEG quality helps on a busy network.

One viewer at a time: if another viewer connects, this one shows **Take the iPhone back** rather than
fighting over it.

Windows needs Apple's USB service for the cable, which comes with **Apple Devices** (Microsoft Store)
or iTunes; Wi-Fi works without it. The `.exe` isn't signed, so SmartScreen asks once: **More info → Run anyway**.

| Keys | Does |
| --- | --- |
| `+` / `-`, mouse wheel | zoom |
| `1` to `8` | zoom 1x to 8x |
| `Up` / `Down` | focus, fine step |
| `Shift` + `Up` / `Down` | focus, coarse step |
| `L` | light on / off |
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

Over USB, the phone app listens on TCP port 7700 on its loopback interface only. The viewer reaches
that port through Apple's USB multiplexer (`/var/run/usbmuxd` on macOS, `127.0.0.1:27015` on Windows),
which `src/usbmux.cpp` speaks directly.

Over Wi-Fi, the phone listens on port 7701 and announces it with Bonjour as `_boardscope._tcp`;
`src/mdns.cpp` asks for it with a multicast DNS query, so the PC needs no Bonjour service of its own.
The viewer's first message is the pairing code, and the phone sends nothing until it's right.

Each message is a type byte, a 4-byte big-endian length, then the payload. `src/wire.h` and
`ios/BoardScope/Wire.swift` define the same messages: JPEG frames, frame acknowledgements, commands,
state, ping/pong, and over Wi-Fi the pairing hello and its refusal. The phone keeps at most two frames unacknowledged and skips camera frames
rather than queueing them, so a slow link drops frames instead of falling behind.
