# Packet Pong

Two-player LAN Pong. Each player runs one copy. The host plays left; the joining player plays right.

## Build

- Linux: install SDL2 and OpenGL development files (`libsdl2-dev` and `libgl-dev` on Debian/Ubuntu), then run `make`. If SDL2 is already installed at runtime, you can instead extract the `libsdl2-dev` package into `vendor/linux`. Run `make smoke` for a headless OpenGL startup test with SDL's offscreen driver and Mesa.
- Windows from Linux: install `x86_64-w64-mingw32-gcc`, extract the SDL2 MinGW development archive into `vendor/`, then run `make windows`. Keep `build/windows/SDL2.dll` beside `build/windows/PacketPong.exe`.

## Play

Run `./PacketPong` on Linux or open `build/windows/PacketPong.exe` on Windows. Press **H** on the host; press **J** on the other machine, enter the host's IP, and press **Enter**. Or launch with `--host` or `--join IP`.

On a direct cable, give the Ethernet ports addresses such as `192.168.50.1/24` and `192.168.50.2/24` if they do not already share a subnet. Join the host at `192.168.50.1`. For IPv6 link-local, append the **joining** machine's Ethernet interface name or index: `fe80::1234%enp3s0` on Linux or `fe80::1234%12` on Windows (use the interface index). Allow inbound UDP port `42874` on the host.

Move with **W/S** or **Up/Down**. **F1** CRT, **F2** chromatic split, **F3** bloom and ghosting, **F4** glitch. Press the active effect key again to turn it off. **Esc** leaves the match.
