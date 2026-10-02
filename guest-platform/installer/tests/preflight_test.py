#!/usr/bin/env python3
"""Invalid handoffs must fail before creating or changing the target root."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import shutil
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="nativepipe-preflight-") as folder:
    payload = Path(folder) / "payload"
    payload.mkdir()
    source = payload / "source"
    source.write_bytes(b"not an archive; no case should reach extraction")
    (payload / "account").write_text("installtest\nfixture-password\n")
    plan = dict(installationID="a" * 32, distribution="alpine", architecture="arm64", format="tar",
                software=[], sourceChecksum=dict(
                    algorithm="sha256", value=hashlib.sha256(source.read_bytes()).hexdigest()))
    cases = [
        json.dumps({**plan, "architecture": "arm64\0amd64"}),
        json.dumps({**plan, "architecture": "riscv64"}),
        json.dumps({**plan, "distribution": "archlinux", "architecture": "arm64", "format": "arch-bootstrap"}),
        json.dumps({**plan, "distribution": "archlinux-arm", "architecture": "amd64"}),
        json.dumps({**plan, "distribution": "archlinux", "architecture": "amd64", "format": "tar"}),
        json.dumps({**plan, "format": "arch-bootstrap"}),
        json.dumps({**plan, "distribution": "fedora", "format": "tar"}),
        json.dumps({**plan, "software": ["wine"]}),
        json.dumps({**plan, "software": ["steam"]}),
        json.dumps({**plan, "software": ["amd64-rootfs"]}),
        json.dumps({**plan, "software": ["chatgpt-desktop"]}),
        json.dumps({**plan, "software": ["claude-desktop"]}),
        json.dumps({**plan, "software": ["claudecode", "claude-code"]}),
        json.dumps({**plan, "software": ["codex", "curl | sh"]}),
        json.dumps(plan) + "{}",
        json.dumps({**plan, "distribution": "alpine\0ubuntu"}),
        json.dumps({**plan, "software": ["developer-tools\0anything"]}),
        json.dumps({**plan, "software": ["unrecognized-option"]}),
        json.dumps({**plan, "sourceChecksum": dict(algorithm="sha256", value="0" * 64)}),
        json.dumps(plan),  # required guest artifacts are absent
    ]
    for index, document in enumerate(cases):
        target = Path(folder) / f"target-{index}"
        (payload / "install.json").write_text(document)
        read_fd, write_fd = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
        result = subprocess.run(
            [binary, "directory"], env={**os.environ, "NP_TARGET_ROOT": str(target),
                                       "NP_SOURCE_PATH": str(source), "NP_INSTALL_ERROR_FD": str(write_fd)},
            pass_fds=(write_fd,),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        os.close(write_fd)
        report = os.read(read_fd, 4096)
        os.close(read_fd)
        assert result.returncode != 0, (index, result.stderr.decode())
        assert not target.exists(), index
        assert b"fixture-password" not in result.stdout + result.stderr
        assert len(report) >= 512 and len(report) % 512 == 0, (index, result.stderr)
        code, message = struct.unpack("=i508s", report[:512])
        assert code > 0 and message.split(b"\0", 1)[0], (index, report)
        assert b"fixture-password" not in report

    # A genuine Linux network namespace with no external interface must fail
    # before touching the target, and return the precise errno and message.
    (payload / "agent").mkdir()
    shutil.copy2("/bin/busybox" if Path("/bin/busybox").exists() else "/bin/true",
                 payload / "agent/nativepipe-guestd")
    plan["architecture"] = "arm64" if os.uname().machine == "aarch64" else "amd64"
    (payload / "install.json").write_text(json.dumps(plan))
    read_fd, write_fd = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
    target = Path(folder) / "offline-target"
    result = subprocess.run(["unshare", "--net", binary, "directory"],
        env={**os.environ, "NP_TARGET_ROOT": str(target), "NP_SOURCE_PATH": str(source),
             "NP_INSTALL_ERROR_FD": str(write_fd)}, pass_fds=(write_fd,),
        capture_output=True, timeout=30)
    os.close(write_fd)
    report = os.read(read_fd, 4096)
    os.close(read_fd)
    assert result.returncode != 0 and not target.exists(), result.stderr
    code, message = struct.unpack("=i508s", report[:512])
    import errno
    assert code == errno.ENETUNREACH and b"network with DNS" in message, (report, result.stderr)
print("installer preflight: malformed plans, NULs, unknown options, digest and missing payload rejection PASS")
print("installer errors: structured cause, no secrets, offline network before disk writes PASS")
