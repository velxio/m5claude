"""USB link: finds an M5Stack on any serial port by its USB-UART chip and streams to it.

No port names to configure: plugging the cable in is enough. Opening the port
with DTR/RTS low avoids resetting the ESP32. A port is only kept open if the
firmware greets us within a few seconds, so other ESP32 boards (and flashing
tools) are left alone.
"""

from __future__ import annotations

import asyncio
import logging
import threading

from ..hub import Hub

log = logging.getLogger("m5claude")

# USB-UART bridges used on M5Stack Core boards (CP2104 on older units, CH9102 on v2.6+).
KNOWN_VID_PID = {(0x10C4, 0xEA60), (0x1A86, 0x55D4), (0x1A86, 0x7523)}
BAUD = 115200
HELLO_TIMEOUT = 6.0   # seconds to wait for the firmware greeting
RETRY_FOREIGN = 30.0  # back-off for ports that never greeted us


def _candidates() -> list[str]:
    from serial.tools import list_ports

    return [p.device for p in list_ports.comports() if (p.vid, p.pid) in KNOWN_VID_PID]


def _open(port: str):
    import serial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, BAUD, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


async def run(hub: Hub) -> None:
    try:
        import serial  # noqa: F401
    except ImportError:
        log.warning("usb: pyserial not installed, USB link disabled (pip install pyserial)")
        return
    busy: set[str] = set()
    while True:
        for port in _candidates():
            if port not in busy:
                busy.add(port)
                asyncio.create_task(_session(hub, port, busy))
        await asyncio.sleep(2)


async def _release_later(port: str, busy: set[str], delay: float) -> None:
    await asyncio.sleep(delay)
    busy.discard(port)


async def _session(hub: Hub, port: str, busy: set[str]) -> None:
    name = f"usb:{port}"
    loop = asyncio.get_running_loop()
    try:
        ser = await asyncio.to_thread(_open, port)
    except OSError as e:  # in use by a serial monitor / flasher
        log.debug("%s: %s", name, e)
        await asyncio.sleep(3)
        busy.discard(port)
        return
    log.info("%s: opened", name)
    stop = threading.Event()

    def reader() -> None:
        buf = b""
        while not stop.is_set():
            try:
                chunk = ser.read(256)
            except OSError:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.startswith(b"{"):
                    loop.call_soon_threadsafe(hub.on_device_line, name, line)
        stop.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    deadline = loop.time() + HELLO_TIMEOUT
    while name not in hub.devices and loop.time() < deadline and not stop.is_set():
        await asyncio.sleep(0.1)
    if name not in hub.devices:
        stop.set()
        ser.close()
        log.debug("%s: no m5claude firmware, ignoring for %ds", name, RETRY_FOREIGN)
        asyncio.create_task(_release_later(port, busy, RETRY_FOREIGN))
        return
    q = hub.subscribe()
    try:
        while not stop.is_set():
            try:
                data = await asyncio.wait_for(q.get(), timeout=1)
            except asyncio.TimeoutError:
                continue
            await asyncio.to_thread(ser.write, data)
    except OSError:
        pass
    finally:
        stop.set()
        hub.unsubscribe(q)
        hub.devices.pop(name, None)
        ser.close()
        busy.discard(port)
        log.info("%s: closed", name)
