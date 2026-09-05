# Steam Deck Moonlight Upstream Audit

**Audit date:** 2026-07-21

**Artemis branch:** `codex/steam-deck`

**Upstream reference:** `research/moonlight-master`

**Common history:** `1bf86f52d368935e60780acc2d9d3c5265f6bce8`

## Method and acceptance criteria

Artemis carries substantial fork-specific work on its Moonlight 6.1-era base, so the candidates below were reviewed as individual behavior ports rather than cherry-picked as a batch. Each patch was inspected from its fetched commit and compared with the current Artemis source.

A candidate was accepted only when it:

1. fixes a failure or reliability problem observable in the supported Steam Deck Flatpak path;
2. does not require later Moonlight SDL, renderer, or platform refactors;
3. preserves Artemis-specific streaming, branding, fractional-refresh, and pinned FFmpeg/libplacebo work;
4. has a narrow automated regression contract where practical; and
5. avoids unrelated behavior changes.

The supported Deck path for this release is the Flatpak under Gamescope/Wayland, with XWayland as a compatibility fallback. Direct-display EGLFS/KMSDRM is not the supported Steam Deck launch path.

## Accepted ports

| Upstream | Result | Dependencies and Steam Deck relevance | Artemis commit and verification |
| --- | --- | --- | --- |
| `402ac593` — select the X11 SDL driver for an xcb Qt platform | Accepted, ported by intent | Uses only the existing Qt platform name, `WMUtils::isRunningWayland()`, and `SDL_VIDEODRIVER`. It makes the SDL/Qt backend pairing deterministic when a Deck Flatpak falls back to xcb/XWayland. The XWayland warning remains conditional, while plain X11 also selects SDL's X11 driver. The KMSDRM selection was moved into the same mutually exclusive platform chain. | `1e82d12a` (`fix: align SDL driver with X11 platform`). A source-contract test requires the xcb outer branch, nested XWayland warning check, and X11 driver assignment. |
| `4cf498b0` — constrain SDL audio latency by queued duration | Accepted | Uses APIs already present in Artemis: Opus sample rate/frame size, SDL queued-audio byte count, and Limelight's pending-duration backpressure. The old ten-frame threshold could allow 100 ms when the host negotiates 10 ms packets; the port consistently caps SDL's queue at 50 ms. This is material to handheld latency and audio/video synchronization. The macOS-specific requested buffer-size branch remains untouched. | `f8a46ef1` (`fix: constrain SDL audio queue by duration`). The regression contract requires calculation of frame duration and duration-based queue gating. |
| `02004bac` — change combo-box options with Left/Right | Accepted | Qt Quick Controls `ComboBox.decrementCurrentIndex()` and `incrementCurrentIndex()` are available on the current QML base and require no later navigation refactor. This lets the Deck D-pad change focused settings without opening the popup. Existing popup navigation-mode handling is preserved. | `15633c07` (`fix: navigate combo options with left and right`). The regression contract requires both key handlers and their direction-specific actions. |
| `d040bd24` — focus and navigate dialog buttons | Accepted | Uses the existing `DialogButtonBox`, button delegate, and Qt focus chain. Initial focus moves to the final standard button (the conservative/default choice), Return/Enter activates the focused button, and Left/Right traverses buttons. This directly improves Deck controller use for errors, confirmations, and help dialogs. | `65c87bc4` (`fix: navigate dialog buttons with gamepad`). The regression contract requires initial button focus, activation handlers, and bidirectional focus-chain traversal. |

## Already present or superseded

| Upstream | Result | Evidence |
| --- | --- | --- |
| `2a1749e2` — disable libplacebo internally synchronized queues for Gamescope | Already present | `packaging/flatpak/libplacebo-disable-internally-synchronized-queues.patch` contains the same functional hunk (`internallySynchronizedQueues = false`) with additional rationale, and the pinned libplacebo module in `com.artemisdesktop.ArtemisDesktopDev.json` applies it. This was landed as part of `5ecafd2b`; importing the AppImage workflow change would duplicate behavior and modify an unsupported packaging path. |
| `2a63ad53` — stop polling gamepads while the GUI is unfocused/hidden | Present and subsequently refined | The upstream commit is already in Artemis ancestry. Follow-up `b6a33692` replaced repeated SDL subsystem enable/disable with `notifyWindowFocus()` and timer-state gating. Current `main.qml` reports `visible && active` on both visibility and activity changes, while `SdlGamepadKeyNavigation::updateTimerState()` starts polling only when enabled and focused. Reapplying the older patch would regress that refinement. |

## Rejected candidates

| Upstream | Decision | Reason |
| --- | --- | --- |
| `53a7680a` — probe AV1 before preferring it to H.264 for SDR | Rejected for this Deck scope | The added probe is not reachable on Steam Deck's Linux/x86 build. In that configuration the existing preprocessor branch includes `\|\| !enableHdr`, so SDR AV1 is already deprioritized before the new `else if`; for HDR, that `else if` is false. The patch changes Windows and non-x86 Unix edge cases, but it provides no Deck-facing behavior and would unnecessarily touch Artemis's heavily customized session and fractional-FPS code. |
| `d17575d4` — make the DRM-master hook lock recursive | Rejected | This addresses re-entrant driver calls while running the Vulkan renderer directly through KMSDRM on AMDGPU. The supported Deck Flatpak presents through Gamescope/Wayland and its WSI layer, not Artemis's direct-display DRM-master handoff. No failure in the supported path is reproduced, so changing global hook locking would expand scope without Deck evidence. |
| `94d47e95` — serialize Vulkan setup with DRM master | Rejected | This is also KMSDRM-only and depends on the DRM-master hook/locker integration. Artemis does not define the upstream `HAVE_DRM_MASTER_HOOKS` contract, and its `plvk.cpp` has diverged significantly for the manifest's pinned FFmpeg/libplacebo versions, including queue-family population, libplacebo queue locking, and FFmpeg's Vulkan proc loader. Importing the patch would either be inert or risk those verified compatibility changes without benefiting Gamescope/Wayland. |
| `efa67fec` — disable VBlank virtualization with dynamic refresh | Rejected | Despite the general title, the patch is entirely under `Q_OS_WIN32` and dynamically calls the Windows DXGI `DXGIDisableVBlankVirtualization()` API. It has no Linux or Steam Deck code path. Fractional refresh on Deck is handled by Artemis's tested refresh parser and session milli-Hz conversion instead. |

## Automated verification

The accepted ports were developed with red/green checks in `packaging/flatpak/tests/test_upstream_ports.py`. These are deliberately narrow source contracts because the affected platform/UI behavior cannot be exercised on this macOS host. They run with the existing manifest contract suite:

```sh
python3 -m unittest discover -s packaging/flatpak/tests -p 'test_*.py' -v
python3 packaging/flatpak/validate-manifest.py \
  packaging/flatpak/com.artemisdesktop.ArtemisDesktopDev.json
git diff --check
```

At audit time the Python suite contains 24 passing tests, including four upstream-port contracts, and the tracked manifest satisfies its validator.

## Remaining validation limits

Neither `qmake6`/`qmake` nor `flatpak-builder`/`flatpak` is installed on the audit host. Consequently, the accepted C++ and QML ports still require the full Linux Flatpak build and offscreen smoke checks in CI. Controller behavior, audio latency, and XWayland fallback also remain hardware/manual acceptance items; this audit does not claim they were exercised on a physical Steam Deck.

## 2026-08-22 addendum: moonlight-common-c bump and input/network latency work

**Audit date:** 2026-08-22
**Audit host:** Windows, no Qt/qmake/Flatpak toolchain; Docker available.

Researched Artemis's Android client and the shared `ClassicOldSong/moonlight-common-c` library (vendored by Artemis Android, Apollo, and this fork alike) for latency/decoding work not yet in Vibertemis. The Qt client's own decode/present pipeline (FFmpeg low-delay flags and slice threading in `ffmpeg.cpp`, zero-copy VAAPI DMA-BUF export in `vaapi.cpp`, and the Vulkan renderer's present-mode selection and `swapchain_depth = 1` in `plvk.cpp`) was reviewed and found to already reflect moonlight-qt's own upstream latency tuning; no changes were made there. The gap was in the vendored `moonlight-common-c` submodule and how the client uses it.

| Change | Result | Rationale and verification |
| --- | --- | --- |
| Bump `moonlight-common-c/moonlight-common-c` submodule from `ad329b24` to fork HEAD `c999436` | Done | Five commits reviewed individually (`git log ad329b24..c999436`): a CGN subnet mask fix, an iOS-only synthesized-IPv6 VPN workaround, a cmake 4.0 compatibility bump, and `LiSendEmptyPayload()`. None touch API surface Vibertemis calls today; re-grepped `app/` for any now-stale references (none found). Same-fork fast-forward, no divergent merge. |
| Call `LiSendEmptyPayload()` as a Wi-Fi-sleep keepalive | Done, `session.cpp`/`session.h` | Upstream's own commit message documents this function as "a workaround for client side wifi sleeps": handhelds power-save their Wi-Fi radio when idle, and waking it injects a latency spike into the next packet. Hooked into the two idle branches of `Session::execInternal()`'s main SDL event loop (`SDL_WaitEventTimeout`/`SDL_PollEvent` fallback), throttled to once per `WIFI_KEEPALIVE_INTERVAL_MS` (3000 ms) via `Session::sendWifiKeepaliveIfNeeded()`, so it fires on a wall-clock cadence independent of whether video frames are actively arriving (unlike the per-second video stats window, which pauses during static/idle scenes — exactly when a keepalive is most needed). |
| Identify Steam Input-routed controllers as `LI_CTYPE_STEAM` | Done, `gamepad.cpp` | Canonical upstream `moonlight-stream/moonlight-common-c` added `LI_CTYPE_STEAM` (`0x04`) on 2026-08-18; it is not yet in the `ClassicOldSong` fork this project vendors. Defined locally with an `#ifndef` guard (becomes a no-op once a future submodule bump adds it upstream) and wired into the `SDL_GameControllerType` → `LI_CTYPE_*` switch: `SDL_CONTROLLER_TYPE_VIRTUAL` with a non-zero `SDL_GameControllerGetSteamHandle()` now maps to `LI_CTYPE_STEAM` instead of falling through to `LI_CTYPE_UNKNOWN`. This is how Steam Deck's built-in controller (and anything else Steam Input manages) reports itself today. Guarded behind `SDL_VERSION_ATLEAST(2, 30, 0)` since `SDL_GameControllerGetSteamHandle()` is a newer SDL2 API. |

### Rejected/deferred candidate

`nanors` SIMD-accelerated Reed-Solomon FEC and LTR-ACK support exist in canonical upstream `moonlight-stream/moonlight-common-c` but not in the `ClassicOldSong` fork Vibertemis vendors (confirmed: the fork still ships `reedsolomon/rs.c`, not `nanors`). This is a real potential latency/CPU win on FEC-heavy connections, but porting it is a multi-commit, invasive rewrite (SIMD dispatch, GFNI runtime detection) that conflicts with this fork's own diverged FEC/small-MTU code (`c86e053 Fix some edge case on small MTU devices using qsv codec`) and needs a real compile-and-test loop against both forks' history. Deferred rather than attempted blind.

### Verification performed

- Read the actual pinned/new commits in both `git log` and diff form rather than trusting commit titles.
- Re-grepped `app/` for symbols touched by the five bumped commits; none found stale.
- Confirmed `SDL_CONTROLLER_TYPE_VIRTUAL` and `SDL_GameControllerGetSteamHandle()` against the vendored SDL2 header (`app/Moonlight.app/Contents/Frameworks/SDL2.framework/.../SDL_gamecontroller.h`) rather than assuming the API shape.
- Confirmed the Flatpak manifest's `artemis` module sources the whole working tree as a local `dir` source (`packaging/flatpak/com.artemisdesktop.ArtemisDesktopDev.json`), so the submodule bump requires no separate manifest pin update; CI already runs `git submodule update --init --recursive` before the build.

### Not verified — same limits as the rest of this audit

No qmake/Qt/Flatpak toolchain on this host means the Qt client changes (`session.cpp`, `session.h`, `gamepad.cpp`) are reviewed by hand against confirmed header signatures, not compiled locally. No physical Steam Deck, Apollo, or Vibepollo host was used to confirm the keepalive reduces observed latency spikes or that a host correctly recognizes `LI_CTYPE_STEAM`. These require the project's own Flatpak CI (`dev-build.yml`) for compile verification and a beta tester for the hardware acceptance matrix, per the process this document already establishes.

## 2026-09-05 addendum: VAAPI RFI opt-in (highest-confidence first streaming improvement)

**Audit date:** 2026-09-05
**Research snapshot:** `research/moonlight-master @ c045ae8986fc7e3b957d5a8b54b487747811525c`
**Upstream commit:** [`d3c23b55dcf14d852d735f59625d803512606b09`][upstream-d3c23b55] — *Disable the VAAPI RFI latency workaround by default* (Cameron Gutman, 2025-11-30)

This is the first streaming-improvement port in this batch because it is the highest-confidence candidate: a single-file source change with no new renderers, no surface-allocation rework, and the upstream author documents that they could no longer reproduce the bug on Ubuntu 24.04 (even with core22). The accompanying downstream changes here are confined to one new header, one site in `vaapi.cpp`, two test files, one `.pro` registration, and one Python source-contract test.

### What changed in Vibertemis

`app/streaming/video/ffmpeg-renderers/vaapi.cpp` previously computed

```
m_HasRfiLatencyBug = vendorStr.contains("Gallium", Qt::CaseInsensitive)
                  && qgetenv("IGNORE_RFI_LATENCY_BUG") != "1";
```

i.e. the workaround was *on by default* on any Gallium/Mesa driver unless the user explicitly opted out. Upstream's
[`d3c23b55`][upstream-d3c23b55] inverts that to opt-in (`HAS_RFI_LATENCY_BUG == "1"`) and drops the legacy variable
entirely. Vibertemis follows upstream, but routes the policy through a tiny inline helper
`RfiPolicy::workaroundEnabled(vendorString)` in a new `app/streaming/video/ffmpeg-renderers/rfipolicy.h` so the
contract is unit-testable without spinning up a VAAPI display.

Net effect:

- Modern AMD VAAPI HEVC and AV1 reference-frame invalidation is **advertised by default** via
  `getDecoderCapabilities()` returning `CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
  CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1`.
- VAAPI is no longer **deprioritized on the first selection pass** on Gallium drivers in the
  default policy. Other `VAAPIRenderer::initialize()` fallback branches (libva-major==0 on
  pre-2.x stacks; X11+NVDEC; `FORCE_VAAPI=1` override) are untouched; VAAPI is not
  universally preferred, only the historical Gallium opt-out is removed.
- The startup warning now reads `VAAPI RFI latency workaround explicitly enabled via HAS_RFI_LATENCY_BUG=1`
  when the helper returns true. The previous wording falsely claimed a driver defect had been detected; the
  new wording matches what the user actually did.

### Implementation plan

| Step | Change | Contract / edge case |
| --- | --- | --- |
| 1 | New header `app/streaming/video/ffmpeg-renderers/rfipolicy.h` exposing `RfiPolicy::isGalliumDriver(QString)`, `RfiPolicy::workaroundOptedIn()`, and `RfiPolicy::workaroundEnabled(QString)`. All three are `inline`; the header has no `.cpp`. | Helper is the only call site that reads `HAS_RFI_LATENCY_BUG`. Legacy `IGNORE_RFI_LATENCY_BUG` is removed from the call site. |
| 2 | Replace the inline `m_HasRfiLatencyBug` assignment with `m_HasRfiLatencyBug = RfiPolicy::workaroundEnabled(vendorStr);` in `vaapi.cpp`. | Same Gallium case-insensitive match; opt-in condition is now `qgetenv("HAS_RFI_LATENCY_BUG") == "1"` (exact). |
| 3 | Reword the conditional `SDL_LogWarn` to mention `HAS_RFI_LATENCY_BUG=1`. | The capability gate in `getDecoderCapabilities()` is unchanged and still keys off `m_HasRfiLatencyBug`, so HEVC/AV1 RFI capability is suppressed only when the user opted in. |
| 4 | Register `rfipolicy.h` in `app/app.pro` under the existing `libva` block, and add `tests/rfipolicy` to `tests/tests.pro`. | Header is in the build graph; new subdir test runs alongside the existing ones. |
| 5 | QtTest data tests under `tests/rfipolicy/tst_rfipolicy.cpp` exercise the helper directly. | `initTestCase`/`cleanupTestCase` snapshot both `HAS_RFI_LATENCY_BUG` and `IGNORE_RFI_LATENCY_BUG`, restore on exit; rows cover Gallium lowercase / UPPERCASE / mixed case, non-Gallium and empty vendor strings, env values unset / `""` / `0` / `1` / `true` / `01`, and legacy `IGNORE_RFI_LATENCY_BUG` values `0` / `1` / `""` / unset. Helper requires the exact value `"1"`. |
| 6 | Add a narrow production-wiring contract to `packaging/flatpak/tests/test_upstream_ports.py` (`test_rfi_workaround_uses_helper_and_opt_in_env`). | Asserts the header declares the namespace and the `workaroundEnabled` member, the call site is `RfiPolicy::workaroundEnabled(vendorStr)`, legacy `IGNORE_RFI_LATENCY_BUG` is gone from `vaapi.cpp`, the warning string names `HAS_RFI_LATENCY_BUG=1`, the actual capability-gate function body gates on `m_HasRfiLatencyBug`, the header is listed in `app/app.pro` and `tests/rfipolicy/rfipolicy.pro`, and `tests/tests.pro` includes the new subdir. |

### Validation performed

- Full Linux VAAPI/EGL app build by Codex at `/tmp/vibertemis-decoder-build` against Qt 6.10.2, SDL 2.32.10,
  FFmpeg libavcodec 62.11.100. No `libplacebo` is installed in that environment, so the Vulkan / Flatpak
  build path was not exercised there. This is reported as a build-environment limit, not a port deficiency.
- `tests/rfipolicy` builds and runs under `qmake6` from the absolute-source project file (no copy of
  `tst_rfipolicy.cpp`) in `/tmp/vibertemis-rfi-review-tests` with `QT_QPA_PLATFORM=offscreen`; all 32 rows
  across the four data tables pass. The build is deliberately scoped to the helper and its QtTest host;
  `vaapi.cpp` is not re-linked.
- `python3 -m unittest discover -s packaging/flatpak/tests -p 'test_*.py'` (with
  `PYTHONDONTWRITEBYTECODE=1`) passes 122 tests including the new
  `test_rfi_workaround_uses_helper_and_opt_in_env` contract and the six
  new `FlatpakDocumentationRegressionTests` parser cases, alongside the
  existing upstream-port contracts.
- `python3 packaging/flatpak/validate-manifest.py packaging/flatpak/com.artemisdesktop.ArtemisDesktopDev.json`
  still validates the tracked manifest (no manifest changes in this port).
- `git diff --check` is clean.

### Validation limits (explicitly not claimed)

- **No benchmarks, no hardware validation.** This document does not claim reduced latency, fewer stalls, or
  better recovery on a real Deck. The acceptance procedure is recorded in `docs/STEAM_DECK.md` under *VAAPI
  reference-frame invalidation policy > Deck acceptance procedure*; running it on hardware is a beta-tester
  item.
- **No AV1 hardware claim.** The Deck acceptance matrix is HEVC-only. The Flatpak probe only confirms
  VAAPI/Vulkan decoder configuration strings are present, not that Steam Deck hardware accelerates AV1
  decoding or AV1 RFI.
- **No change to the wider decoder subsystem.** `ffmpeg.cpp`, `plvk.cpp`, and `eglimagefactory.cpp` are
  untouched. The vendor `m_HasRfiLatencyBug` member still feeds only the capability gate and the legacy
  VDPAU deprioritization; it does not influence any buffer-pool or retention code.

### Deferred candidates from this audit pass

The same research snapshot surfaced three further improvements that were ranked below RFI for this batch.
They are recorded here so the next review pass has a starting point, not because they are being deferred
silently.

- **Decoder pool-size / retention rework.** Moonlight's relevant work is the `extra_hw_frames` allowance
  for the VAAPI/EGL frame-pool, intertwined with retention and surface-bound handling. Vibertemis's EGL
  renderer already moves the last decoded frame forward (`m_LastFrame` in `eglvid.cpp`), so any upstream
  pool-size change is *additive* here and needs a measured comparison on actual Deck hardware, not a
  straight port. Deferred until a benchmark target is agreed.
- **Audio queue ceiling** ([moonlight-qt#1978][mlqt-1978]). A single reporter measured an SDL-audio discard /
  backpressure cycle on SteamOS 3.8.16; this is not multi-user CPU-starvation evidence. Raising the queue
  ceiling is a one-line change but the right value is host- and CPU-class-dependent, so it needs more
  measurements before any number is committed. Deferred pending measurement data.
- **`nanors` SIMD-accelerated Reed-Solomon FEC.** Already deferred in the 2026-08-22 addendum; this batch
  did not re-verify the fork gap. Porting remains invasive (SIMD dispatch, GFNI runtime detection, conflict
  with `c86e053` small-MTU work) and is not on this batch's critical path.

### Consultation outcome

Agy/Gemini 3.1 Pro (High) gave a conditional implementation signoff contingent on the following named
corrections to this addendum and to `docs/STEAM_DECK.md`:

- Treat the affected renderer as VAAPI/Gallium only; do not describe the Vulkan renderer as affected.
- Remove the speculative claim that current SteamOS is fixed; cite the upstream author's Ubuntu 24.04
  report instead.
- Drop the `flatpak kill` step from the one-shot opt-in; rely on the user's normal Quit path.
- Replace any invented Moonlight `--packet-loss` / `--latency` CLI harness with an external router / network
  impairment note.
- Treat the chosen decoder as evidence, not RFI capability bitmasks, and require the same VAAPI decoder in
  both default and opt-in runs for a direct RFI comparison.
- Distinguish first-pass fallback from second-pass, and avoid implying VAAPI is universally preferred.
- Replace multi-user audio backlog language with a single-reporter SteamOS 3.8.16 cycle measurement, and
  avoid any new benchmarking claims.

All seven named corrections have been applied in this revision. Codex independently verified the
final production policy and the named documentation corrections. Agy/Gemini 3.1 Pro (High) approved
the implementation conditional on those corrections; no production-code blockers were found. Validation
above covers the Linux VAAPI/EGL build, 32 QtTest cases, 122 Python checks, and manifest validation.
Physical Deck testing and a Vulkan/Flatpak build remain unrun.

[upstream-d3c23b55]: https://github.com/moonlight-stream/moonlight-qt/commit/d3c23b55dcf14d852d735f59625d803512606b09
[mlqt-1978]: https://github.com/moonlight-stream/moonlight-qt/issues/1978

## 2026-09-05 addendum: pacer synchronization fix

**Branch:** `fix/pacer-wakeups`.
Headless pacer test target is Linux-only (registered in `tests/tests.pro` under `linux: SUBDIRS += pacer`); Windows/macOS default builds skip it.
Rejected: VAAPI `extra_hw_frames`-only initial-pool-size change. Modern VAAPI pools grow dynamically, so enlarging fixed pools alone is ineffective here.
Accepted fix, in `app/streaming/video/ffmpeg-renderers/pacer/{pacer.h,pacer.cpp}`:
`m_Stopping` is now `std::atomic<bool>`; the destructor acquires `m_FrameQueueLock`, sets the flag and wakes all three conditions under that lock, then unlocks and joins the threads outside the lock, preserving the existing vsync-source delete and frame-queue cleanup order.
`signalVsync()` flips a `m_VsyncPending` bit under the lock.
The async vsync waiter uses a single `QDeadlineTimer(100)` predicate wait that preserves the original 100 ms fallback and consumes the pending bit exactly once.
`handleVsync()` runs the stop guard before any frame handoff and uses a single deadline in the empty-queue wait.
Queue depth, display periods, frame-drop logic, and rendering lifetimes are unchanged.
**Validation command:** `mkdir -p /tmp/vibertemis-pacer-final-clean && cd /tmp/vibertemis-pacer-final-clean && qmake6 /home/ubuntu/vibertemis-wt-surface-budget/tests/pacer/pacer.pro && make` then `timeout 30s env QT_QPA_PLATFORM=offscreen ./tst_pacer`. Same recipe with `QMAKE_CXXFLAGS+=-fsanitize=address -fno-omit-frame-pointer` and `QMAKE_LFLAGS+=-fsanitize=address` plus `ASAN_OPTIONS=detect_leaks=1` in `/tmp/vibertemis-pacer-final-asan` for the leak-clean build. App rebuild at `/tmp/vibertemis-pacer-build` (orchestrator-owned) and `git diff --check` are clean; `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s packaging/flatpak/tests -p 'test_*.py'` passes 122 tests; manifest validator satisfied.
FFmpeg n8.0 references: [vaapi_decode.c](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/vaapi_decode.c), [decode.c](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/decode.c).
No hardware claim. Agy/Gemini 3.1 Pro (High) granted source implementation signoff after the callback comment correction and conditionally approved the four follow-up test corrections (stack-only `FakeRenderer`, `m_RendererAttributes=0` for thread-owning tests, post-join assertion in `waitReturnsFalseAfterStop`, `handleVsync(1000)` with `m_Stopping.load()` check in `handleVsyncEmptyQueueBailsOnStop`); Codex verified the corrections; the orchestrator will independently verify before commit.
