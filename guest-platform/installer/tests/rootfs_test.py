#!/usr/bin/env python3
"""Exercise real archive extraction, including malicious and multi-layer input."""
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest

EXTRACTOR = pathlib.Path(sys.argv.pop(1)).resolve()


def tar(entries):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as out:
        for name, value in entries:
            entry = tarfile.TarInfo(name)
            entry.mode = 0o755 if name.endswith("/") else 0o644
            if isinstance(value, tuple):
                entry.type, entry.linkname = value
                out.addfile(entry)
            elif name.endswith("/"):
                entry.type = tarfile.DIRTYPE
                out.addfile(entry)
            else:
                entry.size = len(value)
                out.addfile(entry, io.BytesIO(value))
    return data.getvalue()


def oci(layers, arch="arm64", tamper=False, missing=False):
    files = []

    def blob(data, media):
        digest = hashlib.sha256(data).hexdigest()
        files.append(("blobs/sha256/" + digest, data))
        return {"mediaType": media, "digest": "sha256:" + digest, "size": len(data)}

    config = blob(json.dumps({"architecture": arch, "os": "linux"}).encode(),
                  "application/vnd.oci.image.config.v1+json")
    manifest = {"schemaVersion": 2, "config": config,
                "layers": [blob(tar(x), "application/vnd.oci.image.layer.v1.tar+gzip") for x in layers]}
    descriptor = blob(json.dumps(manifest).encode(), "application/vnd.oci.image.manifest.v1+json")
    if tamper:
        name, data = files[1]
        files[1] = (name, b"X" + data[1:])
    if missing:
        files.pop(1)
    files.append(("index.json", json.dumps({"schemaVersion": 2, "manifests": [descriptor]}).encode()))
    return tar(files)


class Extraction(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="np-rootfs-test-")
        self.addCleanup(self.work.cleanup)
        self.path = pathlib.Path(self.work.name)
        self.root = self.path / "root"
        self.root.mkdir()
        self.outside = self.path / "outside"
        self.outside.mkdir()
        (self.outside / "keep").write_text("preserve")

    def extract(self, data, kind="tar", ok=True):
        source = self.path / "source"
        source.write_bytes(data)
        result = subprocess.run([EXTRACTOR, source, self.root, kind, "arm64"], capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode())
        self.assertEqual((self.outside / "keep").read_text(), "preserve")
        self.assertFalse((self.path / "escaped").exists())

    def test_plain_rootfs_preserves_links(self):
        self.extract(tar([("usr/bin/test", b"hello"),
                          ("usr/bin/other", (tarfile.LNKTYPE, "usr/bin/test")),
                          ("bin", (tarfile.SYMTYPE, "usr/bin"))]))
        self.assertEqual((self.root / "bin/other").read_bytes(), b"hello")
        self.assertEqual((self.root / "usr/bin/test").stat().st_ino,
                         (self.root / "usr/bin/other").stat().st_ino)

    def test_parent_traversal(self):
        self.extract(tar([("../escaped", b"bad")]), ok=False)

    def test_absolute_path(self):
        self.extract(tar([(str(self.path / "escaped"), b"bad")]), ok=False)

    def test_symlink_traversal(self):
        self.extract(tar([("escape", (tarfile.SYMTYPE, str(self.outside))),
                          ("escape/keep", b"changed")]), ok=False)

    def test_hardlink_traversal(self):
        self.extract(tar([("bad", (tarfile.LNKTYPE, "../outside/keep"))]), ok=False)

    def test_special_file(self):
        self.extract(tar([("device", (tarfile.CHRTYPE, ""))]), ok=False)

    def test_truncated(self):
        self.extract(tar([("large", b"x" * 100000)])[:80], ok=False)

    def test_arch_prefix(self):
        self.extract(tar([("root.x86_64/", b""), ("root.x86_64/etc/version", b"arch"),
                          ("root.x86_64/etc/link", (tarfile.LNKTYPE, "root.x86_64/etc/version")),
                          ("version", b"2026.09.01"), ("pkglist.x86_64.txt", b"bash 5.3")]), "arch")
        self.assertEqual((self.root / "etc/link").read_bytes(), b"arch")
        self.assertFalse((self.root / "root.x86_64").exists())
        self.assertFalse((self.root / "version").exists())
        self.assertFalse((self.root / "pkglist.x86_64.txt").exists())

    def test_arch_metadata_must_be_small_regular_files(self):
        for value in [(tarfile.SYMTYPE, "root.x86_64/etc/version"),
                      (tarfile.LNKTYPE, "root.x86_64/etc/version"), b"x" * (1024 * 1024 + 1)]:
            self.extract(tar([("version", value)]), "arch", ok=False)

    def test_arch_unexpected_sibling(self):
        self.extract(tar([("root.x86_64/etc/version", b"arch"), ("etc/extra", b"bad")]), "arch", ok=False)

    def test_arch_wrong_prefix(self):
        self.extract(tar([("root.aarch64/etc/version", b"wrong")]), "arch", ok=False)

    def test_oci_layer_order_and_whiteouts(self):
        self.extract(oci([
            [("etc/removed", b"old"), ("etc/kept", b"yes"), ("var/lib/old", b"gone")],
            [("var/lib/new", b"new"), ("etc/.wh.removed", b""), ("var/lib/.wh..wh..opq", b"")],
            [("etc/kept", b"updated")],
        ]), "oci")
        self.assertFalse((self.root / "etc/removed").exists())
        self.assertFalse((self.root / "var/lib/old").exists())
        self.assertEqual((self.root / "var/lib/new").read_bytes(), b"new")
        self.assertEqual((self.root / "etc/kept").read_bytes(), b"updated")

    def test_oci_whiteout_parent_symlink(self):
        self.extract(oci([[("unsafe", (tarfile.SYMTYPE, str(self.outside)))],
                          [("unsafe/.wh.keep", b"")]]), "oci", ok=False)

    def test_oci_wrong_architecture(self):
        self.extract(oci([[('etc/test', b'hello')]], arch="amd64"), "oci", ok=False)

    def test_oci_tampered_blob(self):
        self.extract(oci([[('etc/test', b'hello')]], tamper=True), "oci", ok=False)

    def test_oci_missing_blob(self):
        self.extract(oci([[('etc/test', b'hello')]], missing=True), "oci", ok=False)

    def test_oci_missing_index(self):
        self.extract(tar([('rootfs', b'unreferenced')]), "oci", ok=False)


unittest.main()
