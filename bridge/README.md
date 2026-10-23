# m5claude-bridge

Runs on your PC, collects Claude usage for every account you use, and pushes it to the
M5Stack dashboard over USB or Bluetooth LE. Nothing about the PC or your network is
stored on the device, so you can switch computers or Wi-Fi freely.

```powershell
pip install -e bridge          # once (Python 3.10+); installs pyserial and bleak
m5claude-bridge autostart on   # start now and at every login, no window
```

`m5claude-bridge` alone runs it in the foreground with the log in the terminal. Only one
bridge runs at a time; a second one exits with a message.

## Autostart

```
m5claude-bridge autostart on       register for login and start it now
m5claude-bridge autostart off      unregister and stop it
m5claude-bridge autostart status   registered? running? last log lines
```

- **Windows**: a `m5claude-bridge` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
  that starts `pythonw` (no console window).
- **macOS**: `~/Library/LaunchAgents/dev.velxio.m5claude-bridge.plist`, kept alive by launchd.
- **Linux**: a systemd user unit, `~/.config/systemd/user/m5claude-bridge.service`.

The log goes to `~/.m5claude/bridge.log` (rotated at 1 MB).

## Accounts

| Kind | What you see | Where it comes from |
|---|---|---|
| Pro / Max | session (5 h) and weekly limits, per-model weekly limits, tokens per day, API-price value | Claude Code's login and its local transcripts |
| API organization | spend (today / 7 days), tokens per day, token types, models | Usage and Cost Admin API |

**Pro / Max** accounts are found automatically: every Claude Code config directory in
your home folder (`~/.claude`, `~/.claude-*`) with a login is one account. To add a second
subscription, log it in under its own config dir:

```powershell
$env:CLAUDE_CONFIG_DIR = "$HOME\.claude-work"; claude    # then /login
```

Plan limits are polled every 5 minutes (the endpoint is rate limited); the last good
answer is cached in `~/.m5claude/limits_cache.json` and shown through transient errors.
The bridge only reads the login; it never refreshes it. If an account shows
"Login expired", use Claude Code with that account once and it recovers on the next refresh.

**API organizations** need an Admin API key (`sk-ant-admin01-…`, Claude Console →
Settings → Admin keys). Prefer keeping it in an environment variable:

```powershell
setx ACME_ADMIN_KEY "sk-ant-admin01-..."
m5claude-bridge add-api "Acme API" --key-env ACME_ADMIN_KEY
```

## Commands

```
m5claude-bridge              collect every 60 s and push to all connected devices
m5claude-bridge snapshot     print one snapshot (JSON) plus a summary
m5claude-bridge accounts     list accounts and where their data comes from
m5claude-bridge init         write ~/.m5claude/config.json from detected accounts (to rename/reorder)
m5claude-bridge add-api …    add an API organization
m5claude-bridge autostart …  on | off | status
  --every N    refresh period   --no-usb    skip USB   --no-ble    skip Bluetooth
  --no-tcp     skip the local development TCP link (127.0.0.1:47800)
```

## Links

- **TCP** (development): the bridge keeps dialing `127.0.0.1:47800`; anything listening there
  that speaks the protocol is treated like a device on a trusted link.
- **USB**: any port whose USB-UART chip is an M5Stack's (CP2104 / CH9102) is probed; it is
  kept only if the firmware greets the bridge, so other boards and flashing tools are left alone.
- **Bluetooth LE**: devices are found by service UUID (no Windows pairing). The first time
  a computer connects, the M5Stack asks on screen: **C = Allow**, **A = Deny**. Approved PCs
  (up to 4, by the id in `~/.m5claude/pc_id`) are remembered; hold **A** while powering the
  device on to forget them all. A device already connected over USB is skipped on Bluetooth.

Protocol and snapshot format: `m5claude_bridge/snapshot.py` and `src/app/app.h`.
