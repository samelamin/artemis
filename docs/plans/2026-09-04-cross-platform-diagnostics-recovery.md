# Cross-platform diagnostics recovery — plan

Branch: `fix/cross-platform-gzip`
Date: 2026-09-04

## Why

CI run 33886143902 on `main` failed on every non-Linux platform while
Linux and Flatpak passed. Two root causes, both localized to the
diagnostics feature added in Wave 4+:

1. `app/backend/diagnosticreporter.cpp` did `#include <zlib.h>` and called
   `deflateInit2`/`deflate`/`deflateEnd`/`deflateBound`. `app/app.pro`
   only added `-lz` under `unix:!macx`, so:
   - macOS arm64: header resolves from the SDK, but nothing links
     → `_deflate`, `_deflateBound`, `_deflateEnd`, `_deflateInit2_`
     undefined symbols.
   - Windows MSVC x64 / ARM64: no zlib at all
     → `fatal error C1083: Cannot open include file: 'zlib.h'`.
2. `app/path.cpp:126` used `QStandardPaths::StateLocation`, which is
   Qt 6.7+. The Raspberry Pi ARM64 image ships older Qt 6, so the
   build broke there with `'StateLocation' is not a member of
   'QStandardPaths'`.

Nothing else in `app/` uses zlib (verified by `grep -rn 'zlib.h\|
deflateInit2\|deflateBound\|-lz' app/`), and `-lz` was added in this
same feature, so dropping zlib entirely is the smaller change.

## Pre-recovery state observed by Codex review

The previous implementation had two CRC32 table typos at indices
121 (`0x29c9c9b8` → `0x29d9c998`) and 127 (`0xc0ba6dad` →
`0xc0ba6cad`). These mismatches corrupted the trailer CRC32, so the
existing end-to-end tests that round-tripped through a separate decoder
were already failing before this recovery work began. Verified
independently by Python:

```python
>>> expected[121]
0x29d9c998
>>> expected[127]
0xc0ba6cad
```

After the fix, every one of the 256 entries matches the reflected
0xEDB88320 polynomial generator.

## Plan

### 1. Replace zlib with `qCompress` + manual framing

`app/backend/diagnosticreporter.cpp` no longer includes `zlib.h`. It
calls `qCompress(input, 9)` to do the deflate work and emits a real
RFC 1952 gzip stream around it:

- 10-byte header
  `1f 8b 08 00 00 00 00 00 00 ff`
  (magic, CM=8, FLG=0, MTIME=0, XFL=0, OS=255 unknown).
  MTIME stays 0 so outputs are byte-deterministic and no host
  timestamp leaks into the uploaded artifact.
- the raw deflate payload extracted from `qCompress`'s zlib framing
  (bytes `[6, size-4)`),
- 4-byte little-endian CRC32 of `input` (IEEE 802.3, polynomial
  0xEDB88320, computed via a file-local `crc32Update` with a constexpr
  table),
- 4-byte little-endian `input.size()` modulo 2^32.

A file-local `crc32Update(quint32, const char *, qsizetype)` helper
computes the trailer CRC. The helper is in the same anonymous
namespace as the production compressor so there is exactly one copy in
the binary; the test never carries its own.

The signature, callers, `DiagnosticReporter::gzipCompress`, and the
`Content-Encoding: gzip` header are unchanged. The function still
returns an empty `QByteArray` on failure so existing callers continue
to handle short output the same way.

### 2. Handle empty input deterministically

`qCompress(empty, 9)` returns an empty `QByteArray` because the
resulting zlib frame is shorter than the 10-byte minimum the
implementation expects. We treat empty input as a contract bug rather
than a production P0 — every report we send is non-empty — but we
still produce a real 20-byte gzip stream for it:

- 10-byte header (same fixed-MTIME shape as above),
- 2-byte deflate payload: a single final fixed-Huffman block carrying
  only the end-of-block symbol (`03 00`),
- 4-byte CRC32 = 0,
- 4-byte ISIZE = 0.

This matches Python's `gzip` reference output for an empty input and
is accepted by every conformant gzip decoder.

### 3. Qt version guard for `StateLocation`

`app/path.cpp` keeps the existing `#elif defined(Q_OS_LINUX)` branch
but wraps the `StateLocation` lookup in
`#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)`. The pre-6.7 fallback
uses `QStandardPaths::AppLocalDataLocation` which is available on
every supported Qt 6. One short comment notes the version split.

### 4. Friend access for direct unit testing

`DiagnosticReporter` declares `friend class DiagnosticReporterTest;`
in `app/backend/diagnosticreporter.h`. The public API surface is
unchanged. The friend lets the test invoke the private static
`gzipCompress()` directly so it can exercise empty input, the canonical
ASCII test vector, all-byte-value binary with NULs, and a large
incompressible input — none of which `sendReport()` could reach
because the report payload is always non-empty JSON.

### 5. Tests refactor

`tests/diagnosticreporter/tst_diagnosticreporter.cpp` no longer:

- `#include <zlib.h>`,
- defines a `gzipInflate()` helper,
- duplicates the CRC32 table,
- links `-lz` from `diagnosticreporter.pro`.

It validates the production stream with the **system `gzip -d -c`
binary** via `QProcess`. A single shared `resolveGzipDecoder()`
helper finds "gzip" (preferred) or "gunzip" on PATH. Tests `QSKIP`
when neither is available so the suite still runs on minimal CI
images; the new compile-sanity CI step on Linux and macOS expects
gzip to be installed and fails the job if it is missing.

The new direct-coverage tests in this block are:

- `gzipCompressProducesValidHeaderAndTrailer` — magic, CM, FLG=0,
  MTIME bytes 4..7 zero, XFL=0, OS=0xff.
- `gzipCompressRoundTripsCanonicalAscii` — "123456789" round-trips
  through system gzip.
- `gzipCompressCarriesCrc32OfInput` — trailer CRC32 = `0xCBF43926`
  (LE), ISIZE = 9; round-trips through system gzip.
- `gzipCompressEmptyInputProducesValidStream` — empty input
  produces exactly 20 bytes; round-trips to empty output.
- `gzipCompressRoundTripsBinaryWithNulsAndAllBytes` — 8 KiB input
  covering all 256 byte values plus injected NUL runs round-trips
  byte-for-byte.
- `gzipCompressRoundTripsLargeIncompressibleInput` — 256 KiB of
  pseudo-random incompressible data round-trips; ISIZE matches.
- `gzipCompressIsDeterministic` — three calls with identical input
  produce identical bytes; MTIME bytes 4..7 are zero.

The eight existing end-to-end tests are preserved verbatim. The two
of them that previously decoded a payload (`truncationPreserves
StructuredHead` and `previewByteIdenticalToUploadedPayload`) now go
through the same `resolveGzipDecoder()` / `runGzipDecoder()` helpers
and `QSKIP` on PATH-less runners.

## Agy plan consultation

The Agy review surfaced three corrections on top of the original
XPLAT-1 spec, all of which are now implemented:

1. **Empty input must produce a valid 20-byte gzip stream** rather
   than being treated as a failure. `qCompress(empty, 9)` does not
   yield a frame of the size `gzipCompressImpl` expects; this is a
   contract bug, not a production P0 because reports are non-empty.
2. **Tests must drive the actual private production compressor**
   through friend access rather than only checking the bytes that
   `sendReport()` uploads. The end-to-end path is non-empty JSON and
   cannot exercise empty input or the "123456789" CRC test vector.
3. **Drop the duplicated `crc32Ieee` table from the test file.**
   The system gzip `-d -c` is the independent oracle; carrying a
   second CRC32 implementation in the test was a maintenance hazard
   that masked the table typos flagged at recovery time.

## Validation contract

The acceptance contract from `XPLAT-1.md` is checked explicitly:

1. `grep -rn 'zlib.h\|deflateInit2\|deflateBound\|-lz' app/`
   → 0 matches in source files. (The macOS .app bundle ships an
   unrelated libcrypto that matches "zlib"; that is out of scope.)
2. Clean out-of-source build from worktree root:
   `mkdir -p build/opencode-app && cd build/opencode-app && \
   qmake6 ../../artemis.pro CONFIG+=release && make -j6`
   → exit 0, `app/artemis` produced.
3. Build and run all six Qt test suites (`refreshrate`,
   `autoupdate`, `virtualdisplay`, `crashhandler`, `logscrubber`,
   `diagnosticreporter`) from a clean `build/opencode-tests`
   directory using `make -j6`. All suites must report 0 failures.
4. `git diff --stat` shows changes to exactly these files:
   - `app/backend/diagnosticreporter.cpp`,
   - `app/backend/diagnosticreporter.h`,
   - `app/app.pro`,
   - `app/path.cpp`,
   - `tests/diagnosticreporter/tst_diagnosticreporter.cpp`,
   - `tests/diagnosticreporter/diagnosticreporter.pro`,
   - `.github/workflows/dev-build.yml`,
   - `docs/plans/2026-09-04-cross-platform-diagnostics-recovery.md`.
   `.opencode-driver/`, `diff.patch`, and any tracked build artifacts
   are out of commit scope.

## Non-goals

- No public API changes to `DiagnosticReporter`.
- No redesign of the diagnostics feature beyond the gzip change.
- No zlib re-introduction on any platform.
- No commit, push, deploy, or external-system call from this branch.
- No change to the log scrubber, reporter wave history, or upload
  endpoint.
- No claim of cross-platform CI green: the Linux+macOS compile-sanity
  extension added in this branch will report its first results on the
  next CI run. The validation above covers local Linux only.