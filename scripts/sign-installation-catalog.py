#!/usr/bin/env python3
"""Sign the installation catalog with the release Ed25519 key.

The private key is a base64-encoded 32-byte Ed25519 seed supplied through the
LIGHTHOUSE_CATALOG_SIGNING_KEY Actions secret. The public key is committed and
also embedded in LightHouse.app; verifying the new signature before writing the
envelope prevents an unrelated or mistyped secret from publishing a catalog.
"""

import base64
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"sign-installation-catalog: {message}")


if len(sys.argv) != 4:
    fail("usage: sign-installation-catalog.py CATALOG PUBLIC_KEY OUTPUT")

catalog_path = pathlib.Path(sys.argv[1])
public_key_path = pathlib.Path(sys.argv[2])
output_path = pathlib.Path(sys.argv[3])
secret = os.environ.get("LIGHTHOUSE_CATALOG_SIGNING_KEY", "").strip()
if not secret:
    fail("LIGHTHOUSE_CATALOG_SIGNING_KEY is not configured")

try:
    private_key = base64.b64decode(secret, validate=True)
    public_key = base64.b64decode(public_key_path.read_text().strip(), validate=True)
except (OSError, ValueError) as error:
    fail(f"cannot decode signing key: {error}")
if len(private_key) != 32 or len(public_key) != 32:
    fail("Ed25519 keys must contain exactly 32 raw bytes")

payload = catalog_path.read_bytes()
private_der = bytes.fromhex("302e020100300506032b657004220420") + private_key
public_der = bytes.fromhex("302a300506032b6570032100") + public_key

with tempfile.TemporaryDirectory(prefix="lighthouse-catalog-sign-") as temporary:
    temporary_path = pathlib.Path(temporary)
    private_path = temporary_path / "private.der"
    public_path = temporary_path / "public.der"
    signature_path = temporary_path / "signature"
    private_path.write_bytes(private_der)
    os.chmod(private_path, 0o600)
    public_path.write_bytes(public_der)
    subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-keyform", "DER",
         "-inkey", str(private_path), "-in", str(catalog_path),
         "-out", str(signature_path)],
        check=True,
    )
    subprocess.run(
        ["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
         "-keyform", "DER", "-inkey", str(public_path),
         "-in", str(catalog_path), "-sigfile", str(signature_path)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    envelope = {
        "keyID": "installation-2026-01",
        "payload": base64.b64encode(payload).decode("ascii"),
        "signature": base64.b64encode(signature_path.read_bytes()).decode("ascii"),
    }

output_path.parent.mkdir(parents=True, exist_ok=True)
temporary_output = output_path.with_suffix(output_path.suffix + ".new")
temporary_output.write_text(
    json.dumps(envelope, indent=2, sort_keys=True, separators=(",", ": ")) + "\n"
)
os.replace(temporary_output, output_path)
