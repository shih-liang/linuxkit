#!/usr/bin/env python3
"""Invalid handoffs must fail before creating or changing the target root."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
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
        result = subprocess.run(
            [binary, "directory"], env={**os.environ, "NP_TARGET_ROOT": str(target),
                                       "NP_SOURCE_PATH": str(source)},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        assert result.returncode != 0, (index, result.stderr.decode())
        assert not target.exists(), index
        assert b"fixture-password" not in result.stdout + result.stderr
print("installer preflight: malformed plans, NULs, unknown options, digest and missing payload rejection PASS")
