import pathlib
import re
import subprocess
import unittest


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[3]


class SteamDeckLatencyContractTests(unittest.TestCase):
    def test_moonlight_common_c_submodule_pinned_to_fork_head_or_later(self):
        result = subprocess.run(
            ["git", "submodule", "status", "moonlight-common-c/moonlight-common-c"],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=True,
        )

        # A leading '-' means the submodule isn't checked out at all, which
        # would silently build against nothing; a leading '+' means it's
        # checked out at a commit other than what's recorded, which is fine
        # for local development but should never land committed.
        self.assertFalse(result.stdout.startswith("-"), "submodule not initialized")

        pinned_sha = result.stdout.strip().lstrip("+- ").split()[0]
        self.assertNotEqual(
            pinned_sha,
            "ad329b240f18826f320ce6a99226b36354b86b59",
            "moonlight-common-c is still pinned to the pre-keepalive commit; "
            "LiSendEmptyPayload() will not be available",
        )

    def test_session_sends_wifi_keepalive_on_a_throttled_cadence(self):
        header = (REPOSITORY_ROOT / "app/streaming/session.h").read_text(encoding="utf-8")
        source = (REPOSITORY_ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")

        self.assertIn("void sendWifiKeepaliveIfNeeded();", header)
        self.assertIn("Uint32 m_LastWifiKeepaliveTimeMs;", header)

        self.assertRegex(
            source,
            re.compile(
                r"void Session::sendWifiKeepaliveIfNeeded\(\)\s*\{"
                r".*?SDL_TICKS_PASSED\(now, m_LastWifiKeepaliveTimeMs \+ WIFI_KEEPALIVE_INTERVAL_MS\)"
                r".*?LiSendEmptyPayload\(\);",
                re.DOTALL,
            ),
        )

        # Must be reachable from the idle branches of the main SDL event loop
        # (not gated behind an actual event arriving), or it will never fire
        # during the quiet periods it exists to cover.
        self.assertEqual(source.count("sendWifiKeepaliveIfNeeded();"), 2)

    def test_steam_input_controllers_report_li_ctype_steam(self):
        source = (
            REPOSITORY_ROOT / "app/streaming/input/gamepad.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("#define LI_CTYPE_STEAM 0x04", source)
        self.assertRegex(
            source,
            re.compile(
                r"case SDL_CONTROLLER_TYPE_VIRTUAL:"
                r".*?SDL_GameControllerGetSteamHandle\(state->controller\) != 0"
                r".*?type = LI_CTYPE_STEAM;",
                re.DOTALL,
            ),
        )


if __name__ == "__main__":
    unittest.main()
