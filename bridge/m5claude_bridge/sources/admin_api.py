"""Usage and spend for an API organization via the Usage and Cost Admin API.

Needs an Admin API key (sk-ant-admin01-...), created in the Claude Console under
Settings → Admin keys. Data lags requests by about 5 minutes; the API supports
polling once per minute.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

from .. import pricing
from ..webapi import HttpError, get_json
from .local_logs import WINDOW_DAYS, TokenStats

BASE = "https://api.anthropic.com/v1/organizations"


def _get_all(path: str, key: str, params: list[tuple[str, str]]) -> list[dict]:
    headers = {"x-api-key": key, "anthropic-version": "2023-06-01"}
    buckets: list[dict] = []
    page = None
    while True:
        p = params + ([("page", page)] if page else [])
        data = get_json(f"{BASE}/{path}", headers, p)
        buckets += data.get("data", [])
        if not data.get("has_more") or not data.get("next_page"):
            return buckets
        page = data["next_page"]


def fetch(admin_key: str) -> tuple[TokenStats | None, str]:
    """Returns (stats, error). Daily buckets are UTC days."""
    now = datetime.now(timezone.utc)
    start = (now - timedelta(days=WINDOW_DAYS - 1)).replace(hour=0, minute=0, second=0, microsecond=0)
    base = [("starting_at", start.strftime("%Y-%m-%dT%H:%M:%SZ")), ("bucket_width", "1d"),
            ("limit", str(WINDOW_DAYS + 1))]
    try:
        usage = _get_all("usage_report/messages", admin_key, base + [("group_by[]", "model")])
        cost = _get_all("cost_report", admin_key, base)
    except HttpError as e:
        return None, {401: "Invalid admin key", 403: "Key lacks access"}.get(e.status, f"HTTP {e.status}")
    except OSError:
        return None, "Offline"

    stats = TokenStats()
    first = start.date()
    today = now.date()
    for b in usage:
        day = datetime.fromisoformat(b["starting_at"].replace("Z", "+00:00")).date()
        idx = (day - first).days
        if not 0 <= idx < WINDOW_DAYS:
            continue
        for r in b.get("results", []):
            cc = r.get("cache_creation") or {}
            cw = int(cc.get("ephemeral_1h_input_tokens") or 0) + int(cc.get("ephemeral_5m_input_tokens") or 0)
            inp, out = int(r.get("uncached_input_tokens") or 0), int(r.get("output_tokens") or 0)
            cr = int(r.get("cache_read_input_tokens") or 0)
            total = inp + out + cr + cw
            stats.daily[idx] += total
            stats.week_in += inp
            stats.week_out += out
            stats.week_cache_read += cr
            stats.week_cache_write += cw
            fam = pricing.family(r.get("model") or "")
            stats.models[fam] = stats.models.get(fam, 0) + total
            if day == today:
                stats.today += total
    for b in cost:
        day = datetime.fromisoformat(b["starting_at"].replace("Z", "+00:00")).date()
        if not 0 <= (day - first).days < WINDOW_DAYS:
            continue
        cents = sum(float(r.get("amount") or 0) for r in b.get("results", []))
        stats.cost_week += cents / 100
        if day == today:
            stats.cost_today += cents / 100
    return stats, ""
