from __future__ import annotations

import email
import email.policy
import hashlib
import http.server
import json
import os
import tempfile
import threading
import unittest
from unittest import mock

from zed_pkg_client import (
    PackageMetadata,
    PublishResponse,
    VersionMetadata,
    ZedApiError,
    ZedClient,
    artifact_path,
    package_path,
    version_path,
)


class _FakeResponse:
    """Minimal stand-in for the object urllib.request.urlopen returns."""

    def __init__(self, payload: dict):
        self._payload = json.dumps(payload).encode()

    def read(self) -> bytes:
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class UrlBuildingTest(unittest.TestCase):
    def test_paths_match_contract(self):
        self.assertEqual(package_path("acme", "kit"), "/v1/packages/acme/kit")
        self.assertEqual(
            version_path("acme", "kit", "1.2.0"),
            "/v1/packages/acme/kit/versions/1.2.0",
        )
        self.assertEqual(artifact_path("abc"), "/v1/artifacts/abc")

    def test_path_segments_are_percent_encoded(self):
        self.assertEqual(
            version_path("acme", "kit", "release candidate/1"),
            "/v1/packages/acme/kit/versions/release%20candidate%2F1",
        )
        self.assertEqual(package_path("a?b", "c#d"), "/v1/packages/a%3Fb/c%23d")

    def test_base_url_is_trimmed(self):
        client = ZedClient("https://registry.zpkg.tech///")
        self.assertEqual(client.base, "https://registry.zpkg.tech")


class WireDecodingTest(unittest.TestCase):
    def test_unknown_server_fields_are_ignored(self):
        payload = {
            "org": "acme",
            "name": "kit",
            "vcs": "git",
            "repo_url": "https://github.com/acme/kit",
            "versions": ["1.2.0"],
            "version_scheme": "calver",
            "brand_new_server_field": {"nested": True},
        }
        with mock.patch("urllib.request.urlopen", return_value=_FakeResponse(payload)):
            package = ZedClient("https://x.test").get_package("acme", "kit")
        self.assertIsInstance(package, PackageMetadata)
        self.assertEqual(package.version_scheme, "calver")
        self.assertFalse(hasattr(package, "brand_new_server_field"))

    def test_version_scheme_defaults_to_semver(self):
        package = PackageMetadata(org="acme", name="kit", vcs="git", repo_url="x")
        self.assertEqual(package.version_scheme, "semver")


class PublishTest(unittest.TestCase):
    def test_publish_builds_multipart_put(self):
        response = {"org": "acme", "name": "kit", "version": "1.2.0", "sha256": "abc"}
        meta = {"manifest": {"package": {"org": "acme", "name": "kit", "version": "1.2.0"}}}
        client = ZedClient("https://x.test", token="zpkg_t")
        with mock.patch("urllib.request.urlopen", return_value=_FakeResponse(response)) as opened:
            result = client.publish("acme", "kit", "1.2.0", meta, b"\x1f\x8bartifact-bytes")

        self.assertEqual(result, PublishResponse(org="acme", name="kit", version="1.2.0", sha256="abc"))
        request = opened.call_args[0][0]
        self.assertEqual(request.get_method(), "PUT")
        self.assertEqual(request.full_url, "https://x.test/v1/packages/acme/kit/versions/1.2.0")
        self.assertEqual(request.get_header("Authorization"), "Bearer zpkg_t")

        content_type = request.get_header("Content-type")
        self.assertTrue(content_type.startswith("multipart/form-data; boundary="))
        message = email.message_from_bytes(
            f"Content-Type: {content_type}\r\n\r\n".encode() + request.data,
            policy=email.policy.HTTP,
        )
        parts = message.get_payload()
        self.assertEqual(len(parts), 2)

        meta_part, artifact_part = parts
        self.assertEqual(meta_part.get_param("name", header="content-disposition"), "meta")
        self.assertEqual(json.loads(meta_part.get_payload()), meta)
        self.assertEqual(artifact_part.get_param("name", header="content-disposition"), "artifact")
        self.assertEqual(artifact_part.get_filename(), "artifact.tar.gz")
        self.assertEqual(artifact_part.get_content_type(), "application/octet-stream")
        self.assertEqual(artifact_part.get_payload(decode=True), b"\x1f\x8bartifact-bytes")


class _RecordingServer:
    """A one-shot localhost HTTP server that records the last request's headers
    and serves a fixed body, for exercising download_artifact end to end."""

    def __init__(self, body: bytes, content_length: int | None = None):
        self.body = body
        self.content_length = content_length
        self.last_headers: dict = {}
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                server.last_headers = dict(self.headers)
                self.send_response(200)
                length = server.content_length
                if length is None:
                    length = len(server.body)
                self.send_header("Content-Length", str(length))
                self.end_headers()
                try:
                    self.wfile.write(server.body)
                except (BrokenPipeError, ConnectionResetError):
                    # The client caps its read and may close early; ignore.
                    pass

            def log_message(self, *args):
                pass

            def handle_one_request(self):
                try:
                    super().handle_one_request()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self._httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self._httpd.server_address[1]}"
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._httpd.shutdown()
        self._httpd.server_close()
        return False


def _version(url: str, body: bytes, size: int = 0) -> VersionMetadata:
    return VersionMetadata(
        org="acme",
        name="kit",
        version="1.2.0",
        sha256=hashlib.sha256(body).hexdigest(),
        size=size,
        format="tar.gz",
        vcs_tag="v1.2.0",
        download_url=url,
        published_at="2024-01-01T00:00:00Z",
    )


class DownloadArtifactTest(unittest.TestCase):
    def test_no_auth_header_sent_to_download_host(self):
        body = b"artifact-bytes"
        with _RecordingServer(body) as server, tempfile.TemporaryDirectory() as tmp:
            client = ZedClient(server.url, token="zpkg_t")
            version = _version(f"{server.url}/artifact", body, size=len(body))
            dest = os.path.join(tmp, "artifact.tar.gz")
            client.download_artifact(version, dest)
            # The bearer token must never reach the download host.
            self.assertNotIn("authorization", {k.lower() for k in server.last_headers})
            with open(dest, "rb") as handle:
                self.assertEqual(handle.read(), body)

    def test_insecure_download_url_rejected(self):
        client = ZedClient("https://registry.zpkg.tech", token="zpkg_t")
        for url in ("http://evil.example/artifact", "file:///etc/passwd"):
            version = _version(url, b"x")
            with self.assertRaises(ZedApiError) as ctx:
                client.download_artifact(version, "/dev/null")
            self.assertEqual(ctx.exception.code, "insecure_download_url")

    def test_oversize_body_rejected(self):
        # Declared size 1 -> limit = 1 + 1 MiB slack; serve just over it.
        from zed_pkg_client import _download_limit

        limit = _download_limit(1)
        body = b"\0" * (limit + 64)
        with _RecordingServer(body) as server, tempfile.TemporaryDirectory() as tmp:
            client = ZedClient(server.url)
            version = _version(f"{server.url}/artifact", body, size=1)
            with self.assertRaises(ZedApiError) as ctx:
                client.download_artifact(version, os.path.join(tmp, "a"))
            self.assertEqual(ctx.exception.code, "artifact_too_large")

    def test_oversize_content_length_rejected(self):
        # A lying Content-Length larger than the cap is rejected before reading.
        from zed_pkg_client import MAX_ARTIFACT_BYTES

        with _RecordingServer(b"small", content_length=MAX_ARTIFACT_BYTES + 1) as server:
            client = ZedClient(server.url)
            version = _version(f"{server.url}/artifact", b"small")
            with self.assertRaises(ZedApiError) as ctx:
                client.download_artifact(version, "/dev/null")
            self.assertEqual(ctx.exception.code, "artifact_too_large")


if __name__ == "__main__":
    unittest.main()
