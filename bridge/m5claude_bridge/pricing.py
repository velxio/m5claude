"""API list prices (USD per million tokens), used to estimate what local Claude Code
usage would cost at API rates. Subscription accounts don't pay these; the number is
shown as an "API value" estimate. Keep in sync with https://www.anthropic.com/pricing.
"""

from __future__ import annotations

# (prefix, input, output, cache_read). Cache writes: 1.25x input (5 min), 2x input (1 h).
# First matching prefix wins, so more specific prefixes go first.
_PRICES: list[tuple[str, float, float, float]] = [
    ("claude-fable-5-1", 10.0, 50.0, 0.25),
    ("claude-mythos", 10.0, 50.0, 1.0),
    ("claude-fable", 10.0, 50.0, 1.0),
    ("claude-opus-5-5", 4.0, 20.0, 0.20),
    ("claude-opus-5", 5.0, 25.0, 0.50),
    ("claude-opus-4-8", 5.0, 25.0, 0.50),
    ("claude-opus-4-7", 5.0, 25.0, 0.50),
    ("claude-opus-4-6", 5.0, 25.0, 0.50),
    ("claude-opus-4-5", 5.0, 25.0, 0.50),
    ("claude-opus", 15.0, 75.0, 1.50),
    ("claude-sonnet-5-5", 2.0, 10.0, 0.20),
    ("claude-sonnet-5", 2.0, 10.0, 0.20),
    ("claude-sonnet", 3.0, 15.0, 0.30),
    ("claude-haiku-4", 1.0, 5.0, 0.10),
    ("claude-haiku", 0.80, 4.0, 0.08),
]
_DEFAULT = (5.0, 25.0, 0.50)


def prices(model: str) -> tuple[float, float, float]:
    for prefix, i, o, cr in _PRICES:
        if model.startswith(prefix):
            return i, o, cr
    return _DEFAULT


def cost_usd(model: str, inp: int, out: int, cache_read: int, cache_5m: int, cache_1h: int) -> float:
    i, o, cr = prices(model)
    return (inp * i + out * o + cache_read * cr + cache_5m * i * 1.25 + cache_1h * i * 2.0) / 1e6


def family(model: str) -> str:
    """Short display name for a model id: 'claude-opus-5-5' -> 'Opus'."""
    for name in ("Fable", "Mythos", "Opus", "Sonnet", "Haiku"):
        if name.lower() in model:
            return name
    return "Other"
