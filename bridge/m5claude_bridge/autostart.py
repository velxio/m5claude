"""Start the bridge automatically when you log in, with no window.

    m5claude-bridge autostart on       register and start it now
    m5claude-bridge autostart off      unregister and stop it
    m5claude-bridge autostart status   is it registered, is it running, last log lines

Windows: a value under HKCU\\...\\CurrentVersion\\Run that launches pythonw (no console).
macOS: a LaunchAgent. Linux: a systemd user service.
Output goes to ~/.m5claude/bridge.log.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
from pathlib import Path

from .config import CONFIG_DIR

NAME = "m5claude-bridge"
LOG_FILE = CONFIG_DIR / "bridge.log"
LOCK_PORT = 47801  # held by the running bridge: one instance per user


def acquire_instance_lock() -> socket.socket | None:
    """A listening socket on localhost works as a cross-platform, crash-safe lock."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform == "win32":
        s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    try:
        s.bind(("127.0.0.1", LOCK_PORT))
        s.listen(1)
        return s
    except OSError:
        s.close()
        return None


def is_running() -> bool:
    s = acquire_instance_lock()
    if s:
        s.close()
        return False
    return True


def _python(windowless: bool) -> str:
    exe = Path(sys.executable)
    if windowless and sys.platform == "win32":
        w = exe.with_name("pythonw.exe")
        if w.exists():
            return str(w)
    return str(exe)


def _args(windowless: bool) -> list[str]:
    return [_python(windowless), "-m", "m5claude_bridge", "--log-file", str(LOG_FILE)]


def _start_detached() -> None:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    flags = 0
    if sys.platform == "win32":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
    subprocess.Popen(_args(True), cwd=str(Path.home()), creationflags=flags,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=sys.platform != "win32")


def _stop_running() -> None:
    if sys.platform == "win32":
        # Only our own processes: pythonw/python running m5claude_bridge.
        ps = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*-m m5claude_bridge*' "
              "-and $_.ProcessId -ne " + str(os.getpid()) + " } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }")
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    else:
        subprocess.run(["pkill", "-f", "-m m5claude_bridge --log-file"], capture_output=True)


# ------------------------------------------------------------------ Windows

_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def _win_command() -> str:
    return subprocess.list2cmdline(_args(True))


def _win_set(enable: bool) -> None:
    import winreg

    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if enable:
            winreg.SetValueEx(k, NAME, 0, winreg.REG_SZ, _win_command())
        else:
            try:
                winreg.DeleteValue(k, NAME)
            except FileNotFoundError:
                pass


def _win_enabled() -> bool:
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as k:
            winreg.QueryValueEx(k, NAME)
            return True
    except FileNotFoundError:
        return False


# -------------------------------------------------------------------- macOS

_PLIST = Path.home() / "Library" / "LaunchAgents" / "dev.velxio.m5claude-bridge.plist"


def _mac_set(enable: bool) -> None:
    if enable:
        args = "".join(f"<string>{a}</string>" for a in _args(False))
        _PLIST.parent.mkdir(parents=True, exist_ok=True)
        _PLIST.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
            '<plist version="1.0"><dict>'
            "<key>Label</key><string>dev.velxio.m5claude-bridge</string>"
            f"<key>ProgramArguments</key><array>{args}</array>"
            "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>"
            "</dict></plist>\n", encoding="utf-8")
        subprocess.run(["launchctl", "load", "-w", str(_PLIST)], capture_output=True)
    elif _PLIST.exists():
        subprocess.run(["launchctl", "unload", "-w", str(_PLIST)], capture_output=True)
        _PLIST.unlink()


# -------------------------------------------------------------------- Linux

_UNIT = Path.home() / ".config" / "systemd" / "user" / "m5claude-bridge.service"


def _linux_set(enable: bool) -> None:
    if enable:
        _UNIT.parent.mkdir(parents=True, exist_ok=True)
        _UNIT.write_text(
            "[Unit]\nDescription=m5claude bridge (Claude usage for the M5Stack dashboard)\n\n"
            f"[Service]\nExecStart={subprocess.list2cmdline(_args(False))}\nRestart=on-failure\n\n"
            "[Install]\nWantedBy=default.target\n", encoding="utf-8")
        subprocess.run(["systemctl", "--user", "daemon-reload"], capture_output=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", "m5claude-bridge"], capture_output=True)
    elif _UNIT.exists():
        subprocess.run(["systemctl", "--user", "disable", "--now", "m5claude-bridge"], capture_output=True)
        _UNIT.unlink()


# ---------------------------------------------------------------- commands

def enabled() -> bool:
    if sys.platform == "win32":
        return _win_enabled()
    if sys.platform == "darwin":
        return _PLIST.exists()
    return _UNIT.exists()


def set_enabled(enable: bool) -> str:
    if sys.platform == "win32":
        _win_set(enable)
        if enable and not is_running():
            _start_detached()
        if not enable:
            _stop_running()
    elif sys.platform == "darwin":
        _mac_set(enable)  # launchd starts/stops it
    else:
        _linux_set(enable)  # systemd starts/stops it
    if enable:
        return f"Autostart on: the bridge starts when you log in. Log: {LOG_FILE}"
    return "Autostart off, bridge stopped."


def status(tail: int = 8) -> str:
    lines = [f"Autostart: {'on' if enabled() else 'off'}",
             f"Running:   {'yes' if is_running() else 'no'}",
             f"Log:       {LOG_FILE}"]
    try:
        log = LOG_FILE.read_text(encoding="utf-8", errors="replace").splitlines()[-tail:]
        lines += ["", *log]
    except OSError:
        pass
    return "\n".join(lines)
