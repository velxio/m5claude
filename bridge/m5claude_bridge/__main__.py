"""Command line entry point.

    python -m m5claude_bridge              run: collect and push to every device found
    python -m m5claude_bridge snapshot     print one snapshot and exit
    python -m m5claude_bridge accounts     list configured accounts
    python -m m5claude_bridge init         write ~/.m5claude/config.json from what's detected
    python -m m5claude_bridge add-api NAME (--key-env VAR | --key sk-ant-admin01-...)
    python -m m5claude_bridge autostart on|off|status
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import logging.handlers
import sys
import time

from . import __version__, autostart, config
from .hub import Hub
from .snapshot import Collector
from .transports import ble, tcp, usb_serial

log = logging.getLogger("m5claude")


def _summary(snap: dict) -> str:
    lines = []
    for a in snap["acc"]:
        head = f"  {a['n']:<16} {a.get('p', ''):<8}"
        if not a.get("ok"):
            lines.append(f"{head} ! {a.get('e', 'error')}")
            continue
        parts = []
        if "s" in a:
            parts.append(f"session {a['s'][0]:.0f}%")
        if "w" in a:
            parts.append(f"week {a['w'][0]:.0f}%")
        for label, pct, _ in a.get("x", []):
            parts.append(f"{label} {pct:.0f}%")
        if "tk" in a:
            parts.append(f"7d {sum(a['tk']) / 1e6:.1f}M tok, ${a.get('c', 0):.2f}")
        lines.append(f"{head} " + " · ".join(parts))
    return "\n".join(lines)


async def _collect_loop(hub: Hub, collector: Collector, every: int) -> None:
    while True:
        t0 = time.monotonic()
        snap = await asyncio.to_thread(collector.collect)
        hub.publish(snap)
        log.info("snapshot in %.1fs, %d device(s)\n%s", time.monotonic() - t0, len(hub.devices),
                 _summary(snap))
        hub.refresh_now.clear()
        try:
            await asyncio.wait_for(hub.refresh_now.wait(), timeout=every)
            collector.force = True
        except asyncio.TimeoutError:
            pass


async def _run(args: argparse.Namespace) -> None:
    cfg = config.load()
    if not cfg.accounts:
        log.error("No accounts found. Log in with Claude Code or run `add-api`.")
        return
    hub = Hub()
    collector = Collector(cfg)
    tasks = [_collect_loop(hub, collector, args.every or cfg.refresh_seconds)]
    if not args.no_tcp:
        tasks.append(tcp.run(hub, port=args.tcp_port))
    if not args.no_usb:
        tasks.append(usb_serial.run(hub))
    if not args.no_ble:
        tasks.append(ble.run(hub))
    await asyncio.gather(*tasks)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="m5claude-bridge", description="Claude usage bridge for M5Stack")
    ap.add_argument("--version", action="version", version=__version__)
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    run = sub.add_parser("run", help="collect and push to devices (default)")
    for p in (ap, run):
        p.add_argument("--every", type=int, default=0, help="refresh period in seconds")
        p.add_argument("--no-usb", action="store_true", help="don't look for devices on USB")
        p.add_argument("--no-ble", action="store_true", help="don't look for devices over Bluetooth")
        p.add_argument("--no-tcp", action="store_true", help="don't dial the local TCP link (development)")
        p.add_argument("--tcp-port", type=int, default=47800)
        p.add_argument("--log-file", help="write the log to this file (used by autostart)")
    sub.add_parser("snapshot", help="print one snapshot as JSON")
    sub.add_parser("accounts", help="list configured accounts")
    sub.add_parser("init", help="write a config file from detected accounts")
    add = sub.add_parser("add-api", help="add an API organization (Admin API key)")
    add.add_argument("name")
    g = add.add_mutually_exclusive_group(required=True)
    g.add_argument("--key-env", help="environment variable holding the admin key")
    g.add_argument("--key", help="admin key (stored in plain text in the config file)")
    auto = sub.add_parser("autostart", help="start the bridge automatically when you log in")
    auto.add_argument("action", choices=["on", "off", "status"])
    args = ap.parse_args(argv)

    level = logging.DEBUG if args.verbose else logging.INFO
    if getattr(args, "log_file", None):
        config.CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(args.log_file, maxBytes=1_000_000,
                                                       backupCount=1, encoding="utf-8")
        logging.basicConfig(level=level, handlers=[handler],
                            format="%(asctime)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    else:
        logging.basicConfig(level=level, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    if args.cmd == "autostart":
        if args.action == "status":
            print(autostart.status())
        else:
            print(autostart.set_enabled(args.action == "on"))
        return 0

    if args.cmd == "snapshot":
        snap = Collector(config.load()).collect()
        print(json.dumps(snap, indent=1))
        print(_summary(snap), file=sys.stderr)
        return 0
    if args.cmd == "accounts":
        cfg = config.load()
        src = config.CONFIG_FILE if cfg.from_file else "auto-detected (no config file)"
        print(f"Accounts — {src}")
        for a in cfg.accounts:
            where = a.claude_dir if a.type == "subscription" else (
                f"key from ${a.admin_key_env}" if a.admin_key_env else "key in config")
            print(f"  [{a.type}] {a.name}: {where}")
        return 0
    if args.cmd in ("init", "add-api"):
        cfg = config.load()
        if args.cmd == "add-api":
            cfg.accounts = [a for a in cfg.accounts if a.name != args.name]
            cfg.accounts.append(config.AccountConfig("api", args.name, admin_key=args.key,
                                                     admin_key_env=args.key_env))
        print(f"wrote {config.save(cfg)}")
        return 0
    lock = autostart.acquire_instance_lock()  # two bridges would fight over USB and BLE
    if lock is None:
        log.error("The bridge is already running (see `m5claude-bridge autostart status`).")
        return 1
    try:
        asyncio.run(_run(args))
    except KeyboardInterrupt:
        pass
    finally:
        lock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
