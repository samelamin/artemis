import pathlib
import re
import unittest


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[3]


class UpstreamPortContractTests(unittest.TestCase):
    def test_xcb_platform_always_selects_sdl_x11_driver(self):
        source = (REPOSITORY_ROOT / "app/main.cpp").read_text(encoding="utf-8")

        self.assertRegex(
            source,
            re.compile(
                r'if \(QGuiApplication::platformName\(\) == "xcb"\) \{'
                r'.*?if \(WMUtils::isRunningWayland\(\)\) \{'
                r'.*?qputenv\("SDL_VIDEODRIVER", "x11"\);',
                re.DOTALL,
            ),
        )

    def test_sdl_audio_backpressure_uses_queued_duration(self):
        header = (
            REPOSITORY_ROOT / "app/streaming/audio/renderers/sdl.h"
        ).read_text(encoding="utf-8")
        source = (
            REPOSITORY_ROOT / "app/streaming/audio/renderers/sdlaud.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("Uint32 m_FrameDurationMs;", header)
        self.assertIn(
            "m_FrameDurationMs = opusConfig->samplesPerFrame / "
            "(opusConfig->sampleRate / 1000);",
            source,
        )
        self.assertIn(
            "SDL_GetQueuedAudioSize(m_AudioDevice) / m_FrameSize * "
            "m_FrameDurationMs <= 50",
            source,
        )

    def test_combo_boxes_handle_left_and_right_navigation(self):
        source = (
            REPOSITORY_ROOT / "app/gui/AutoResizingComboBox.qml"
        ).read_text(encoding="utf-8")

        self.assertRegex(
            source,
            re.compile(r"Keys\.onLeftPressed:\s*\{\s*decrementCurrentIndex\(\)\s*\}"),
        )
        self.assertRegex(
            source,
            re.compile(r"Keys\.onRightPressed:\s*\{\s*incrementCurrentIndex\(\)\s*\}"),
        )

    def test_dialog_focus_starts_on_buttons_and_cycles_horizontally(self):
        source = (
            REPOSITORY_ROOT / "app/gui/NavigableMessageDialog.qml"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "dialogButtonBox.itemAt(dialogButtonBox.count - 1).forceActiveFocus(Qt.TabFocus)",
            source,
        )
        self.assertRegex(
            source,
            re.compile(
                r"delegate:\s*Button\s*\{.*?"
                r"Keys\.onReturnPressed:\s*clicked\(\).*?"
                r"Keys\.onEnterPressed:\s*clicked\(\).*?"
                r"Keys\.onRightPressed:\s*nextItemInFocusChain\(true\)"
                r"\.forceActiveFocus\(Qt\.TabFocus\).*?"
                r"Keys\.onLeftPressed:\s*nextItemInFocusChain\(false\)"
                r"\.forceActiveFocus\(Qt\.TabFocus\)",
                re.DOTALL,
            ),
        )

    def test_rfi_workaround_uses_helper_and_opt_in_env(self):
        header = (
            REPOSITORY_ROOT
            / "app/streaming/video/ffmpeg-renderers/rfipolicy.h"
        ).read_text(encoding="utf-8")
        source = (
            REPOSITORY_ROOT / "app/streaming/video/ffmpeg-renderers/vaapi.cpp"
        ).read_text(encoding="utf-8")
        pro = (REPOSITORY_ROOT / "app/app.pro").read_text(encoding="utf-8")
        tests_pro = (
            REPOSITORY_ROOT / "tests/tests.pro"
        ).read_text(encoding="utf-8")
        rfi_pro = (
            REPOSITORY_ROOT / "tests/rfipolicy/rfipolicy.pro"
        ).read_text(encoding="utf-8")

        # Header declares the namespace and exposes the new opt-in member.
        self.assertRegex(
            header,
            re.compile(r"namespace\s+RfiPolicy\s*\{"),
        )
        self.assertRegex(
            header,
            re.compile(
                r"inline\s+bool\s+workaroundEnabled\s*\(\s*const\s+QString\s*&"
            ),
        )
        self.assertIn("HAS_RFI_LATENCY_BUG", header)
        self.assertNotIn("IGNORE_RFI_LATENCY_BUG", header)

        # VAAPI renderer initializes the workaround through the helper.
        self.assertRegex(
            source,
            re.compile(
                r"m_HasRfiLatencyBug\s*=\s*RfiPolicy::workaroundEnabled\(vendorStr\);"
            ),
        )

        # Legacy env name no longer controls the policy in vaapi.cpp nor in
        # the helper. This is the source-level proof that an explicit
        # HAS_RFI_LATENCY_BUG=1 opt-in is honored regardless of whether the
        # legacy IGNORE_RFI_LATENCY_BUG variable is set to "0" or "1".
        self.assertNotIn("IGNORE_RFI_LATENCY_BUG", source)
        self.assertNotIn("IGNORE_RFI_LATENCY_BUG", header)

        # Warning says the workaround was explicitly enabled, not that a
        # driver defect was detected.
        self.assertRegex(
            source,
            re.compile(
                r"VAAPI RFI latency workaround explicitly enabled via HAS_RFI_LATENCY_BUG=1"
            ),
        )

        # Capability gate is the actual function body, not bare strings.
        # getDecoderCapabilities() must gate CAPABILITY_REFERENCE_FRAME_INVALIDATION_*
        # on m_HasRfiLatencyBug, so a future drive-by edit cannot silently
        # decouple the two.
        capability_body = re.search(
            r"int\s+VAAPIRenderer::getDecoderCapabilities\(\)\s*\{.*?\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(
            capability_body,
            "VAAPIRenderer::getDecoderCapabilities() body not found",
        )
        body = capability_body.group(0)
        self.assertIn("m_HasRfiLatencyBug", body)
        self.assertIn("CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC", body)
        self.assertIn("CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1", body)

        # Header is wired into the app build graph and the test suite.
        self.assertIn("streaming/video/ffmpeg-renderers/rfipolicy.h", pro)
        self.assertIn("rfipolicy", tests_pro)
        self.assertIn("streaming/video/ffmpeg-renderers/rfipolicy.h", rfi_pro)


if __name__ == "__main__":
    unittest.main()
