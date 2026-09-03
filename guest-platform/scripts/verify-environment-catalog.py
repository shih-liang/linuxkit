#!/usr/bin/env python3
"""Verify the exact bundled environment catalog and its Ed25519 envelope."""

import argparse
import base64
import json
from pathlib import Path
import subprocess
import tempfile


ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def decode_base64(value, label):
    if not isinstance(value, str):
        raise SystemExit("%s is not a base64 string" % label)
    try:
        return base64.b64decode(value, validate=True)
    except ValueError as error:
        raise SystemExit("%s is not canonical base64" % label) from error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()

    catalog = (args.resources / "GuestEnvironmentCatalog.json").read_bytes()
    public_key = decode_base64(
        (args.resources / "GuestEnvironmentCatalogPublicKey.txt")
        .read_text(encoding="ascii").strip(),
        "environment catalog public key",
    )
    envelope = json.loads(
        (args.resources / "GuestEnvironmentCatalog.signed.json")
        .read_text(encoding="utf-8")
    )
    payload = decode_base64(envelope.get("payload"), "environment catalog payload")
    signature = decode_base64(
        envelope.get("signature"), "environment catalog signature"
    )
    if payload != catalog:
        raise SystemExit("signed environment catalog payload does not match catalog")
    if len(public_key) != 32 or len(signature) != 64:
        raise SystemExit("environment catalog is not an Ed25519 key/signature pair")

    with tempfile.TemporaryDirectory(prefix="lighthouse-catalog-verify-") as temporary:
        temporary = Path(temporary)
        public_der = temporary / "public.der"
        payload_path = temporary / "payload"
        signature_path = temporary / "signature"
        public_der.write_bytes(ED25519_SPKI_PREFIX + public_key)
        payload_path.write_bytes(payload)
        signature_path.write_bytes(signature)
        result = subprocess.run(
            [
                args.openssl,
                "pkeyutl",
                "-verify",
                "-rawin",
                "-pubin",
                "-keyform",
                "DER",
                "-inkey",
                str(public_der),
                "-in",
                str(payload_path),
                "-sigfile",
                str(signature_path),
            ],
            check=False,
        )
    if result.returncode != 0:
        raise SystemExit("environment catalog Ed25519 signature verification failed")
    print("environment catalog signature: ok")


if __name__ == "__main__":
    main()
