"""Builds the dashboard snapshot sent to the device.

Wire format (one JSON object per line, kept short for BLE):

    {"v": 1, "ts": <epoch>, "tz": <local UTC offset, s>, "acc": [
      {"n": "Personal", "p": "Max 5x", "k": "sub" | "api", "ok": 1, "e": "<error>",
       "s": [pct, reset],               session (5 h) limit       — subscriptions only
       "w": [pct, reset],               weekly limit               — subscriptions only
       "x": [[label, pct, reset], ...], per-model weekly limits    — subscriptions only
       "td": tokens_today, "tk": [input, output, cache_read, cache_write] (7 days),
       "c": cost_7d, "ct": cost_today,  USD (estimate at API prices for subscriptions)
       "d": [7 daily token totals, oldest first, last = today],
       "m": [[family, pct_of_tokens], ...]}
    ]}
"""

from __future__ import annotations

import json
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict

from .config import CONFIG_DIR, AccountConfig, Config
from .sources import admin_api, subscription
from .sources.local_logs import LocalLogs, TokenStats

PROTOCOL_VERSION = 1
# Plan limits move slowly and the endpoint is rate limited: poll it less often than
# the local token stats, and keep showing the last good answer through transient errors.
LIMITS_EVERY = 300
LIMITS_MIN_GAP = 60  # even for a refresh requested from the device
TRANSIENT = {"Rate limited", "Offline"}
LIMITS_CACHE = CONFIG_DIR / "limits_cache.json"
MAX_CACHE_AGE = 6 * 3600


def _load_cache() -> dict[str, tuple[float, subscription.SubscriptionUsage]]:
    try:
        raw = json.loads(LIMITS_CACHE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    out = {}
    for key, (ts, d) in raw.items():
        if time.time() - ts > MAX_CACHE_AGE:
            continue
        u = subscription.SubscriptionUsage(**{k: v for k, v in d.items()
                                              if k not in ("session", "weekly", "scoped")})
        u.session = subscription.LimitInfo(**d["session"]) if d.get("session") else None
        u.weekly = subscription.LimitInfo(**d["weekly"]) if d.get("weekly") else None
        u.scoped = [subscription.LimitInfo(**x) for x in d.get("scoped", [])]
        out[key] = (ts, u)
    return out


def _save_cache(cache: dict[str, tuple[float, subscription.SubscriptionUsage]]) -> None:
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        LIMITS_CACHE.write_text(json.dumps({k: [ts, asdict(u)] for k, (ts, u) in cache.items()}),
                                encoding="utf-8")
    except OSError:
        pass


def _tokens(entry: dict, t: TokenStats) -> None:
    entry["td"] = t.today
    entry["tk"] = [t.week_in, t.week_out, t.week_cache_read, t.week_cache_write]
    entry["c"] = round(t.cost_week, 2)
    entry["ct"] = round(t.cost_today, 2)
    entry["d"] = t.daily
    total = sum(t.models.values())
    if total:
        top = sorted(t.models.items(), key=lambda kv: -kv[1])[:3]
        entry["m"] = [[name, round(v * 100 / total)] for name, v in top]


class Collector:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self._logs: dict[str, LocalLogs] = {}
        self._limits = _load_cache()  # last good answer per account
        self._next_try: dict[str, float] = {}
        self.force = False  # set for a device-requested refresh

    def _limits_for(self, a: AccountConfig) -> subscription.SubscriptionUsage:
        key = str(a.claude_dir)
        now = time.time()
        last = self._limits.get(key)
        due = now >= self._next_try.get(key, 0)
        if last and not due and not (self.force and now - last[0] >= LIMITS_MIN_GAP):
            return last[1]
        u = subscription.fetch(a.claude_dir)
        if u.error in TRANSIENT:
            self._next_try[key] = now + max(u.retry_after, LIMITS_EVERY)
            if last:
                return last[1]
            return u
        self._next_try[key] = now + LIMITS_EVERY
        if not u.error:
            self._limits[key] = (now, u)
            _save_cache(self._limits)
        return u

    def _subscription(self, a: AccountConfig) -> dict:
        u = self._limits_for(a)
        # Transient errors keep the account usable (tokens still show); limits say why they're missing.
        hard = bool(u.error) and u.error not in TRANSIENT
        e: dict = {"n": a.name, "p": u.plan or "Claude", "k": "sub", "ok": 0 if hard else 1}
        if u.error:
            e["e"] = u.error
        if u.session:
            e["s"] = [round(u.session.pct, 1), u.session.resets_at]
        if u.weekly:
            e["w"] = [round(u.weekly.pct, 1), u.weekly.resets_at]
        if u.scoped:
            e["x"] = [[s.label, round(s.pct, 1), s.resets_at] for s in u.scoped[:2]]
        logs = self._logs.setdefault(str(a.claude_dir), LocalLogs(a.claude_dir))
        _tokens(e, logs.collect())
        return e

    @staticmethod
    def _api(a: AccountConfig) -> dict:
        e: dict = {"n": a.name, "p": "API", "k": "api", "ok": 1}
        key = a.resolve_admin_key()
        if not key:
            e.update(ok=0, e="No admin key")
            return e
        stats, err = admin_api.fetch(key)
        if err:
            e.update(ok=0, e=err)
        if stats:
            _tokens(e, stats)
        return e

    def collect(self) -> dict:
        def one(a: AccountConfig) -> dict:
            try:
                return self._api(a) if a.type == "api" else self._subscription(a)
            except Exception as ex:  # one broken account must not blank the dashboard
                return {"n": a.name, "p": "", "k": a.type[:3], "ok": 0, "e": type(ex).__name__}

        with ThreadPoolExecutor(max_workers=4) as pool:
            accounts = list(pool.map(one, self.cfg.accounts))
        self.force = False
        return {"v": PROTOCOL_VERSION, "ts": int(time.time()), "tz": time.localtime().tm_gmtoff,
                "acc": accounts}
