import unittest

from zed_pkg_client import (
    ZedClient,
    artifact_path,
    package_path,
    version_path,
)


class UrlBuildingTest(unittest.TestCase):
    def test_paths_match_contract(self):
        self.assertEqual(package_path("acme", "kit"), "/v1/packages/acme/kit")
        self.assertEqual(
            version_path("acme", "kit", "1.2.0"),
            "/v1/packages/acme/kit/versions/1.2.0",
        )
        self.assertEqual(artifact_path("abc"), "/v1/artifacts/abc")

    def test_base_url_is_trimmed(self):
        client = ZedClient("https://registry.zpkg.tech///")
        self.assertEqual(client.base, "https://registry.zpkg.tech")


if __name__ == "__main__":
    unittest.main()
