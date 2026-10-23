"""Fan-out of snapshots to every connected device, plus refresh requests coming back."""

from __future__ import annotations

import asyncio
import json
import logging

log = logging.getLogger("m5claude")


class Hub:
    def __init__(self) -> None:
        self.latest: bytes | None = None
        self._subs: set[asyncio.Queue[bytes]] = set()
        self.refresh_now = asyncio.Event()
        self.devices: dict[str, str] = {}  # link name -> device id

    def publish(self, snapshot: dict) -> None:
        self.latest = (json.dumps(snapshot, separators=(",", ":")) + "\n").encode()
        for q in self._subs:
            if q.full():
                q.get_nowait()  # a slow link only needs the newest snapshot
            q.put_nowait(self.latest)

    def subscribe(self) -> asyncio.Queue[bytes]:
        q: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
        if self.latest:
            q.put_nowait(self.latest)
        self._subs.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue[bytes]) -> None:
        self._subs.discard(q)

    def on_device_line(self, link: str, line: bytes) -> None:
        """Messages from a device: hello and refresh requests."""
        try:
            msg = json.loads(line)
        except ValueError:
            return
        if "hello" in msg:
            self.devices[link] = str(msg.get("id", "?"))
            log.info("%s: device %s connected (fw %s)", link, msg.get("id"), msg.get("fw"))
        if msg.get("req") == "refresh":
            log.info("%s: refresh requested from device", link)
            self.refresh_now.set()
