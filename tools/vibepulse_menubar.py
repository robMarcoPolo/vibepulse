#!/usr/bin/env python3
"""VibePulse tokenserver health, for SwiftBar.

Read-only: this script starts nothing and restarts nothing. Supervision
belongs to launchd (se.torget.tokenserver). Design:
docs/superpowers/specs/2026-09-17-vibepulse-menubar-health-design.md
"""

OK = "ok"
DEGRADED = "degraded"
DOWN = "down"

GLYPH = {OK: "●", DEGRADED: "◐", DOWN: "○"}

PANEL_MAX_AGE_S = 15


def decide(payload, checkout_src, instances, error=None):
    """Map one GET / body plus local facts onto (state, reasons, lines).

    Pure: no clock, no network, no subprocess, no filesystem. `reasons`
    is empty exactly when the state is OK.
    """
    if error is not None:
        return DOWN, [error], []
    if not isinstance(payload, dict):
        return DOWN, ["unparseable response body"], []
    return OK, [], []
