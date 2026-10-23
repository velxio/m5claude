"""Bridge configuration.

Lives in ~/.m5claude/config.json. It is optional: with no file, every Claude Code
config directory found in the home folder (~/.claude, ~/.claude-*) is used as a
subscription account. API organizations must be added explicitly (they need an
Admin API key).

    {
      "refresh_seconds": 60,
      "accounts": [
        {"type": "subscription", "name": "Personal", "config_dir": "~/.claude"},
        {"type": "subscription", "name": "Work", "config_dir": "~/.claude-work"},
        {"type": "api", "name": "Acme API", "admin_key_env": "ACME_ADMIN_KEY"}
      ]
    }
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path

CONFIG_DIR = Path(os.environ.get("M5CLAUDE_HOME", Path.home() / ".m5claude"))
CONFIG_FILE = CONFIG_DIR / "config.json"


@dataclass
class AccountConfig:
    type: str  # "subscription" | "api"
    name: str
    config_dir: str | None = None     # subscription: Claude Code config dir
    admin_key: str | None = None      # api: Admin API key (sk-ant-admin01-...)
    admin_key_env: str | None = None  # api: or the env var holding it

    @property
    def claude_dir(self) -> Path:
        return Path(os.path.expanduser(self.config_dir or "~/.claude"))

    def resolve_admin_key(self) -> str | None:
        if self.admin_key_env:
            return os.environ.get(self.admin_key_env)
        return self.admin_key


@dataclass
class Config:
    refresh_seconds: int = 60
    accounts: list[AccountConfig] = field(default_factory=list)
    from_file: bool = False


def discover_claude_dirs() -> list[Path]:
    """Claude Code config dirs in the home folder that hold a login."""
    home = Path.home()
    found = []
    for p in sorted(home.glob(".claude*")):
        if p.is_dir() and (p / ".credentials.json").exists():
            found.append(p)
    env = os.environ.get("CLAUDE_CONFIG_DIR")
    if env and Path(env).is_dir() and Path(env) not in found:
        found.append(Path(env))
    if not found and (home / ".claude").is_dir():
        found.append(home / ".claude")  # macOS keeps the token in the Keychain
    return found


def _default_name(claude_dir: Path) -> str:
    suffix = claude_dir.name.removeprefix(".claude").lstrip("-_.")
    return suffix.capitalize() if suffix else "Personal"


def load() -> Config:
    if CONFIG_FILE.exists():
        raw = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        accounts = [AccountConfig(**a) for a in raw.get("accounts", [])]
        return Config(refresh_seconds=int(raw.get("refresh_seconds", 60)), accounts=accounts,
                      from_file=True)
    accounts = [AccountConfig("subscription", _default_name(d), str(d)) for d in discover_claude_dirs()]
    return Config(accounts=accounts)


def save(cfg: Config) -> Path:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    data = {
        "refresh_seconds": cfg.refresh_seconds,
        "accounts": [{k: v for k, v in a.__dict__.items() if v is not None} for a in cfg.accounts],
    }
    CONFIG_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return CONFIG_FILE
