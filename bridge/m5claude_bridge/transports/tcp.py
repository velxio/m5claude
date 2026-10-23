"""TCP link for development: anything listening on 127.0.0.1:47800 that speaks the
device protocol (see src/app/app.h) is treated like a device on a trusted link.

The bridge keeps dialing; when a listener is up it gets the latest snapshot
immediately and every new one after that.
"""

from __future__ import annotations

import asyncio
import logging

from ..hub import Hub

log = logging.getLogger("m5claude")


async def run(hub: Hub, host: str = "127.0.0.1", port: int = 47800, name: str = "tcp") -> None:
    while True:
        try:
            reader, writer = await asyncio.open_connection(host, port)
        except OSError:
            await asyncio.sleep(2)
            continue
        log.info("%s: connected to %s:%d", name, host, port)
        q = hub.subscribe()

        async def pump_in() -> None:
            while line := await reader.readline():
                hub.on_device_line(name, line)

        reader_task = asyncio.create_task(pump_in())
        try:
            while not reader_task.done():
                get = asyncio.create_task(q.get())
                done, _ = await asyncio.wait({get, reader_task}, return_when=asyncio.FIRST_COMPLETED)
                if get in done:
                    writer.write(get.result())
                    await writer.drain()
                else:
                    get.cancel()
        except (OSError, asyncio.IncompleteReadError):
            pass
        finally:
            reader_task.cancel()
            hub.unsubscribe(q)
            hub.devices.pop(name, None)
            writer.close()
            log.info("%s: disconnected", name)
        await asyncio.sleep(1)
