"""Python SDK for the zed-pkg registry. Stdlib only (urllib), dataclasses
mirroring the JSON Schemas in zed-interfaces/schemas/."""

from __future__ import annotations

import hashlib
import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

DEFAULT_REGISTRY_URL = "https://registry.zpkg.tech"


class ZedApiError(Exception):
    """Registry error carrying the stable ApiError code."""

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(f"registry error {status}: {code}: {message}")
        self.status = status
        self.code = code
        self.message = message


def package_path(org: str, name: str) -> str:
    return f"/v1/packages/{org}/{name}"


def version_path(org: str, name: str, version: str) -> str:
    return f"/v1/packages/{org}/{name}/versions/{version}"


def artifact_path(sha256: str) -> str:
    return f"/v1/artifacts/{sha256}"


@dataclass
class PackageSummary:
    org: str
    name: str
    description: Optional[str] = None
    latest: Optional[str] = None


@dataclass
class PackageMetadata:
    org: str
    name: str
    vcs: str
    repo_url: str
    versions: list[str] = field(default_factory=list)
    description: Optional[str] = None
    latest: Optional[str] = None


@dataclass
class VersionMetadata:
    org: str
    name: str
    version: str
    sha256: str
    size: int
    format: str
    vcs_tag: str
    download_url: str
    published_at: str
    yanked: bool = False
    vcs_commit: Optional[str] = None


class ZedClient:
    def __init__(
        self,
        registry_url: str = DEFAULT_REGISTRY_URL,
        token: Optional[str] = None,
    ) -> None:
        self.base = registry_url.rstrip("/")
        self.token = token

    def _request(self, path: str, method: str = "GET", body: Any = None) -> Any:
        url = f"{self.base}{path}"
        data = None
        headers = {"user-agent": "zed-client-python/0.1.0"}
        if self.token:
            headers["authorization"] = f"Bearer {self.token}"
        if body is not None:
            data = json.dumps(body).encode()
            headers["content-type"] = "application/json"
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            raw = error.read().decode()
            try:
                parsed = json.loads(raw)
                raise ZedApiError(error.code, parsed.get("code", "unknown"), parsed.get("message", raw)) from None
            except (ValueError, KeyError):
                raise ZedApiError(error.code, "unknown", raw) from None

    def get_package(self, org: str, name: str) -> PackageMetadata:
        return PackageMetadata(**self._request(package_path(org, name)))

    def get_version(self, org: str, name: str, version: str) -> VersionMetadata:
        return VersionMetadata(**self._request(version_path(org, name, version)))

    def search(self, query: str) -> list[PackageSummary]:
        data = self._request(f"/v1/search?q={urllib.parse.quote(query)}")
        return [PackageSummary(**item) for item in data.get("items", [])]

    def claim_org(self, slug: str) -> dict:
        return self._request("/v1/orgs", method="POST", body={"slug": slug})

    def download_artifact(self, version: VersionMetadata, dest_path: str) -> None:
        """Download and sha256-verify an artifact."""
        url = version.download_url
        if not url.startswith("http"):
            url = f"{self.base}{artifact_path(version.sha256)}"
        with urllib.request.urlopen(url) as response:
            payload = response.read()
        actual = hashlib.sha256(payload).hexdigest()
        if actual != version.sha256:
            raise ZedApiError(0, "sha256_mismatch", f"expected {version.sha256}, got {actual}")
        with open(dest_path, "wb") as handle:
            handle.write(payload)
