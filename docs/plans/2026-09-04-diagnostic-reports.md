# Diagnostic reports: capture on Linux, scrub, manual upload

Status: proposed (v3, revised after Codex + Antigravity review)
Date: 2026-09-04

## Problem

We ship a Steam Deck build and have no way to see why it fails on one.

| Capability | Windows | Mac | Linux / Deck |
|---|---|---|---|
| Log written to a file (`app/main.cpp:95-101`) | yes | release only | **no, stderr only** |
| Crash dump (`app/main.cpp:299`) | minidump | no | **no handler at all** |
| Any way for a user to hand us a log | no | no | no |

In Gaming Mode the Flatpak's stderr goes to the journal, which a Deck user will
never retrieve. The HDR/AV1 work in `a6b18a17` shipped with three
hardware-only unknowns and no channel to learn about any of them.

## Scope

No auto-send, no crash grouping, no dashboard, no telemetry. A report leaves
the machine only when the user presses a button, and only after they have seen
its exact contents on screen.

## 0. Storage location (do this first)

`Path::initialize()` sets `s_LogDir = QDir::tempPath()` for everything
non-Darwin (`app/path.cpp:117`). Inside a Flatpak that is a sandbox-private
`/tmp` destroyed when the last instance exits — so today a Deck crash takes its
own log with it, and nothing survives to the next launch to be sent.

Add a Linux branch: `s_LogDir = QStandardPaths::writableLocation(
QStandardPaths::StateLocation)` (under Flatpak, `~/.var/app/<id>/.local/state`,
which persists). `writableLocation()` only computes a path, it does not create
one, and `s_LoggerFile->open()` (`app/main.cpp:405`) fails silently if the
directory is absent — which would disable logging entirely on a fresh install.
`mkpath(".")` before the open is mandatory, not defensive.

Windows and Mac branches are untouched. Export to
`xdg-download` only when the user asks to save a bundle; that permission is
already granted (`packaging/flatpak/com.artemisdesktop.ArtemisDesktopDev.json:22`).

## 1. Capture on Linux

**Log to file.** `LOG_TO_FILE` becomes active on Linux too. Do **not** gate it
on `isatty()`: a non-TTY stderr also means `artemis 2>my.log`, a pipe, SSH, or
systemd, and gating would steal a redirection the user asked for. Instead, on
Linux always write the file **and** keep writing stderr — `LoggerTask::run()`
gains a second sink rather than `s_LoggerStream.setDevice()` replacing the
first. The existing prune-to-10 loop (`app/main.cpp:435-441`) then applies
unchanged.

**Crash tail ring buffer.** Log writes are queued to `s_LoggerThread`
(`app/main.cpp:104`, `:171`) and only drained on clean shutdown
(`app/main.cpp:892`). A fatal signal drains nothing, so the messages explaining
the crash are exactly the ones lost, and the handler must never call
`waitForDone()` — the crashing thread may hold the allocator or stream lock.

So `logToLoggerStream()` also copies each scrubbed message into a static 64 KB
ring buffer before queueing. The crash handler dumps it with `write()`. This is
the only mechanism that gets the crash tail out, and it is why the ring buffer
is in scope while everything else optional is not.

A bare atomic index is **not** enough: concurrent writers interleave and
corrupt each other on wrap, and other threads keep running while the handler
reads, tearing the snapshot. So:

- Writers take a `std::atomic_flag` spinlock around the copy. Held for a
  `memcpy` of a few hundred bytes, so contention is irrelevant.
- The handler first sets a global `g_Crashing` atomic, which makes every other
  thread's writer path return immediately instead of taking the lock.
- The handler then dumps the buffer **without** waiting for the lock. If the
  faulting thread held it, waiting would deadlock, and a best-effort torn
  snapshot beats no snapshot.

**Signal handler** (Linux only, new `app/backend/crashhandler.{cpp,h}`):

- SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, on a `sigaltstack` so a
  stack-overflow SIGSEGV can still run.
- Crash file fd opened at **install** time, not in the handler.
- Call `backtrace()` once at install time to force the libgcc unwinder to load,
  so the in-handler call does not hit the loader lock or first-call allocation.
- In the handler: only `write()`, `backtrace()`, and a copy of
  `/proc/self/maps` (open+read+write, all async-signal-safe).
- **Not** `backtrace_symbols_fd()`. POSIX does not list it as
  async-signal-safe, and glibc's implementation takes `dl_load_lock` to resolve
  module names — a crash during library resolution would deadlock in the
  handler. It also buys nothing here: the frames get resolved offline against
  maps + build-id regardless. Dump raw hex addresses with a hand-rolled
  signal-safe formatter and `write()`.
- Emit signal number, `si_addr`, build version/commit, the frame addresses, the
  maps, then the ring buffer.
- Restore `SIG_DFL` and re-raise, so the OS still does whatever it normally
  does and we cannot loop.

**Frames will be module+offset, not names.** The release Flatpak is stripped
and there is no symbol pipeline. That is fine and is why `/proc/self/maps` and
the build-id are in the file: `addr2line` against the matching unstripped
binary resolves them offline. CI change: archive the unstripped binary and its
build-id alongside each release build. The acceptance test asserts
module+offset frames and a build-id, **not** a symbol name.

Windows keeps its existing minidump path unchanged. Minidumps are **never**
uploaded — they contain arbitrary process memory. Only the text crash file and
the scrubbed log are.

## 2. Privacy model

A denylist of regexes over a free-form log is not sufficient on its own: the
log carries uncontrolled schemas (XML bodies, full URLs) and future log lines
nobody has written yet. So the bundle has two halves and one hard gate.

**Half one — structured, allowlisted.** Explicit keys only, assembled from
known values, never scraped from the log: app version/commit/channel, OS,
kernel, Deck model, GPU, driver, session type, Qt version, Flatpak yes/no,
codec and HDR decisions, error codes, and whether the host was link-local or
routable (the category, not the address).

**Half two — the scrubbed log tail.** Denylist scrubbing, deliberately
aggressive, because a lost diagnostic line is cheaper than a leaked identifier:

| Class | Rule |
|---|---|
| IP addresses | full redact, v4 and v6. A retained prefix still identifies |
| MAC addresses | full redact |
| URL query strings | drop the entire query string, keep scheme/host-shape/path |
| XML/JSON bodies (`serverinfo`, `applist`) | drop the body, keep the element names |
| UUIDs | full redact, any format |
| Client `uniqueid` (`app/backend/identitymanager.cpp:202`) | full redact |
| PC / host names (`app/backend/computermanager.cpp:57,101`) | `<HOST>` |
| Discord username (`app/backend/richpresencemanager.cpp:55`) | `<USER>` |
| Home and config paths | `<HOME>`, `<CONFIG>` |
| PEM blocks, `&rikey`, `&rikeyid`, bearer tokens | full redact |
| Game / app names | `<APP>` — user activity is PII |

Kept: GPU and driver strings, resolutions, codec/HDR decisions, Qt/SDL/FFmpeg
diagnostics, error text.

**The gate — on-device preview.** Before anything is sent, the exact bundle is
shown with **Send** and **Cancel**. Regexes will eventually miss something; a
user who can read what leaves their machine is the backstop scrubbing alone
cannot be.

A 2 MB JSON blob in a scroll view would be theatre — nobody audits 100,000
escaped lines on a 7-inch screen held at arm's length. So the preview is
constrained to be actually readable:

- The structured head renders as formatted key/value rows, not raw JSON.
- The log tail renders as raw text, unescaped.
- The log tail cap drops from 512 KB to **256 KB**, and the preview opens
  scrolled to the end, where the interesting lines are.

If a user cannot plausibly skim it, the gate does not exist, so the cap is a
privacy control and not a bandwidth one.

The existing `k_RikeyRegex` / `k_RikeyIdRegex` replacements stay where they are
as defence in depth for the on-disk log.

## 3. Client: send action

New `app/backend/diagnosticreporter.{cpp,h}`, modelled on
`app/backend/autoupdatechecker.cpp` — same `QNetworkAccessManager` shape, same
explicit connect/idle/overall timeouts, same hard byte limits, same redirect
cap.

Bundle: JSON `{ v, app, sys, log, crash, note }`, gzipped, hard cap 2 MB, log
tail capped at 256 KB before compression. Log tail truncated to fit; the
structured head is never truncated. Optional
280-char user note — included in the preview like everything else.

`PUT https://<worker>/v1/report`, headers `X-Vbt-Ver`,
`Content-Encoding: gzip`. On 200, parse `{"id":"VBT-7QX4M"}` and show the code.
On any failure, offer **Save to Downloads** so a failed upload never loses the
report.

**No build-embedded key.** It would be extractable from a public binary, so it
authenticates nothing; it only adds a CI build-define and the false impression
the endpoint is protected. The size cap and rate limit are the actual controls.

UI: a "Diagnostics" section in `app/gui/SettingsView.qml` — note field, **Send
diagnostic report**, **Save report to Downloads**. Settings text states plainly
what a report contains and that nothing is sent otherwise.

## 4. Worker

New `services/crash-endpoint/` — `wrangler.toml`, `src/index.ts`, tests.

```
PUT /v1/report
  reject: non-PUT, body > 2 MB, > N requests/IP/hour
  store:  reports/<yyyy-mm-dd>/<version>/<id>.gz  -> R2
  return: 200 {"id":"VBT-7QX4M"}
```

- Report id: 8 chars Crockford base32, generated server-side.
- Cloudflare's native rate-limiting binding, not a Durable Object.
- **A global daily budget in KV, not only a per-IP limit.** R2 has no hard
  billing cap, the endpoint is unauthenticated by design, and a per-IP limit is
  defeated by rotating IPs — 5,000 PUTs at the 2 MB cap exhausts the 10 GB free
  tier, and everything past it bills. The Worker keeps a KV counter of bytes
  and requests accepted today and returns 503 once either exceeds its budget.
  Losing reports on a flood day is the correct failure mode; an unbounded bill
  is not.
- R2 lifecycle rule deletes objects after 30 days.
- Deploys to `*.workers.dev`; no custom domain required.
- Bucket is write-only from the internet. Reading is
  `wrangler r2 object get` from a machine holding the account credentials. No
  web UI, no public read path.

## Acceptance contract

- Linux: log file appears in `StateLocation` on every run, and stderr output is
  still produced when stderr is redirected to a file.
- A deliberate SIGSEGV in a test harness produces a crash file containing a
  build-id, module+offset frames, a `/proc/self/maps` copy, and the last log
  lines from the ring buffer — including a line logged microseconds before the
  fault, proving the async logger is bypassed. The process still dies of
  SIGSEGV.
- `tests/logscrubber` (new, added to `tests/tests.pro`): a case per redaction
  row, plus negative cases asserting GPU strings, codec/HDR decisions and error
  text survive, plus a real captured Moonlight session log fixture asserted to
  contain no IP, UUID, hostname or app name after scrubbing.
- `tests/diagnosticreporter`: bundle under cap, truncation preserves the
  structured head, upload failure falls back to file, preview text is byte
  identical to the uploaded payload.
- Worker `vitest`: method rejection, size rejection, per-IP rate limit, global
  daily budget returning 503, id shape, object key shape.
- Crash handler contains no call to `backtrace_symbols_fd`, asserted by grep.
- Log directory is created if missing: test starts with the state dir absent
  and asserts a log file appears.
- `qmake6 && make` clean; existing suites still 43/56/110, 0 failed.
- No new Flatpak permission required.

## Deploy

`naseyma/infra/env/backup.env` holds `CLOUDFLARE_ACCOUNT_ID`,
`CLOUDFLARE_R2_ACCESS_KEY_ID`, `CLOUDFLARE_R2_SECRET_ACCESS_KEY` and
`R2_BUCKET_NAME=naseyma-backups`. Verified working: listing
`s3://naseyma-backups/` succeeds; `ListBuckets` is denied, so the key is
scoped to that one bucket.

That covers **storage** and not **deploy**. R2 S3 keys cannot publish a Worker;
that needs a Cloudflare API token with `Workers Scripts:Edit` and
`Workers R2 Storage:Edit`. So one of:

1. **Preferred** — a CF API token (dashboard, My Profile → API Tokens, two
   minutes). Then `wrangler r2 bucket create vbt-reports && wrangler deploy`.
   Isolated from the naseyma production box, free tier, own budget.
2. Reuse `naseyma-backups` under a `vibertemis-reports/` prefix if the scoped
   key also permits writes there. Still needs the API token for the Worker
   itself.
3. **No new credential at all** — run the ingest service on the existing VPS
   behind the nginx already there (`naseyma/infra/nginx/nginx.conf`), writing
   to R2 with the S3 keys it already holds. Works today, but puts a public
   unauthenticated endpoint on the production box, which option 1 avoids.

Everything else is built and tested regardless; deploy is the last step.

## Review

- **Codex (v1):** blocked on five items — unsafe unwinder, `isatty()` gating
  stealing explicit redirection, ephemeral Flatpak crash location, absent
  symbol pipeline, denylist-only privacy model. Also listed PII classes v1
  missed (serverinfo XML, full request URLs, client uniqueid, Discord
  username, config paths, PC names, app names). All folded into v2: §0 moves
  storage, §1 pre-warms the unwinder and drops the gating, §1 ships maps +
  build-id instead of pretending to symbolicate on device, §2 rebuilds the
  privacy model around an allowlisted head plus a mandatory preview.
  Cuts taken: embedded key, Durable Object, bespoke crash pruning.
  Cut declined: in-process unwinding stays, since maps + build-id make
  module+offset frames resolvable offline at no extra runtime risk.
- **Antigravity (v2):** blocked on three v2-specific items, all folded in — the
  ring buffer needed a spinlock plus a `g_Crashing` gate (a bare atomic index
  corrupts on wrap and tears under the handler), `StateLocation` needs
  `mkpath()` or logging silently dies, and an unauthenticated R2 endpoint needs
  a global daily budget because R2 has no hard billing cap. Also caught that
  `backtrace_symbols_fd()` is not async-signal-safe in glibc, which removes it
  from the handler entirely, and that a 2 MB preview is unreadable on a Deck —
  hence the 256 KB cap and formatted rendering.
