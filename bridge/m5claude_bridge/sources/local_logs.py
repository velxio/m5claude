"""Token usage from Claude Code's local transcripts (<config_dir>/projects/**/*.jsonl).

Transcripts can be hundreds of MB, so each file is parsed once and re-parsed only
when its size or mtime changes. Only files touched in the last 8 days are read.
A resumed session copies earlier messages into a new file, so records are
de-duplicated by message id across files.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta
from pathlib import Path

from .. import pricing

WINDOW_DAYS = 7


@dataclass(frozen=True)
class Rec:
    msg_id: str
    day: date      # local day
    model: str
    inp: int
    out: int
    cache_read: int
    cache_5m: int
    cache_1h: int


@dataclass
class TokenStats:
    today: int = 0
    week_in: int = 0
    week_out: int = 0
    week_cache_read: int = 0
    week_cache_write: int = 0
    cost_week: float = 0.0
    cost_today: float = 0.0
    daily: list[int] = field(default_factory=lambda: [0] * WINDOW_DAYS)  # [-1] = today
    models: dict[str, int] = field(default_factory=dict)                # family -> tokens

    @property
    def week_total(self) -> int:
        return self.week_in + self.week_out + self.week_cache_read + self.week_cache_write


def _parse_file(path: Path) -> list[Rec]:
    recs: dict[str, Rec] = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"assistant"' not in line or '"usage"' not in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            msg = d.get("message") or {}
            u = msg.get("usage")
            if d.get("type") != "assistant" or not u or not msg.get("id"):
                continue
            model = msg.get("model") or ""
            if model.startswith("<"):  # "<synthetic>" messages carry no real usage
                continue
            ts = d.get("timestamp")
            if not ts:
                continue
            day = datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone().date()
            cc = u.get("cache_creation") or {}
            c1h = int(cc.get("ephemeral_1h_input_tokens") or 0)
            c5m = int(cc.get("ephemeral_5m_input_tokens") or 0)
            if not cc:
                c5m = int(u.get("cache_creation_input_tokens") or 0)
            # Streaming writes the same message several times; the last write is final.
            recs[msg["id"]] = Rec(msg["id"], day, model, int(u.get("input_tokens") or 0),
                                  int(u.get("output_tokens") or 0),
                                  int(u.get("cache_read_input_tokens") or 0), c5m, c1h)
    return list(recs.values())


class LocalLogs:
    def __init__(self, claude_dir: Path):
        self.root = claude_dir / "projects"
        self._cache: dict[Path, tuple[int, float, list[Rec]]] = {}

    def collect(self) -> TokenStats:
        today = date.today()
        first = today - timedelta(days=WINDOW_DAYS - 1)
        cutoff = time.time() - (WINDOW_DAYS + 1) * 86400
        seen: set[Path] = set()
        if self.root.is_dir():
            for p in self.root.rglob("*.jsonl"):
                try:
                    st = p.stat()
                except OSError:
                    continue
                if st.st_mtime < cutoff:
                    continue
                seen.add(p)
                cached = self._cache.get(p)
                if not cached or cached[0] != st.st_size or cached[1] != st.st_mtime:
                    try:
                        self._cache[p] = (st.st_size, st.st_mtime, _parse_file(p))
                    except OSError:
                        continue
        for p in list(self._cache):
            if p not in seen:
                del self._cache[p]

        stats = TokenStats()
        counted: set[str] = set()
        for _, _, recs in self._cache.values():
            for r in recs:
                if r.day < first or r.day > today or r.msg_id in counted:
                    continue
                counted.add(r.msg_id)
                total = r.inp + r.out + r.cache_read + r.cache_5m + r.cache_1h
                cost = pricing.cost_usd(r.model, r.inp, r.out, r.cache_read, r.cache_5m, r.cache_1h)
                stats.daily[(r.day - first).days] += total
                stats.week_in += r.inp
                stats.week_out += r.out
                stats.week_cache_read += r.cache_read
                stats.week_cache_write += r.cache_5m + r.cache_1h
                stats.cost_week += cost
                fam = pricing.family(r.model)
                stats.models[fam] = stats.models.get(fam, 0) + total
                if r.day == today:
                    stats.today += total
                    stats.cost_today += cost
        return stats
