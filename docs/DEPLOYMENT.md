# Running a server for others to join (LAN or internet)

This covers getting `AndreasOnlineServer` reachable from outside your own PC.
None of this was runtime-tested by the AI that wrote it - it could only
verify the *code* (see `docs/RGL_COMPAT_NOTES.md` for that caveat in
general). Please follow the steps and report back what actually happens.

## 1. Same Wi-Fi / LAN (easiest - try this first)

1. Run the server: `AndreasOnlineServer.exe` (or `server.cfg` - see below).
2. On the server PC, find its LAN IP:
   ```
   ipconfig
   ```
   Look for "IPv4 Address" under your active adapter (usually
   `192.168.x.x` or `10.x.x.x`).
3. On every other PC on the same network, in `AndreasOnline.cfg` (next to
   `AndreasOnline.asi`):
   ```ini
   server_ip=192.168.x.x
   server_port=7777
   player_name=YourName
   ```
4. Windows Firewall will prompt the first time `AndreasOnlineServer.exe`
   listens - click **Allow** (for both Private and Public networks if asked).
   If it doesn't prompt (or you already dismissed it), add a rule manually:
   ```powershell
   New-NetFirewallRule -DisplayName "AndreasOnline Server" -Direction Inbound -Protocol UDP -LocalPort 7777 -Action Allow
   ```

If this doesn't work, the two PCs may be on a network that isolates clients
from each other (common on guest Wi-Fi) - try a different network, or skip
to port forwarding below and test across two different networks (e.g. one
PC on Wi-Fi, one on mobile data/hotspot).

## 2. Over the internet (port forwarding)

1. Find your public IP from the server PC (open
   <https://whatismyipaddress.com> in a browser, or search "what is my ip").
2. Log into your router's admin page (commonly `192.168.1.1` or
   `192.168.0.1` - check the sticker on the router, or run `ipconfig` and
   use the "Default Gateway" address) and find **Port Forwarding** /
   **Virtual Server** / **NAT**. Every router's UI is different; the setting
   you want is usually called one of those three names.
3. Forward **UDP port 7777** (or whatever you set in `server.cfg`) to the
   server PC's **LAN IP** (the `192.168.x.x` address from step 1 above,
   not the public one - routers usually want both the external and internal
   port, set both to 7777).
4. Other players set, in their `AndreasOnline.cfg`:
   ```ini
   server_ip=<your public IP>
   server_port=7777
   ```
5. **NAT loopback caveat**: some routers won't let a PC on the *same* LAN as
   the server reach it via the *public* IP (only a genuinely remote PC can).
   If you want to test from the same house, use the LAN IP (section 1)
   there and only give the public IP to people outside your network.

Your public IP usually changes every so often (unless your ISP gives you a
static one) - if players suddenly can't connect after a while, re-check it.

## 3. Keeping settings without retyping command-line args

Create `server.cfg` next to `AndreasOnlineServer.exe`:
```ini
# lines starting with # or ; are ignored
port=7777
max_players=32
```
Command-line args still override this file if given:
`AndreasOnlineServer.exe 7778 16` uses port 7778 / 16 players even with a
different `server.cfg` present.

## 4. Keeping the server running

- `AndreasOnlineServer.exe` is a plain console app - closing the window (or
  Ctrl+C) now shuts it down cleanly (disconnects everyone first) rather than
  just vanishing.
- To keep it running unattended on a PC that's otherwise idle: Task
  Scheduler, with a trigger of "At startup" or "At log on", running
  `AndreasOnlineServer.exe` with "Start in" set to its own folder (so it
  finds `server.cfg` next to it).
- There's no graceful "restart on crash" built in yet - if that becomes a
  real need, a small wrapper script restarting the process on exit, or the
  equivalent in Task Scheduler ("restart the task if it fails"), is the
  straightforward option.
