"""Bluetooth LE link: finds M5Claude devices by their service UUID and streams to them.

No pairing dialog on the PC. The first time a computer connects, the device asks on
its screen whether to accept it; the approval is remembered on the device, keyed by
this PC's stable id (~/.m5claude/pc_id).
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket
import uuid

from ..config import CONFIG_DIR
from ..hub import Hub

log = logging.getLogger("m5claude")

# Keep in sync with firmware/m5claude/m5claude.ino.
SVC_UUID = "8f1a0001-5c6e-4b7a-9d2e-6d35c1a0e001"
RX_UUID = "8f1a0002-5c6e-4b7a-9d2e-6d35c1a0e001"  # PC -> device
TX_UUID = "8f1a0003-5c6e-4b7a-9d2e-6d35c1a0e001"  # device -> PC
SCAN_EVERY = 8.0
DENIED_BACKOFF = 300.0
PAIR_TIMEOUT = 120.0  # time to approve a new PC on the device
USB_BACKOFF = 60.0    # re-check later in case the cable is unplugged


def pc_identity() -> tuple[str, str]:
    f = CONFIG_DIR / "pc_id"
    try:
        pcid = f.read_text(encoding="utf-8").strip()
    except OSError:
        pcid = ""
    if not pcid:
        pcid = uuid.uuid4().hex[:16]
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        f.write_text(pcid, encoding="utf-8")
    return socket.gethostname()[:30], pcid


async def run(hub: Hub) -> None:
    try:
        from bleak import BleakScanner
    except ImportError:
        log.warning("ble: bleak not installed, Bluetooth disabled (pip install bleak)")
        return
    busy: set[str] = set()
    blocked: dict[str, float] = {}
    loop = asyncio.get_running_loop()
    while True:
        try:
            found = await BleakScanner.discover(timeout=4.0, service_uuids=[SVC_UUID])
        except Exception as e:  # adapter off / missing
            log.debug("ble: scan failed: %s", e)
            await asyncio.sleep(30)
            continue
        for dev in found:
            if dev.address in busy or blocked.get(dev.address, 0) > loop.time():
                continue
            busy.add(dev.address)
            asyncio.create_task(_session(hub, dev, busy, blocked))
        await asyncio.sleep(SCAN_EVERY)


async def _session(hub: Hub, dev, busy: set[str], blocked: dict[str, float]) -> None:
    from bleak import BleakClient

    name = f"ble:{dev.name or dev.address}"
    loop = asyncio.get_running_loop()
    buf = bytearray()
    paired = asyncio.Event()
    denied = asyncio.Event()

    def on_notify(_, data: bytearray) -> None:
        buf.extend(data)
        while b"\n" in buf:
            i = buf.index(b"\n")
            line = bytes(buf[:i])
            del buf[: i + 1]
            hub.on_device_line(name, line)
            try:
                pair = json.loads(line).get("pair")
            except ValueError:
                continue
            if pair == "pending":
                log.info("%s: approve this computer on the M5Stack screen (press C)", name)
            elif pair == "ok":
                paired.set()
            elif pair == "denied":
                log.warning("%s: this computer was denied on the device", name)
                denied.set()

    try:
        async with BleakClient(dev, timeout=15.0) as client:
            log.info("%s: connected", name)
            await client.start_notify(TX_UUID, on_notify)
            chunk = max(20, (client.mtu_size or 23) - 3)

            async def send(data: bytes) -> None:
                for off in range(0, len(data), chunk):
                    await client.write_gatt_char(RX_UUID, data[off:off + chunk], response=False)

            # Windows often doesn't report the advertised name, so identify the device by
            # its hello; if it's already served over USB, BLE would only duplicate it.
            for _ in range(30):
                if name in hub.devices:
                    break
                await asyncio.sleep(0.1)
            dev_id = hub.devices.get(name)
            if dev_id and any(k != name and v == dev_id for k, v in hub.devices.items()):
                log.info("%s: %s is connected over USB, skipping Bluetooth", name, dev_id)
                blocked[dev.address] = loop.time() + USB_BACKOFF
                return
            pc, pcid = pc_identity()
            await send((json.dumps({"pc": pc, "pcid": pcid}) + "\n").encode())
            # Wait for the device's verdict (instant for a known PC, or until someone
            # presses a button); it ignores snapshots until then.
            deadline = loop.time() + PAIR_TIMEOUT
            while not (paired.is_set() or denied.is_set()) and client.is_connected:
                if loop.time() > deadline:
                    return
                await asyncio.sleep(0.2)
            if not paired.is_set():
                return
            log.info("%s: paired", name)
            q = hub.subscribe()  # starts with the latest snapshot
            try:
                while client.is_connected:
                    try:
                        data = await asyncio.wait_for(q.get(), timeout=2)
                    except asyncio.TimeoutError:
                        continue
                    await send(data)
            finally:
                hub.unsubscribe(q)
    except Exception as e:
        log.debug("%s: %s", name, e)
    finally:
        hub.devices.pop(name, None)
        if denied.is_set():
            blocked[dev.address] = loop.time() + DENIED_BACKOFF
        busy.discard(dev.address)
        log.info("%s: disconnected", name)
