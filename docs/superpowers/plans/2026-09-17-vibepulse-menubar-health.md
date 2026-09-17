# VibePulse Menu Bar Health Indicator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A read-only macOS menu bar indicator that says whether the local VibePulse tokenserver is serving, running this checkout's code, delivering data, and running alone.

**Architecture:** One stdlib-only Python script rendered by SwiftBar every 30 s. A pure `decide()` function maps a `GET /` payload plus two locally gathered facts onto a state and dropdown lines; a thin `main()` does all I/O. SwiftBar owns scheduling, rendering and login persistence, so there is no build, no signing and no lifecycle code.

**Tech Stack:** Python 3 (standard library only), SwiftBar, `unittest`.

**Spec:** `docs/superpowers/specs/2026-09-17-vibepulse-menubar-health-design.md`

## Global Constraints

- **Standard library only.** No pip installs. The host service is "a plain directory of scripts" (`pyproject.toml`) and this follows it.
- **macOS only.** No Windows or Linux counterpart is planned.
- **Read-only.** The script starts nothing, restarts nothing, writes no state. Supervision belongs to launchd (`se.torget.tokenserver`).
- **Stateless between runs.** Nothing is remembered from one refresh to the next.
- **Never hang.** Every network call uses a hard 3-second timeout. The tool exists to detect a server that accepts connections and never answers; it must not itself hang on one.
- **No traceback ever reaches the menu bar.** `main()` traps every exception and renders it as DOWN with a one-line reason.
- **Panel age threshold: 15 seconds.**
- **Refresh interval: 30 s**, expressed only in the SwiftBar symlink's filename.
- **Repo is not a Python package.** Tests run from the repo root as `python3 -m unittest tools.<module> -v` (implicit namespace packages). Precedent: `tools/test_hardware_registry.py`.
- **Not wired into CI.** Do not add this to `test/run.sh` or `test/tokenserver-suite.txt`; it is a personal utility.
- **Lint:** `ruff check .` must pass (rules F, E722, B, S110, PLE, PLW0602).

---

## File Structure

| File | Responsibility |
|---|---|
| `tools/vibepulse_menubar.py` | Create. `decide()` (pure) + `main()` (all I/O). |
| `tools/test_vibepulse_menubar.py` | Create. `unittest` coverage of `decide()`. |
| SwiftBar plugin folder | A **symlink** `vibepulse.30s.py` → `tools/vibepulse_menubar.py`. Never a copy. |

No existing repo file is modified.

---

### Task 1: Module skeleton, states, and the DOWN paths

Establishes the signature every later task extends, and covers the failure the tool was built for: a server that does not answer.

**Files:**
- Create: `tools/vibepulse_menubar.py`
- Test: `tools/test_vibepulse_menubar.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - Constants `OK = "ok"`, `DEGRADED = "degraded"`, `DOWN = "down"`, `GLYPH: dict[str, str]`, `PANEL_MAX_AGE_S = 15`
  - `decide(payload, checkout_src, instances, error=None) -> tuple[str, list[str], list[str]]` returning `(state, reasons, lines)`. `reasons` is empty exactly when the state is `OK`.

- [ ] **Step 1: Write the failing test**

Create `tools/test_vibepulse_menubar.py`:

```python
import unittest

from tools import vibepulse_menubar as mb


def payload(**overrides):
    """A healthy GET / body, shaped like the real one."""
    document = {
        "rev": "0d6f8b5",
        "srcFingerprint": "e0e425fb9e6b",
        "startedAt": "2026-09-17T09:10:13+01:00",
        "claudeProbe": "usage_http_200 + ok",
        "claudeStatusline": {
            "status": "fresh",
            "ageS": 4,
            "claudeCodeVersion": "2.1.267",
            "bridged": True,
            "account": "assumed-single",
        },
        "interactions": {
            "panel": {"status": "ready", "ageS": 0,
                      "route": "/api/agent-status"},
        },
    }
    document.update(overrides)
    return document


SRC = "e0e425fb9e6b"


class DownStates(unittest.TestCase):

    def test_an_error_is_down_and_carries_its_reason(self):
        state, reasons, _ = mb.decide(None, SRC, 1, error="timeout after 3s")
        self.assertEqual(mb.DOWN, state)
        self.assertEqual(["timeout after 3s"], reasons)

    def test_a_non_dict_body_is_down(self):
        state, reasons, _ = mb.decide("<html>503</html>", SRC, 1)
        self.assertEqual(mb.DOWN, state)
        self.assertTrue(reasons)

    def test_a_healthy_payload_is_ok_with_no_reasons(self):
        state, reasons, _ = mb.decide(payload(), SRC, 1)
        self.assertEqual(mb.OK, state)
        self.assertEqual([], reasons)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tools.vibepulse_menubar'`

- [ ] **Step 3: Write the minimal implementation**

Create `tools/vibepulse_menubar.py`:

```python
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
```

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git commit -m "feat(menubar): decide() skeleton and the DOWN paths

A server that accepts connections and never answers is the failure this
tool exists to catch, so it is the first state implemented."
```

---

### Task 2: Signal 2 — running this checkout's code

**Files:**
- Modify: `tools/vibepulse_menubar.py`
- Test: `tools/test_vibepulse_menubar.py`

**Interfaces:**
- Consumes: `decide`, `OK`, `DEGRADED`, `DOWN` from Task 1.
- Produces: no new names; `decide` now returns `DEGRADED` when `payload["srcFingerprint"] != checkout_src`.

- [ ] **Step 1: Write the failing test**

Append to `tools/test_vibepulse_menubar.py`:

```python
class Fingerprint(unittest.TestCase):

    def test_a_differing_fingerprint_is_degraded(self):
        state, reasons, _ = mb.decide(
            payload(srcFingerprint="19442f38b93f"), SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertIn("19442f38b93f", reasons[0])
        self.assertIn(SRC, reasons[0])

    def test_rev_alone_never_decides_the_state(self):
        # rev moves on every firmware commit while the tokenserver's own
        # sources are untouched; only the fingerprint may colour the glyph.
        state, reasons, _ = mb.decide(payload(rev="97110b6"), SRC, 1)
        self.assertEqual(mb.OK, state)
        self.assertEqual([], reasons)

    def test_a_missing_fingerprint_is_degraded(self):
        body = payload()
        del body["srcFingerprint"]
        state, reasons, _ = mb.decide(body, SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(reasons)

    def test_an_unreadable_checkout_says_so_rather_than_blaming_the_server(self):
        # checkout_fingerprint() returns None when the import fails; the
        # reason must name that, not read "e0e425fb9e6b != None".
        state, reasons, _ = mb.decide(payload(), None, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertIn("checkout", reasons[0])
        self.assertNotIn("None", reasons[0])
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `python3 -m unittest tools.test_vibepulse_menubar.Fingerprint -v`
Expected: FAIL — the differing-fingerprint test gets `ok`, not `degraded`

- [ ] **Step 3: Write the minimal implementation**

In `tools/vibepulse_menubar.py`, replace the final `return OK, [], []` of `decide` with:

```python
    reasons = []

    served_src = payload.get("srcFingerprint")
    if checkout_src is None:
        reasons.append("cannot read this checkout's fingerprint")
    elif served_src != checkout_src:
        reasons.append(
            f"running code differs from this checkout "
            f"({served_src} != {checkout_src})")

    return (OK if not reasons else DEGRADED), reasons, []
```

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: PASS (7 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git commit -m "feat(menubar): the fingerprint decides staleness, never rev

rev moves on every firmware commit while the tokenserver's sources are
untouched, and cannot see an uncommitted edit at all."
```

---

### Task 3: Signal 3 — data flowing

The subtle one: a 429 backoff is **not** a fault while the statusLine bridge is covering.

**Files:**
- Modify: `tools/vibepulse_menubar.py`
- Test: `tools/test_vibepulse_menubar.py`

**Interfaces:**
- Consumes: `decide`, `PANEL_MAX_AGE_S` from Task 1.
- Produces: no new names.

- [ ] **Step 1: Write the failing test**

Append to `tools/test_vibepulse_menubar.py`:

```python
BACKOFF = "usage_http_429 + backoff_until_09:12"


class DataFlowing(unittest.TestCase):

    def test_a_429_is_healthy_while_the_bridge_covers(self):
        # Captured 2026-09-17: the probe rate-limited, the bridge feeding,
        # session and week both live. The system working as designed.
        state, reasons, _ = mb.decide(payload(claudeProbe=BACKOFF), SRC, 1)
        self.assertEqual(mb.OK, state)
        self.assertEqual([], reasons)

    def test_a_429_without_the_bridge_is_degraded(self):
        # Captured 2026-09-16: probe resting, no bridge, blank session and
        # a 102-minute-old model week on the panel.
        body = payload(
            claudeProbe=BACKOFF,
            claudeStatusline={"status": "not_installed", "ageS": None,
                              "bridged": False, "account": "assumed-single"})
        state, reasons, _ = mb.decide(body, SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(any("429" in r for r in reasons))

    def test_a_broken_bridge_is_degraded_even_with_a_healthy_probe(self):
        for status in ("missing", "unreadable", "invalid"):
            with self.subTest(status=status):
                body = payload(claudeStatusline={
                    "status": status, "bridged": False})
                state, reasons, _ = mb.decide(body, SRC, 1)
                self.assertEqual(mb.DEGRADED, state)
                self.assertTrue(any(status in r for r in reasons))

    def test_a_stale_panel_is_degraded_at_the_threshold(self):
        body = payload(interactions={"panel": {
            "status": "ready", "ageS": mb.PANEL_MAX_AGE_S + 1}})
        state, reasons, _ = mb.decide(body, SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(any("panel" in r for r in reasons))

    def test_a_panel_age_at_the_threshold_is_still_ok(self):
        body = payload(interactions={"panel": {
            "status": "ready", "ageS": mb.PANEL_MAX_AGE_S}})
        state, _, _ = mb.decide(body, SRC, 1)
        self.assertEqual(mb.OK, state)

    def test_an_absent_panel_age_is_degraded(self):
        body = payload(interactions={"panel": {"status": "warming",
                                               "ageS": None}})
        state, reasons, _ = mb.decide(body, SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(any("panel" in r for r in reasons))
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `python3 -m unittest tools.test_vibepulse_menubar.DataFlowing -v`
Expected: FAIL — every degraded case reports `ok`

- [ ] **Step 3: Write the minimal implementation**

In `decide`, insert after the fingerprint block and before the `return`:

```python
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
```

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: PASS (13 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git commit -m "feat(menubar): data-flowing signal, with the bridge exemption

Treating every non-ok probe as amber would light the indicator during
normal operation and train the reader to ignore it."
```

---

### Task 4: Signal 4 — only one instance

**Files:**
- Modify: `tools/vibepulse_menubar.py`
- Test: `tools/test_vibepulse_menubar.py`

**Interfaces:**
- Consumes: `decide` from Task 1.
- Produces: no new names; the `instances` parameter now affects the state.

- [ ] **Step 1: Write the failing test**

Append to `tools/test_vibepulse_menubar.py`:

```python
class Instances(unittest.TestCase):

    def test_two_processes_is_degraded(self):
        state, reasons, _ = mb.decide(payload(), SRC, 2)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(any("2" in r for r in reasons))

    def test_the_lost_probe_lock_is_degraded(self):
        # Free and definitive, but one-sided: only the instance that LOST
        # the lock publishes it, so the pgrep count above is still needed.
        state, reasons, _ = mb.decide(
            payload(claudeProbe="probe_held_by_other_instance"), SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertTrue(any("lock" in r for r in reasons))

    def test_one_process_is_ok(self):
        state, _, _ = mb.decide(payload(), SRC, 1)
        self.assertEqual(mb.OK, state)

    def test_an_unknown_process_count_does_not_decide_the_state(self):
        # pgrep failing must not invent a fault.
        state, _, _ = mb.decide(payload(), SRC, None)
        self.assertEqual(mb.OK, state)
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `python3 -m unittest tools.test_vibepulse_menubar.Instances -v`
Expected: FAIL — both degraded cases report `ok`

- [ ] **Step 3: Write the minimal implementation**

In `decide`, insert before the `return`:

```python
    if probe == "probe_held_by_other_instance":
        reasons.append("another instance holds the probe lock")

    if isinstance(instances, int) and instances > 1:
        reasons.append(f"{instances} tokenserver processes running")
```

Note: the `probe_held_by_other_instance` string already fails the
`usage_http_200` test in Task 3, so it will also produce a "bridge not
covering" reason when the bridge is down. Both reasons are true; the
dropdown shows both.

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: PASS (17 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git commit -m "feat(menubar): detect a second instance two ways

The probe lock's status is free and definitive but visible only from the
instance that lost it; a pgrep count is the only check that sees an
instance started on another port."
```

---

### Task 5: Dropdown lines and `main()`

**Files:**
- Modify: `tools/vibepulse_menubar.py`
- Test: `tools/test_vibepulse_menubar.py`

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `decide` now returns a populated `lines` list.
  - `fetch(url, timeout) -> tuple[dict | None, str | None]` returning `(payload, error)`.
  - `count_instances() -> int | None`
  - `checkout_fingerprint() -> str | None`
  - `render(state, reasons, lines) -> str`
  - `main() -> int`

- [ ] **Step 1: Write the failing test**

Append to `tools/test_vibepulse_menubar.py`:

```python
class Rendering(unittest.TestCase):

    def test_lines_carry_the_facts_a_reader_needs(self):
        _, _, lines = mb.decide(payload(), SRC, 1)
        joined = "\n".join(lines)
        self.assertIn("0d6f8b5", joined)          # rev, as information
        self.assertIn(SRC, joined)                # fingerprint
        self.assertIn("usage_http_200", joined)   # probe
        self.assertIn("2.1.267", joined)          # Claude Code version

    def test_render_puts_the_glyph_first_and_opens_a_dropdown(self):
        out = mb.render(mb.OK, [], ["rev 0d6f8b5"])
        first, rest = out.split("\n", 1)
        self.assertTrue(first.startswith(mb.GLYPH[mb.OK]))
        self.assertIn("---", rest)
        self.assertIn("rev 0d6f8b5", rest)

    def test_render_shows_the_first_reason_in_the_title_when_not_ok(self):
        out = mb.render(mb.DOWN, ["timeout after 3s"], [])
        self.assertTrue(out.startswith(mb.GLYPH[mb.DOWN]))
        self.assertIn("timeout after 3s", out.split("\n", 1)[0])
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `python3 -m unittest tools.test_vibepulse_menubar.Rendering -v`
Expected: FAIL — `AttributeError: module has no attribute 'render'`

- [ ] **Step 3: Write the minimal implementation**

Add the imports at the top of `tools/vibepulse_menubar.py`, under the docstring:

```python
import json
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

BASE_URL = "http://127.0.0.1:8737/"
TIMEOUT_S = 3
REPO_ROOT = Path(__file__).resolve().parents[1]
```

Build the dropdown at the end of `decide`, replacing its `return`:

```python
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
```

Then add the I/O layer at the end of the file:

```python
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
            ["pgrep", "-f", r"tokenserver\.py"],
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
```

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `python3 -m unittest tools.test_vibepulse_menubar -v`
Expected: PASS (20 tests)

Then run it for real and read the output:

Run: `python3 tools/vibepulse_menubar.py`
Expected: three or more lines, the first beginning with `●`, `◐` or `○`, then `---`, then the detail lines. No traceback under any circumstance.

Then confirm it stays honest when the server is unreachable:

Run:

```bash
python3 - <<'PYEOF'
import tools.vibepulse_menubar as m
payload, error = m.fetch("http://127.0.0.1:9/")
print(m.render(*m.decide(payload, "x", 1, error=error)))
PYEOF
```

Expected: a `○` title naming a connection error — not a traceback. For example:
`○ URLError: <urlopen error [Errno 61] Connection refused>`

(The `error` argument must be passed by keyword. `decide(*m.fetch(url), 'x', 1)`
looks equivalent but lands `'x'` and `1` on `checkout_src` and `instances`,
putting the error string in the wrong slot and printing `○ 1`.)

- [ ] **Step 5: Lint and commit**

```bash
ruff check tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git add tools/vibepulse_menubar.py tools/test_vibepulse_menubar.py
git commit -m "feat(menubar): dropdown lines and the I/O layer

fetch, pgrep and the fingerprint import are the only impure parts; each
returns a value rather than raising, so decide() stays total."
```

---

### Task 6: Wire it into SwiftBar and verify on screen

The only task with no unit test: its deliverable is a glyph in the menu bar.

**Files:**
- Create: a symlink in the SwiftBar plugin folder. No repo file changes.

**Interfaces:**
- Consumes: `tools/vibepulse_menubar.py` from Task 5.
- Produces: nothing other tasks depend on.

- [ ] **Step 1: Choose and create the plugin folder**

SwiftBar asks for a plugin folder on first launch; if it was skipped the
preference is unset. Check and set it:

```bash
defaults read com.ameba.SwiftBar PluginDirectory 2>/dev/null \
  || echo "(unset — choose one in SwiftBar's preferences)"
mkdir -p ~/.swiftbar-plugins
```

Set the folder to `~/.swiftbar-plugins` in SwiftBar → Preferences → Plugin
Folder. This must be done in the UI: writing the preference behind
SwiftBar's back does not register it.

- [ ] **Step 2: Create the symlink, never a copy**

```bash
ln -sfn "$PWD/tools/vibepulse_menubar.py" \
        ~/.swiftbar-plugins/vibepulse.30s.py
ls -l ~/.swiftbar-plugins/vibepulse.30s.py
```

Expected: a symlink pointing into this checkout. A copy would become the
sixth absolute-path integration on this Mac that can drift to an old
checkout — the failure `docs/lessons.md` records twice.

- [ ] **Step 3: Make sure the script is executable**

```bash
chmod +x tools/vibepulse_menubar.py
head -1 tools/vibepulse_menubar.py
```

Expected: `#!/usr/bin/env python3`

- [ ] **Step 4: Refresh SwiftBar and read the menu bar**

Open SwiftBar (or its menu → Refresh All). Expected: a `●`, `◐` or `○`
in the menu bar, and a dropdown listing rev, fingerprint, started, probe,
bridge and panel age.

Verify the states are reachable and honest, one at a time:

```bash
# DEGRADED via the fingerprint: touch a tokenserver source, do not commit.
printf '\n# menubar state check\n' >> tools/tokenserver/quota_cache.py
python3 tools/vibepulse_menubar.py | head -1     # expect ◐
git checkout -- tools/tokenserver/quota_cache.py
python3 tools/vibepulse_menubar.py | head -1     # expect ● again
```

Expected: `◐` while the file is dirty, `●` after the revert. This proves
the check sees an uncommitted edit, which a `rev` comparison cannot.

- [ ] **Step 5: Commit**

Only the executable bit is a repo change; the symlink is local.

```bash
git add tools/vibepulse_menubar.py
git commit -m "chore(menubar): make the plugin executable for SwiftBar"
```

---

## Verification

After Task 6, all of the following must hold:

- `python3 -m unittest tools.test_vibepulse_menubar -v` — 20 tests pass
- `ruff check tools/` — clean
- `python3 tools/vibepulse_menubar.py` — prints a SwiftBar document, never a traceback
- A glyph is visible in the menu bar and its dropdown matches `curl -s http://127.0.0.1:8737/`
- `◐` appears when a tokenserver source is edited without committing, and clears on revert
- `test/run.sh` and `test/tokenserver-suite.txt` are unchanged — this is a personal utility and is deliberately not in CI
