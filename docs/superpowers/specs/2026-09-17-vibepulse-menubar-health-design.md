# VibePulse menu bar health indicator

**Date:** 2026-09-17

**Status:** Designed, not implemented.

**Scope:** A personal macOS menu bar indicator that answers one question —
is the tokenserver on this Mac healthy, running this checkout's code, and
actually delivering data? It is a **read-only indicator**: it starts
nothing, restarts nothing and changes no state. Supervision belongs to
launchd (`se.torget.tokenserver`, `KeepAlive`). The firmware, the wire
contract, the tokenserver and the statusLine bridge are unchanged; this
adds one script and one symlink and touches nothing else in the repo.

**Audience:** the maintainer's own Mac. macOS-only by construction, no
Windows counterpart, no CI, no support promise. It is not part of the
setup runbook and `agent-setup.md` does not gain a step.

## Problem

Two failures on 2026-09-16/17 were both invisible until someone went
looking, and neither is detectable by the supervision we have.

**1. A live process serving stale numbers.** The Claude probe sat in a
`usage_http_429` backoff for roughly 90 minutes. The process was healthy
by every measure launchd can see. The panel showed a blank session and a
102-minute-old Fable week, and the only way to learn why was to read
`GET /` by hand.

**2. A wedged-but-listening process.** Installing the LaunchAgent against
`.venv/bin/python` enabled the optional `zeroconf` discovery advertiser
for the first time. With it active the HTTP server deadlocks: a process
sample showed the accept loop healthy in `select_poll_poll` while every
worker thread sat in `PyThread_acquire_lock_timed`. Connections were
accepted and closed with no bytes written, piling up in `CLOSE_WAIT`; at
five the server was inert. Reproduced deterministically twice, ~2 minutes
after start:

| | discovery ON (venv 3.11) | discovery OFF (brew 3.14) |
|---|---|---|
| `CLOSE_WAIT` | climbs 0 to 5 | stays 0 |
| HTTP after ~2 min | dead (`000`) | 12/12 × `200` over 4 min |

`KeepAlive` cannot see this: the process never exits. A port check cannot
see it either — the socket is listening and `connect()` succeeds. Only a
**completed HTTP request** distinguishes the two states. That is the
central requirement of this design, and the reason a naive "is it up?"
indicator would have shown green throughout the outage.

A third, older failure shares the shape. `docs/lessons.md` records twice
(2026-08-13, 2026-08-28 recurrence) a service quietly running a different
checkout than the one being edited. The guards added afterwards — `rev`
and `srcFingerprint` on `GET /` — make it *visible*, but only to someone
who asks.

All three are answerable from one `GET /`. Nothing asks.

## What it watches

Three signals, each tied to a failure above.

**1. Serving.** One `GET http://127.0.0.1:8737/` with a hard 3-second
timeout, parsed as JSON. Not a port check, not a TCP connect — see the
wedge above. A timeout is the *positive* signal for the deadlock, so it
must be bounded well below the refresh interval and never inherited from
a system default.

**2. Running this checkout's code.** Compare the served `srcFingerprint`
against the same hash recomputed from disk, importing
`_read_source_fingerprint` from the tokenserver rather than
reimplementing it — `smoke.py:473` already does exactly this, and a
second definition of the hash could drift from the server's.

`rev` is deliberately **not** the health signal. It is wrong in both
directions, as this repo demonstrates daily:

- *False alarms:* `rev` moved `7ca27a2` → `0d6f8b5` → `97110b6` on
  2026-09-16/17 while `srcFingerprint` held at `e0e425fb9e6b` — those
  commits were firmware and docs, code the tokenserver never loads. A
  `rev` comparison would sit amber for days at a time and train the
  reader to ignore it.
- *Missed alarms:* `rev` cannot see an uncommitted edit or a change made
  after the process started. The fingerprint catches both.

`rev` is still shown in the dropdown as information.

**3. Data flowing.** `claudeProbe`, `claudeStatusline.status` /
`bridged`, and `interactions.panel.ageS`.

The subtlety that makes this signal worth having: **a `429` backoff is
not a fault when `bridged` is true.** Since the statusLine bridge was
installed on 2026-09-17 the session and general week come from Claude
Code's own stdin sample, with no upstream call; a resting probe is then
the system working as designed. Only the Fable model week is probe-only
and honestly degrades. Treating every non-`ok` probe as amber would light
the indicator during normal operation — the same alert-fatigue failure
the colour rules below exist to avoid.

**4. Only one instance.** Two signals, because neither alone suffices.

The tokenserver holds a machine-wide `flock` on `claude-probe.lock` so
at most one instance ever reaches `api.anthropic.com`; an instance that
loses that race publishes `probe_held_by_other_instance`
(`tokenserver.py:1539`). That status is free — already on the wire — and
is definitive proof of a second instance. It is also one-sided: it is
visible only from the instance that *lost* the lock, so the one being
queried may be the holder and look entirely normal.

A `pgrep -f "tokenserver\.py"` count closes that gap, and is the only
check here that sees an instance started on a different port. Two or
more matching processes is DEGRADED.

A second instance on the *same* port does not persist: a bind failure is
not handled specially, so `Address already in use` propagates and the
process exits. Under `KeepAlive` with `ThrottleInterval 30` that becomes
a respawn every 30 seconds behind whoever owns the port. Detecting that
loop would mean remembering the previous `runs` counter, and this design
carries no state between runs; the two signals above are what stays
stateless.

## States

| State | Glyph | Condition |
|---|---|---|
| DOWN | `○` | no response, timeout, non-200, or unparseable body |
| DEGRADED | `◐` | responds, but fingerprint mismatches, data is not flowing, or more than one instance is running |
| OK | `●` | responds, fingerprint matches, data is fresh |

"Data is not flowing" means any of: `interactions.panel.ageS` exceeds
**15 seconds**; the probe is failing **and** the bridge is not covering;
or the bridge reports `missing`, `unreadable` or `invalid`.

Fifteen seconds is fifteen missed polls — the device polls at 1 Hz, so
the signal is unambiguous well before the threshold is reached. It
follows that a sleeping device or a WiFi drop shows amber. That is
correct rather than noisy: data genuinely is not reaching the panel,
which is the thing this indicator exists to report.

A state is never inferred from a stale reading. If a refresh fails, the
indicator shows DOWN with the reason — it does not keep painting the last
good state, which is the mistake that let the original stale-data
complaint go unnoticed for 90 minutes.

## Structure

Two units, split so the interesting half is testable.

```
decide(payload: dict | None, checkout_src: str, error: str | None)
    -> (State, list[Line])
```

Pure: no clock, no network, no subprocess, no filesystem. Every state
above is reachable by handing it a dict. This is the only practical way
to test the wedge — reproducing a deadlocked server in a unit test is
impractical, but feeding its last-known JSON to a pure function is
trivial.

`main()` is thin: fetch, recompute the fingerprint, call `decide`, print.
It owns all I/O and all error trapping.

## Output contract

SwiftBar renders stdout: the first line is the menu bar title, `---`
opens the dropdown. The dropdown carries, in order: the state and its
reason; `rev` and fingerprint (with checkout values when they differ);
uptime from `startedAt`; probe status with cooldown; bridge status and
`bridged`; panel contact age.

## Where it lives

The script lives in the repo at `tools/vibepulse_menubar.py`. SwiftBar's
plugin folder gets a **symlink** named `vibepulse.30s.py` pointing at it.

A copy would become the sixth absolute-path integration on this Mac that
can silently drift to an old checkout — the failure `docs/lessons.md`
records twice, once as a recurrence across four integrations at once. A
symlink cannot drift. The refresh interval lives in the symlink's name,
so the cadence is configuration, not code, and changing it is a rename
with no scheduler to get wrong.

## Error handling

The indicator watches for a server that accepts connections and never
answers. It must therefore never hang on one: the 3-second timeout is the
mechanism, and `main()` traps every exception and renders it as DOWN with
a one-line reason. A traceback must never reach the menu bar. SwiftBar
re-runs the script on its own schedule, so a failed run costs one stale
cycle and nothing else — there is no retry logic and no state carried
between runs.

## Testing

Tests live at `tools/test_vibepulse_menubar.py` and follow the host
suite's style (`unittest`, fixtures inline). They are run by hand; this
is a personal utility and is not wired into CI or the tokenserver
suite. `decide()` is covered for: healthy;
fingerprint mismatch; probe 429 **with** `bridged` true (expected OK);
probe 429 **without** the bridge (expected DEGRADED); bridge
`invalid`/`missing`; stale panel; `None` payload with each error kind.

The captured `GET /` payloads from the 2026-09-16 stale-data incident and
the 2026-09-17 wedge are used verbatim as fixtures, so the two failures
that motivated the tool are the two the tests pin.

`main()` is not unit-tested; it is exercised by running the script.

## Non-goals

- **Supervision.** launchd owns restarts. An indicator that restarts
  things would re-open the decision taken on 2026-09-17 and add a
  second, unsupervised supervisor.
- **Controls.** No restart, no log tailing, no smoke runs. Read-only was
  the explicit scope choice; a console is a different tool.
- **Quota display.** The device shows quota. Duplicating it here would
  compete for the same strip of menu bar as the health glyph and dilute
  the one question this answers.
- **Windows or Linux.** No counterpart is planned.
- **Fixing the discovery deadlock.** Out of scope and tracked separately;
  this design only has to stay honest while it is unfixed.

## Resolved during review (2026-09-17)

1. **Panel-age threshold: 15 seconds.** See signal 3.
2. **Import cost accepted.** Importing the tokenserver module every 30 s
   to reach `_read_source_fingerprint` costs ~100 ms and pulls in a large
   module for ten lines of hashing. Accepted as the price of a single
   definition of the hash. Should it prove noisy, extracting the hash to
   a small shared module is preferred over duplicating it.
3. **Second instance: detected, via signal 4.** Comparing the port owner
   against the LaunchAgent pid was considered and declined — a second
   subprocess per refresh for a case the two chosen signals largely
   already reach.

## Open questions

None outstanding.
