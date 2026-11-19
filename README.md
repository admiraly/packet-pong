# Packet Pong

Two-player LAN Pong. Each player runs one copy. The game picks which player is on the left.

## Build

- Linux: install SDL2 and OpenGL development files (`libsdl2-dev` and `libgl-dev` on Debian/Ubuntu), then run `make`. If SDL2 is already installed at runtime, you can instead extract the `libsdl2-dev` package into `vendor/linux`. Run `make smoke` for a headless OpenGL startup test with SDL's offscreen driver and Mesa.
- Windows from Linux: install `x86_64-w64-mingw32-gcc`, extract the SDL2 MinGW development archive into `vendor/`, then run `make windows`. Keep `build/windows/SDL2.dll` beside `build/windows/PacketPong.exe`.

## Play

Connect the computers with an Ethernet cable or put them on the same LAN. Run `./PacketPong` on Linux or open `build/windows/PacketPong.exe` on Windows. Both copies show **Searching for player** until they connect. If the connection drops, the game searches again and starts a new match when it reconnects.

Leave IPv6 enabled for a direct cable without DHCP. The game uses IPv6 link-local multicast or IPv4 broadcast for discovery. If prompted by a firewall, allow Packet Pong on both computers. Discovery uses UDP port `42874`; gameplay uses dynamically assigned UDP ports.

Move with **W/S** or **Up/Down**. **F1** CRT, **F2** chromatic split, **F3** bloom and ghosting, **F4** glitch. Press the active effect key again to turn it off. **Esc** quits.

Press **T** to open chat, **Enter** to send, and **Esc** to close it. Use **PgUp/PgDn** or the mouse wheel to scroll through the last 256 messages. Chat accepts up to 120 printable ASCII characters per message. When chat is closed, up to five recent messages appear in the bottom-left and fade out after about eight seconds. Both players need a chat-capable build to exchange messages. The top-left counter shows smoothed FPS.

## Automatic direct-cable IPv4

Automatic IPv4 setup is on by default. Wi-Fi and Ethernet adapters that already have a usable IPv4 address are skipped. On a shared network, run `./PacketPong --no-auto-link` (Linux) or `PacketPong.exe --no-auto-link` (Windows) to use ordinary LAN discovery only: if no DHCP server answers on an eligible Ethernet adapter, automatic setup can start its own. `--auto-link` still explicitly enables setup.

Approve the one-time administrator prompt if asked. Linux needs `pkexec`, a graphical authorization agent, and `ip` from `iproute2`; Windows uses UAC. Only the link helper is relaunched with elevated privileges. If permission is declined, a DHCP port is busy, or setup fails, ordinary LAN discovery still works as before.

On an isolated cable without DHCP, one copy takes `10.77.0.1/24` and offers `10.77.0.2` to its peer for one hour (UDP ports `67` and `68`). The game shows the assigned address as `LINK: ...` and removes only the address it added when it exits. It does not install a default gateway or DNS settings. Restart the game if the lease expires; it does not renew leases. Run `make test` for the DHCP and chat packet tests.
