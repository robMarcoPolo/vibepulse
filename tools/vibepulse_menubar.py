#!/usr/bin/env python3
"""VibePulse tokenserver health, for SwiftBar.

Read-only: this script starts nothing and restarts nothing. Supervision
belongs to launchd (se.torget.tokenserver). Design:
docs/superpowers/specs/2026-09-17-vibepulse-menubar-health-design.md
"""

import json
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

BASE_URL = "http://127.0.0.1:8737/"
TIMEOUT_S = 3
REPO_ROOT = Path(__file__).resolve().parents[1]

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

    served_src = payload.get("srcFingerprint") or "absent"
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

    if probe == "probe_held_by_other_instance":
        reasons.append("another instance holds the probe lock")

    if isinstance(instances, int) and instances > 1:
        reasons.append(f"{instances} tokenserver processes running")

    statusline_version = statusline.get("claudeCodeVersion") or "unknown"
    lines = [
        f"rev {payload.get('rev', '?')}",
        f"fingerprint {served_src}",
        f"started {payload.get('startedAt', '?')}",
        f"probe {probe}",
        f"bridge {bridge_status} (bridged={bridged}), "
        f"Claude Code {statusline_version}",
        f"panel age {age}s",
    ]
    if isinstance(instances, int):
        lines.append(f"instances {instances}")

    return (OK if not reasons else DEGRADED), reasons, lines


def fetch(url=BASE_URL, timeout=TIMEOUT_S):
    """(payload, error). Never raises; a hang is the thing being watched."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            if response.status != 200:
                return None, f"HTTP {response.status}"
            return json.load(response), None
    except urllib.error.HTTPError as error:
        return None, f"HTTP {error.code}"
    except Exception as error:
        # A resilience boundary: a menu bar plugin never crashes.
        return None, f"{type(error).__name__}: {error}"


def count_instances():
    """How many tokenserver.py processes exist, or None if unknown."""
    try:
        found = subprocess.run(
            ["pgrep", "-f", r"[Pp]ython.*tokenserver\.py"],
            capture_output=True, text=True, timeout=TIMEOUT_S, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if found.returncode not in (0, 1):
        return None
    return len([line for line in found.stdout.split("\n") if line.strip()])


def checkout_fingerprint():
    """The hash the tokenserver itself would compute from this checkout."""
    try:
        sys.path.insert(0, str(REPO_ROOT))
        from tools.tokenserver.tokenserver import _read_source_fingerprint
        return _read_source_fingerprint()
    except Exception:
        # Reported as a reason by decide(), never raised.
        return None


def render(state, reasons, lines):
    title = GLYPH[state] + (f" {reasons[0]}" if reasons else "")
    body = list(lines)
    if len(reasons) > 1:
        body = [f"also: {reason}" for reason in reasons[1:]] + body
    return "\n".join([title, "---", *body])


def main():
    try:
        payload, error = fetch()
        state, reasons, lines = decide(
            payload, checkout_fingerprint(), count_instances(), error=error)
        print(render(state, reasons, lines))
    except Exception as error:
        # Last resort: never a traceback on screen.
        print(f"{GLYPH[DOWN]} {type(error).__name__}")
        print("---")
        print(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
