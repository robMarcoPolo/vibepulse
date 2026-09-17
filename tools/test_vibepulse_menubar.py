import subprocess
import unittest
from unittest.mock import patch

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

    def test_a_missing_fingerprint_says_absent_rather_than_none(self):
        # served_src is None when the key is absent; "None" must never
        # reach the reason string or the dropdown fingerprint line.
        body = payload()
        del body["srcFingerprint"]
        state, reasons, lines = mb.decide(body, SRC, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertNotIn("None", reasons[0])
        self.assertIn("absent", reasons[0])
        self.assertNotIn("None", "\n".join(lines))
        self.assertIn("fingerprint absent", "\n".join(lines))

    def test_an_unreadable_checkout_says_so_rather_than_blaming_the_server(self):
        # checkout_fingerprint() returns None when the import fails; the
        # reason must name that, not read "e0e425fb9e6b != None".
        state, reasons, _ = mb.decide(payload(), None, 1)
        self.assertEqual(mb.DEGRADED, state)
        self.assertIn("checkout", reasons[0])
        self.assertNotIn("None", reasons[0])


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


class _CompletedProcess:
    """Just enough of subprocess.CompletedProcess for these tests."""

    def __init__(self, returncode, stdout):
        self.returncode = returncode
        self.stdout = stdout


class CountInstances(unittest.TestCase):

    def test_a_normal_two_line_match_counts_two(self):
        with patch.object(mb.subprocess, "run",
                           return_value=_CompletedProcess(0, "111\n222\n")):
            self.assertEqual(2, mb.count_instances())

    def test_no_matches_is_zero_not_none(self):
        # pgrep's own convention: returncode 1 means "found nothing",
        # not an error.
        with patch.object(mb.subprocess, "run",
                           return_value=_CompletedProcess(1, "")):
            self.assertEqual(0, mb.count_instances())

    def test_a_pgrep_error_is_unknown(self):
        with patch.object(mb.subprocess, "run",
                           return_value=_CompletedProcess(2, "")):
            self.assertIsNone(mb.count_instances())

    def test_a_missing_pgrep_binary_is_unknown(self):
        with patch.object(mb.subprocess, "run",
                           side_effect=OSError("no such file")):
            self.assertIsNone(mb.count_instances())

    def test_a_timeout_is_unknown(self):
        with patch.object(
                mb.subprocess, "run",
                side_effect=subprocess.TimeoutExpired(cmd="pgrep", timeout=3)):
            self.assertIsNone(mb.count_instances())


if __name__ == "__main__":
    unittest.main()
