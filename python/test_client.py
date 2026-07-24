import email
import email.policy
import json
import unittest
from unittest import mock

from zed_pkg_client import (
    PackageMetadata,
    PublishResponse,
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


if __name__ == "__main__":
    unittest.main()
