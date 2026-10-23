"""Plan limits for a Claude Pro/Max account, read with Claude Code's own login.

Uses the same endpoint Claude Code's /usage screen calls. The access token is read
from the account's Claude Code config dir on every refresh (Claude Code rotates it),
and is never refreshed here: rotating the refresh token would log Claude Code out.
If the token has expired, the account reports "Login expired" until Claude Code is
used again with that account.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from ..webapi import HttpError, get_json

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"


@dataclass
class LimitInfo:
    label: str
    pct: float
    resets_at: int  # epoch seconds, 0 if unknown


@dataclass
class SubscriptionUsage:
    plan: str = ""
    email: str = ""
    session: LimitInfo | None = None
    weekly: LimitInfo | None = None
    scoped: list[LimitInfo] = field(default_factory=list)  # per-model weekly limits
    error: str = ""
    retry_after: float = 0  # seconds, when rate limited


def _epoch(ts: str | None) -> int:
    if not ts:
        return 0
    return int(datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp())


def _read_credentials(claude_dir: Path) -> dict | None:
    f = claude_dir / ".credentials.json"
    if f.exists():
        return json.loads(f.read_text(encoding="utf-8")).get("claudeAiOauth")
    if sys.platform == "darwin":  # Claude Code stores the login in the Keychain on macOS
        try:
            out = subprocess.run(
                ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                capture_output=True, text=True, timeout=5, check=True).stdout
            return json.loads(out).get("claudeAiOauth")
        except (subprocess.SubprocessError, json.JSONDecodeError):
            return None
    return None


def _plan_label(creds: dict) -> str:
    sub = (creds.get("subscriptionType") or "").lower()
    tier = (creds.get("rateLimitTier") or "").lower()
    if sub == "max":
        for mult in ("20x", "5x"):
            if mult in tier:
                return f"Max {mult}"
        return "Max"
    return sub.capitalize() if sub else "Claude"


def _email(claude_dir: Path) -> str:
    # ~/.claude keeps its state in ~/.claude.json; custom config dirs keep it inside.
    for p in (claude_dir / ".claude.json", claude_dir.parent / f"{claude_dir.name}.json"):
        try:
            return json.loads(p.read_text(encoding="utf-8"))["oauthAccount"]["emailAddress"]
        except (OSError, KeyError, ValueError, TypeError):
            continue
    return ""


def fetch(claude_dir: Path) -> SubscriptionUsage:
    creds = _read_credentials(claude_dir)
    if not creds or not creds.get("accessToken"):
        return SubscriptionUsage(error="Not logged in")
    out = SubscriptionUsage(plan=_plan_label(creds), email=_email(claude_dir))

    if creds.get("expiresAt", 0) / 1000 < time.time():
        out.error = "Login expired"
        return out
    try:
        data = get_json(USAGE_URL, {
            "Authorization": f"Bearer {creds['accessToken']}",
            "anthropic-beta": "oauth-2025-04-20",
        })
    except HttpError as e:
        if e.status in (401, 403):
            out.error = "Login expired"
        elif e.status == 429:
            out.error = "Rate limited"
            out.retry_after = e.retry_after
        else:
            out.error = f"HTTP {e.status}"
        return out
    except OSError:
        out.error = "Offline"
        return out

    # Prefer the generic `limits` list; fall back to the older fixed fields.
    for lim in data.get("limits") or []:
        info = LimitInfo("", float(lim.get("percent") or 0), _epoch(lim.get("resets_at")))
        kind = lim.get("kind")
        if kind == "session":
            info.label = "Session"
            out.session = info
        elif kind == "weekly_all":
            info.label = "Weekly"
            out.weekly = info
        elif kind == "weekly_scoped":
            model = ((lim.get("scope") or {}).get("model") or {}).get("display_name")
            info.label = model or "Scoped"
            out.scoped.append(info)
    if out.session is None and data.get("five_hour"):
        fh = data["five_hour"]
        out.session = LimitInfo("Session", float(fh.get("utilization") or 0), _epoch(fh.get("resets_at")))
    if out.weekly is None and data.get("seven_day"):
        sd = data["seven_day"]
        out.weekly = LimitInfo("Weekly", float(sd.get("utilization") or 0), _epoch(sd.get("resets_at")))
    if not out.scoped:
        for key, label in (("seven_day_opus", "Opus"), ("seven_day_sonnet", "Sonnet")):
            if data.get(key):
                d = data[key]
                out.scoped.append(LimitInfo(label, float(d.get("utilization") or 0), _epoch(d.get("resets_at"))))
    return out
