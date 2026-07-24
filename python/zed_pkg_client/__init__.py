"""Python SDK for the zed-pkg registry. Stdlib only (urllib), dataclasses
mirroring the JSON Schemas in zed-interfaces/schemas/."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field, fields
from typing import Any, Optional

DEFAULT_REGISTRY_URL = "https://registry.zpkg.tech"

USER_AGENT = "zed-client-python/0.1.0"

# Bounds every request (connect + read), in seconds.
DEFAULT_TIMEOUT = 30.0

# Hard ceiling on artifact downloads, matching the server's MAX_ARTIFACT_BYTES
# default (100 MiB); plus the slack added to a version's declared size.
MAX_ARTIFACT_BYTES = 100 * 1024 * 1024
_DOWNLOAD_SLACK = 1024 * 1024


def _is_loopback_host(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _download_limit(size: int) -> int:
    """The declared size (when sane) plus slack, capped by the ceiling."""
    if size and size > 0:
        return min(size + _DOWNLOAD_SLACK, MAX_ARTIFACT_BYTES)
    return MAX_ARTIFACT_BYTES


class ZedApiError(Exception):
    """Registry error carrying the stable ApiError code."""

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(f"registry error {status}: {code}: {message}")
        self.status = status
        self.code = code
        self.message = message


def _quote(segment: str) -> str:
    """Percent-encode one path segment (everything reserved, including "/")."""
    return urllib.parse.quote(segment, safe="")


def package_path(org: str, name: str) -> str:
    return f"/v1/packages/{_quote(org)}/{_quote(name)}"


def version_path(org: str, name: str, version: str) -> str:
    return f"/v1/packages/{_quote(org)}/{_quote(name)}/versions/{_quote(version)}"


def artifact_path(sha256: str) -> str:
    return f"/v1/artifacts/{_quote(sha256)}"


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
    version_scheme: str = "semver"


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


@dataclass
class PublishResponse:
    org: str
    name: str
    version: str
    sha256: str


def _from_wire(cls: type, data: dict) -> Any:
    """Build dataclass ``cls`` from a wire dict, ignoring unknown server
    fields so newly added contract fields never crash older clients."""
    known = {f.name for f in fields(cls)}
    return cls(**{key: value for key, value in data.items() if key in known})


def _api_error(error: urllib.error.HTTPError) -> ZedApiError:
    """Map an HTTPError to a ZedApiError carrying the registry code."""
    raw = error.read().decode()
    try:
        parsed = json.loads(raw)
        code = parsed.get("code", "unknown")
        message = parsed.get("message", raw)
    except (ValueError, AttributeError):
        code, message = "unknown", raw
    return ZedApiError(error.code, code, message)


class ZedClient:
    def __init__(
        self,
        registry_url: str = DEFAULT_REGISTRY_URL,
        token: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self.base = registry_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _allowed_download_url(self, raw: str) -> str:
        """Enforce the download-url scheme policy: https is always allowed; http
        only for loopback hosts or when the registry base is itself http. A
        malicious registry response must not redirect fetches to plaintext or
        unexpected hosts."""
        parsed = urllib.parse.urlparse(raw)
        scheme = parsed.scheme
        loopback = _is_loopback_host(parsed.hostname or "")
        if scheme == "https":
            return raw
        if scheme == "http" and (loopback or self.base.startswith("http://")):
            return raw
        raise ZedApiError(
            0,
            "insecure_download_url",
            f"refusing artifact download over {scheme!r} from {raw} "
            "(https required for non-local registries)",
        )

    def _headers(self) -> dict:
        headers = {"user-agent": USER_AGENT}
        if self.token:
            headers["authorization"] = f"Bearer {self.token}"
        return headers

    def _request(
        self,
        path: str,
        method: str = "GET",
        body: Any = None,
        data: Optional[bytes] = None,
        content_type: Optional[str] = None,
    ) -> Any:
        url = f"{self.base}{path}"
        headers = self._headers()
        if body is not None:
            data = json.dumps(body).encode()
            headers["content-type"] = "application/json"
        elif content_type is not None:
            headers["content-type"] = content_type
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            raise _api_error(error) from None

    def get_package(self, org: str, name: str) -> PackageMetadata:
        return _from_wire(PackageMetadata, self._request(package_path(org, name)))

    def get_version(self, org: str, name: str, version: str) -> VersionMetadata:
        return _from_wire(VersionMetadata, self._request(version_path(org, name, version)))

    def search(self, query: str) -> list[PackageSummary]:
        data = self._request(f"/v1/search?q={urllib.parse.quote(query)}")
        return [_from_wire(PackageSummary, item) for item in data.get("items", [])]

    def claim_org(self, slug: str) -> dict:
        return self._request("/v1/orgs", method="POST", body={"slug": slug})

    def download_artifact(self, version: VersionMetadata, dest_path: str) -> None:
        """Download and sha256-verify an artifact."""
        url = version.download_url
        # An absolute url (any scheme) must clear the scheme/host policy; a bare
        # path is resolved against the trusted registry base.
        if "://" in url:
            url = self._allowed_download_url(url)
        else:
            url = f"{self.base}{artifact_path(version.sha256)}"
        # Deliberately no auth header: download_url may point at a third-party
        # host (e.g. a presigned S3/R2 url), and sending the bearer token there
        # would leak it. Only a user-agent is sent.
        request = urllib.request.Request(url, headers={"user-agent": USER_AGENT})
        limit = _download_limit(version.size)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                declared = response.headers.get("Content-Length")
                if declared is not None:
                    try:
                        if int(declared) > limit:
                            raise ZedApiError(
                                0, "artifact_too_large", f"artifact exceeded {limit} bytes; refusing"
                            )
                    except ValueError:
                        pass
                # Read one extra byte so an over-limit body is detected without
                # buffering the whole thing.
                payload = response.read(limit + 1)
        except urllib.error.HTTPError as error:
            raise _api_error(error) from None
        if len(payload) > limit:
            raise ZedApiError(0, "artifact_too_large", f"artifact exceeded {limit} bytes; refusing")
        actual = hashlib.sha256(payload).hexdigest()
        if actual != version.sha256:
            raise ZedApiError(0, "sha256_mismatch", f"expected {version.sha256}, got {actual}")
        with open(dest_path, "wb") as handle:
            handle.write(payload)

    def publish(self, org: str, name: str, version: str, meta: Any, artifact: bytes) -> PublishResponse:
        """Publish a version: multipart PUT with a ``meta`` field (PublishMeta
        JSON) and an ``artifact`` field (archive bytes). Requires a bearer
        token."""
        meta_json = meta if isinstance(meta, str) else json.dumps(meta)
        boundary = uuid.uuid4().hex
        body = b"".join(
            [
                (
                    f"--{boundary}\r\n"
                    'Content-Disposition: form-data; name="meta"\r\n'
                    "\r\n"
                    f"{meta_json}\r\n"
                ).encode(),
                (
                    f"--{boundary}\r\n"
                    'Content-Disposition: form-data; name="artifact"; filename="artifact.tar.gz"\r\n'
                    "Content-Type: application/octet-stream\r\n"
                    "\r\n"
                ).encode()
                + artifact
                + b"\r\n",
                f"--{boundary}--\r\n".encode(),
            ]
        )
        data = self._request(
            version_path(org, name, version),
            method="PUT",
            data=body,
            content_type=f"multipart/form-data; boundary={boundary}",
        )
        return _from_wire(PublishResponse, data)
