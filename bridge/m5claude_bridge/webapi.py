"""Tiny JSON-over-HTTPS helper on top of urllib (keeps the bridge dependency-free)."""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from . import __version__

USER_AGENT = f"m5claude-bridge/{__version__} (https://github.com/m5claude)"


class HttpError(Exception):
    def __init__(self, status: int, body: str, retry_after: float = 0):
        super().__init__(f"HTTP {status}: {body[:200]}")
        self.status = status
        self.body = body
        self.retry_after = retry_after


def get_json(url: str, headers: dict[str, str], params: list[tuple[str, str]] | None = None,
             timeout: float = 20) -> dict:
    if params:
        url = f"{url}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **headers})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            retry = float(e.headers.get("retry-after") or 0)
        except ValueError:
            retry = 0
        raise HttpError(e.code, e.read().decode("utf-8", "replace"), retry) from None
