#!/usr/bin/env python3
"""Source-contract regression for Session::startConnectionAsync().

Asserts the production RTSP session-URL handoff wiring retains all
invariants required for the Steam Deck startup fix. Rejects the
original (pre-fix) app/streaming/session.cpp.

The original file is expected to be materialised separately via:
    git show e4ec9a05:app/streaming/session.cpp > build/original-session.cpp

Usage:
    check_contract.py <session.cpp>
Exit 0 = all invariants satisfied, exit 1 = violation listed on stderr.
"""
import sys


def strip_comments(text):
    """Strip // line comments and /* block */ comments while keeping
    newlines so byte indices remain roughly comparable."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if c == '/' and nxt == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif c == '/' and nxt == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"':
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def check_contract(text):
    fails = []
    src = strip_comments(text)

    # I1: LiInitializeServerInformation(&hostInfo) must be invoked BEFORE
    # any assignment to hostInfo fields. Search AFTER the hostInfo decl
    # (not after init) so moving init down the file actually trips the
    # check. We compare against the first hostInfo.address assignment,
    # the first hostInfo.rtspSessionUrl assignment, and the first
    # LiStartConnection call.
    decl = src.find('SERVER_INFORMATION hostInfo;')
    init = src.find('LiInitializeServerInformation(&hostInfo)',
                    decl if decl >= 0 else 0)
    if decl < 0:
        fails.append('I1: SERVER_INFORMATION hostInfo; declaration missing')
    elif init < 0:
        fails.append('I1: LiInitializeServerInformation(&hostInfo) missing')
    else:
        addr_asgn = src.find('hostInfo.address', decl)
        rtsp_asgn = src.find('hostInfo.rtspSessionUrl', decl)
        li_start = src.find('LiStartConnection(', decl)
        if addr_asgn >= 0 and init > addr_asgn:
            fails.append('I1: hostInfo.address assigned before zero-init')
        if rtsp_asgn >= 0 and init > rtsp_asgn:
            fails.append('I1: hostInfo.rtspSessionUrl assigned before zero-init')
        if li_start >= 0 and init > li_start:
            fails.append('I1: LiStartConnection() called before zero-init')

    # I4: QByteArray rtspSessionUrlStorage; declared in function scope,
    # BEFORE the operations struct init.
    ops_open = src.find('ConnectionRetryOperations operations')
    storage_decl = src.find('QByteArray rtspSessionUrlStorage;')
    if storage_decl < 0:
        fails.append('I4: QByteArray rtspSessionUrlStorage; declaration missing')
    elif ops_open >= 0 and storage_decl > ops_open:
        fails.append('I4: QByteArray rtspSessionUrlStorage declared AFTER operations init')

    # Bounded region: between the operations struct init and the
    # callbacks struct init (whichever comes first).
    cb_open = src.find('ConnectionRetryCallbacks callbacks', ops_open if ops_open >= 0 else 0)
    if ops_open < 0 or cb_open < 0:
        fails.append('I2/I3: operations or callbacks init not found')
        return fails
    region = src[ops_open:cb_open]

    # I2: launch lambda body calls http.startApp() AND writes
    # rtspSessionUrlStorage AFTER that call.
    start_app = region.find('http.startApp(')
    storage_write = region.find('rtspSessionUrlStorage =')
    if start_app < 0:
        fails.append('I2: http.startApp() call missing in operations region')
    if storage_write < 0:
        fails.append('I2: rtspSessionUrlStorage assignment missing in operations region')
    if start_app >= 0 and storage_write >= 0 and storage_write < start_app:
        fails.append('I2: rtspSessionUrlStorage assigned BEFORE http.startApp()')

    # I3: connect lambda body assigns hostInfo.rtspSessionUrl
    # BEFORE invoking LiStartConnection().
    rtsp_asgn = region.find('hostInfo.rtspSessionUrl')
    li_start = region.find('LiStartConnection(')
    if rtsp_asgn < 0:
        fails.append('I3: hostInfo.rtspSessionUrl assignment missing in operations region')
    if li_start < 0:
        fails.append('I3: LiStartConnection() call missing in operations region')
    if rtsp_asgn >= 0 and li_start >= 0 and rtsp_asgn > li_start:
        fails.append('I3: hostInfo.rtspSessionUrl assigned AFTER LiStartConnection()')

    return fails


def main():
    if len(sys.argv) < 2:
        sys.stderr.write('usage: check_contract.py <session.cpp>\n')
        return 2
    path = sys.argv[1]
    try:
        with open(path, 'r') as f:
            text = f.read()
    except IOError as e:
        sys.stderr.write('cannot open %s: %s\n' % (path, e))
        return 2
    fails = check_contract(text)
    if fails:
        sys.stderr.write('CONTRACT FAILED for %s (%d violation(s)):\n'
                         % (path, len(fails)))
        for fail in fails:
            sys.stderr.write('  - %s\n' % fail)
        return 1
    sys.stderr.write('CONTRACT PASSED for %s\n' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
