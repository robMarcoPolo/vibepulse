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
