#!/usr/bin/env python3
"""Exercise publication idempotency with an in-memory GitHub transport."""

from contextlib import redirect_stderr, redirect_stdout
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release_verifier", Path(__file__).with_name("verify-kernel-release.py"))
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
TAG = "lighthouse-kernel-v6.18.56-1"
SHA = "a" * 40
REPOSITORY = "example/linuxkit"


class ReleaseVerificationTests(unittest.TestCase):
    def setUp(self):
        self.files = {name: name.encode() for name in verifier.payload_names("6.18.56")}
        self.files["SHA256SUMS"] = "".join(
            f"{hashlib.sha256(content).hexdigest()}  {name}\n"
            for name, content in sorted(self.files.items())
        ).encode()
        self.release = {"tag_name": TAG, "draft": False, "assets": []}
        self.refresh_metadata()
        self.commit = {"sha": SHA}
        self.listing = []
        self.responses = {}
        self.download_error = False
        self.downloads = 0
        self.environment = patch.dict(verifier.os.environ, {"GH_REPO": REPOSITORY})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.transport = patch.object(verifier.subprocess, "run", side_effect=self.run_gh)
        self.mock_run = self.transport.start()
        self.addCleanup(self.transport.stop)

    def refresh_metadata(self):
        self.release["assets"] = [{"name": name, "size": len(content), "state": "uploaded"}
                                  for name, content in self.files.items()]

    def run_gh(self, command, **options):
        self.assertEqual(command[0], "gh")
        if command[1] == "api":
            endpoint = command[2].removeprefix(f"repos/{REPOSITORY}/")
            if endpoint in self.responses:
                status, payload = self.responses[endpoint]
            elif endpoint == f"commits/{TAG}":
                status, payload = (200, self.commit) if self.commit else (404, {})
            elif endpoint == f"releases/tags/{TAG}":
                status, payload = (200, self.release) if self.release else (404, {})
            elif endpoint == "releases?per_page=100&page=1":
                status, payload = 200, self.listing
            else:
                self.fail(f"Unexpected API: {endpoint}")
            output = f"HTTP/2.0 {status} Status\r\nContent-Type: application/json\r\n\r\n{json.dumps(payload)}"
            return subprocess.CompletedProcess(command, 0 if status == 200 else 1, output, "")
        self.assertEqual(command[1:3], ["release", "download"])
        self.downloads += 1
        if self.download_error:
            return subprocess.CompletedProcess(command, 1, "", "connection failed")
        destination = Path(command[command.index("--dir") + 1])
        requested = {command[index + 1] for index, argument in enumerate(command) if argument == "--pattern"}
        self.assertEqual(requested, verifier.payload_names("6.18.56") | {"SHA256SUMS"})
        for name in requested:
            if name in self.files:
                (destination / name).write_bytes(self.files[name])
        return subprocess.CompletedProcess(command, 0, "", "")

    def verify(self, **options):
        return verifier.verify(TAG, **options)

    def assert_invalid(self, **options):
        with self.assertRaises(verifier.VerificationError):
            self.verify(**options)

    def test_valid_existing_release(self):
        for asset in self.release["assets"]:
            asset["digest"] = "sha256:" + hashlib.sha256(self.files[asset["name"]]).hexdigest()
        self.assertTrue(self.verify(expected_commit=SHA, require_existing=True))
        self.assertEqual(self.downloads, 1)

    def test_wrong_commit(self):
        self.assert_invalid(expected_commit="b" * 40)
        self.assertEqual(self.downloads, 0)

    def test_missing_release_cannot_bypass_wrong_commit(self):
        self.release = None
        self.assert_invalid(expected_commit="b" * 40)

    def test_draft(self):
        self.release["draft"] = True
        self.assert_invalid()

    def test_draft_hidden_by_tag_lookup(self):
        self.release["draft"] = True
        self.listing = [self.release]
        self.release = None
        self.assert_invalid()

    def test_listed_published_release_is_still_verified(self):
        self.listing = [self.release]
        self.release = None
        self.assertTrue(self.verify(expected_commit=SHA))
        self.assertEqual(self.downloads, 1)

    def test_draft_on_later_listing_page(self):
        self.release["draft"] = True
        self.responses["releases?per_page=100&page=2"] = (200, [self.release])
        self.listing = [{"tag_name": f"unrelated-{index}"} for index in range(100)]
        self.release = None
        self.assert_invalid()

    def test_missing_release_and_tag(self):
        self.release = self.commit = None
        self.assertFalse(self.verify(expected_commit=SHA))
        self.assertEqual(self.downloads, 0)

    def test_missing_release_existing_matching_tag(self):
        self.release = None
        self.assertFalse(self.verify(expected_commit=SHA))

    def test_require_existing(self):
        self.release = None
        self.assert_invalid(require_existing=True)

    def test_release_without_tag(self):
        self.commit = None
        self.assert_invalid()

    def test_auth_and_server_errors_are_not_missing(self):
        for endpoint in [f"commits/{TAG}", f"releases/tags/{TAG}", "releases?per_page=100&page=1"]:
            for status in [403, 500]:
                with self.subTest(endpoint=endpoint, status=status):
                    self.responses = {endpoint: (status, {})}
                    if endpoint.startswith("releases?"):
                        self.responses[f"releases/tags/{TAG}"] = (404, {})
                    self.assert_invalid()

    def test_network_failure_is_not_missing(self):
        self.mock_run.side_effect = None
        self.mock_run.return_value = subprocess.CompletedProcess(["gh"], 1, "", "network unavailable")
        self.assert_invalid()

    def test_null_response_is_not_missing(self):
        self.responses[f"releases/tags/{TAG}"] = (200, None)
        self.assert_invalid()

    def test_incomplete_empty_duplicate_or_unuploaded_assets(self):
        original = copy.deepcopy(self.release["assets"])
        for defect in ["missing", "empty", "duplicate", "unuploaded"]:
            with self.subTest(defect=defect):
                self.release["assets"] = copy.deepcopy(original)
                assets = self.release["assets"]
                if defect == "missing":
                    assets.pop()
                elif defect == "empty":
                    assets[0]["size"] = 0
                elif defect == "duplicate":
                    assets[-1] = copy.deepcopy(assets[0])
                else:
                    assets[0]["state"] = "new"
                self.assert_invalid()
        self.assertEqual(self.downloads, 0)

    def test_duplicate_missing_traversal_and_malformed_checksums(self):
        original = self.files["SHA256SUMS"]
        lines = original.splitlines(keepends=True)
        malformed = [original + lines[0], b"".join(lines[1:]), original + b"a" * 64 + b"  ../outside\n", b"bad checksum\n"]
        for manifest in malformed:
            with self.subTest(manifest=manifest[-30:]):
                self.files["SHA256SUMS"] = manifest
                self.refresh_metadata()
                self.assert_invalid()

    def test_payload_corruption(self):
        name = "config-aarch64"
        self.files[name] = b"x" * len(self.files[name])
        self.assert_invalid()

    def test_size_mismatch(self):
        self.files["config-aarch64"] += b"x"
        self.assert_invalid()

    def test_github_digest_mismatch(self):
        self.release["assets"][0]["digest"] = "sha256:" + "0" * 64
        self.assert_invalid()

    def test_download_failure(self):
        self.download_error = True
        self.assert_invalid()

    def test_cli_stdout_contract(self):
        for state, expected_output, expected_status in [("valid", "true\n", 0), ("missing", "false\n", 0), ("error", "", 1)]:
            with self.subTest(state=state):
                if state == "missing":
                    self.release = None
                if state == "error":
                    self.responses[f"releases/tags/{TAG}"] = (500, {})
                stdout, stderr = io.StringIO(), io.StringIO()
                with redirect_stdout(stdout), redirect_stderr(stderr):
                    status = verifier.main([TAG, "--expected-commit", SHA])
                self.assertEqual(status, expected_status)
                self.assertEqual(stdout.getvalue(), expected_output)
                self.assertEqual(bool(stderr.getvalue()), bool(expected_status))


if __name__ == "__main__":
    unittest.main()
