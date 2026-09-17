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

    reasons = []

    served_src = payload.get("srcFingerprint")
    if checkout_src is None:
        reasons.append("cannot read this checkout's fingerprint")
    elif served_src != checkout_src:
        reasons.append(
            f"running code differs from this checkout "
            f"({served_src} != {checkout_src})")

    probe = payload.get("claudeProbe") or "unknown"
    statusline = payload.get("claudeStatusline") or {}
    bridged = bool(statusline.get("bridged"))
    bridge_status = statusline.get("status")

    # A rate-limited probe is not a fault while the statusLine bridge is
    # covering: session and general week then come from Claude Code's own
    # sample with no upstream call. Only the model week is probe-only.
    if not probe.startswith("usage_http_200") and not bridged:
        reasons.append(f"probe {probe}, bridge not covering")

    if bridge_status in ("missing", "unreadable", "invalid"):
        reasons.append(f"statusLine bridge {bridge_status}")

    panel = (payload.get("interactions") or {}).get("panel") or {}
    age = panel.get("ageS")
    if not isinstance(age, (int, float)) or isinstance(age, bool):
        reasons.append("panel never contacted")
    elif age > PANEL_MAX_AGE_S:
        reasons.append(f"panel last served {int(age)}s ago")

    return (OK if not reasons else DEGRADED), reasons, []
