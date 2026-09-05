from pathlib import Path
import json
import re
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class FlatpakDocumentationTests(unittest.TestCase):
    def test_documented_run_commands_match_manifest_app_id(self):
        manifest = json.loads(
            (
                REPOSITORY_ROOT
                / "packaging/flatpak/com.artemisdesktop.ArtemisDesktopDev.json"
            ).read_text(encoding="utf-8")
        )
        expected_app_id = manifest["app-id"]

        documented_app_ids = []
        for relative_path in (
            "README.md",
            "docs/STEAM_DECK_QUICK_START.md",
            "docs/STEAM_DECK.md",
            ".github/ISSUE_TEMPLATE/bug_report.md",
        ):
            path = REPOSITORY_ROOT / relative_path
            with self.subTest(path=relative_path):
                self.assertTrue(path.is_file())
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8")
            documented_app_ids.extend(self._extract_app_ids(text, relative_path))

        self.assertTrue(
            documented_app_ids,
            "No documented flatpak run commands found in tracked docs",
        )
        with self.subTest(check="all-documented-app-ids-match-manifest"):
            unexpected = set(documented_app_ids) - {expected_app_id}
            self.assertEqual(
                unexpected,
                set(),
                f"Documented flatpak run commands used unexpected app ids: "
                f"{sorted(unexpected)}; expected {expected_app_id!r}",
            )

    @staticmethod
    def _extract_app_ids(text, relative_path):
        # Join line continuations so multi-line commands are tokenized as one
        # logical line. The newline (and any following indentation) after a
        # trailing backslash is consumed. Plain whitespace at line ends is
        # preserved because flatpak's argv splitter is whitespace-agnostic.
        normalized = re.sub(r"\\\r?\n[ \t]*", " ", text)

        # Match: `flatpak run` followed by zero or more `--flag=value` or
        # `--flag value` options, then capture the first non-option token
        # (the app id). This intentionally does NOT capture leading flag
        # tokens as app ids; it captures the app id even when commands like
        # `flatpak run --env=HAS_RFI_LATENCY_BUG=1 com.app.App` appear.
        app_id_pattern = re.compile(
            r"flatpak[ \t]+run\b"
            r"(?:[ \t]+--(?:\S+(?:=[^\s]+)?|[^\s]+))*[ \t]+"
            r"([A-Za-z][A-Za-z0-9_.+-]*)"
        )
        return app_id_pattern.findall(normalized)


class FlatpakDocumentationRegressionTests(unittest.TestCase):
    """Regression inputs for the documented-run-command parser.

    The parser must capture the app id and not be fooled by preceding
    `--flag=value` options or line continuations. It must still flag wrong
    app ids in commands that hide inside obvious-looking text.
    """

    @staticmethod
    def _parse(text):
        return FlatpakDocumentationTests._extract_app_ids(text, "doc.md")

    def test_captures_app_id_after_single_env_flag(self):
        text = "Run with:\nflatpak run --env=HAS_RFI_LATENCY_BUG=1 com.example.App\n"
        self.assertEqual(self._parse(text), ["com.example.App"])

    def test_captures_app_id_after_multiple_flags(self):
        text = (
            "flatpak run --env=FOO=bar --assumeyes "
            "com.example.App\n"
        )
        self.assertEqual(self._parse(text), ["com.example.App"])

    def test_captures_app_id_across_line_continuation(self):
        text = (
            "flatpak run --env=HAS_RFI_LATENCY_BUG=1 \\\n"
            "  com.example.App\n"
        )
        self.assertEqual(self._parse(text), ["com.example.App"])

    def test_captures_app_id_with_no_options(self):
        text = "flatpak run com.example.App\n"
        self.assertEqual(self._parse(text), ["com.example.App"])

    def test_does_not_swallow_env_token_as_app_id(self):
        # The historical bug was that `--env` was captured as the app id.
        text = "flatpak run --env=HAS_RFI_LATENCY_BUG=1 com.example.App\n"
        self.assertNotIn("--env", self._parse(text))
        self.assertNotIn("HAS_RFI_LATENCY_BUG=1", self._parse(text))

    def test_flags_wrong_app_id_when_command_uses_typo(self):
        text = (
            "Good:\nflatpak run com.example.App\n"
            "Bad:\nflatpak run com.example.Wrong\n"
        )
        ids = self._parse(text)
        self.assertIn("com.example.App", ids)
        self.assertIn("com.example.Wrong", ids)


if __name__ == "__main__":
    unittest.main()
