"""Python SDK for the zed-pkg registry.

The package is stdlib-only, transports bearer credentials without parsing them,
never retries writes, refuses redirects, bounds response bodies, and verifies
artifact sha256 values before writing bytes to disk.
"""

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
DEFAULT_TIMEOUT = 30.0
MAX_ARTIFACT_BYTES = 100 * 1024 * 1024
MAX_JSON_RESPONSE_BYTES = 16 * 1024 * 1024
MAX_ERROR_BODY_BYTES = 16 * 1024
_DOWNLOAD_SLACK = 1024 * 1024


def _is_loopback_host(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _download_limit(size: int) -> int:
    if size and size > 0:
        return min(size + _DOWNLOAD_SLACK, MAX_ARTIFACT_BYTES)
    return MAX_ARTIFACT_BYTES


class ZedApiError(Exception):
    """Registry failure with a stable code and bounded explicit remote text."""

    def __init__(self, status: int, code: str, registry_message: str) -> None:
        super().__init__(f"registry error {status}: {code}")
        self.status = status
        self.code = code
        self.registry_message = registry_message
        # Compatibility alias for callers that intentionally inspect remote text.
        self.message = registry_message


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


def _quote(segment: str) -> str:
    return urllib.parse.quote(segment, safe="")


def package_path(org: str, name: str) -> str:
    return f"/v1/packages/{_quote(org)}/{_quote(name)}"


def version_path(org: str, name: str, version: str) -> str:
    return f"/v1/packages/{_quote(org)}/{_quote(name)}/versions/{_quote(version)}"


def yank_path(org: str, name: str, version: str) -> str:
    return f"{version_path(org, name, version)}/yank"


def artifact_path(sha256: str) -> str:
    return f"/v1/artifacts/{_quote(sha256)}"


def _normalize_registry_url(raw: str) -> str:
    parsed = urllib.parse.urlsplit(raw.strip())
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(
            "registry_url must be a credential-free absolute HTTP(S) URL "
            "without query or fragment"
        )
    path = parsed.path.rstrip("/")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, path, "", "")).rstrip("/")


def _read_bounded(response: Any, limit: int, *, fail_on_overflow: bool) -> bytes:
    declared = response.headers.get("Content-Length") if hasattr(response, "headers") else None
    if declared is not None:
        try:
            if int(declared) > limit and fail_on_overflow:
                raise ZedApiError(0, "response_too_large", f"response exceeded {limit} bytes")
        except ValueError:
            pass
    payload = response.read(limit + 1)
    if len(payload) > limit:
        if fail_on_overflow:
            raise ZedApiError(0, "response_too_large", f"response exceeded {limit} bytes")
        payload = payload[:limit]
    return payload


def _api_error(error: urllib.error.HTTPError) -> ZedApiError:
    raw_bytes = _read_bounded(error, MAX_ERROR_BODY_BYTES, fail_on_overflow=False)
    raw = raw_bytes.decode(errors="replace")
    code = f"http_{error.code}"
    message = raw
    try:
        parsed = json.loads(raw)
        if isinstance(parsed, dict):
            if isinstance(parsed.get("code"), str):
                code = parsed["code"]
            if isinstance(parsed.get("message"), str):
                message = parsed["message"]
    except ValueError:
        pass
    return ZedApiError(error.code, code, message)


@dataclass
class PackageSummary:
    org: str
    name: str
    description: Optional[str] = None
    latest: Optional[str] = None
    tags: list[str] = field(default_factory=list)


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
    tags: list[str] = field(default_factory=list)


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
class ClaimOrgResponse:
    slug: str
    created: bool


@dataclass
class YankResponse:
    org: str
    name: str
    version: str
    yanked: bool


@dataclass
class PublishResponse:
    org: str
    name: str
    version: str
    sha256: str


def _from_wire(cls: type, data: dict) -> Any:
    known = {item.name for item in fields(cls)}
    return cls(**{key: value for key, value in data.items() if key in known})


class ZedClient:
    def __init__(
        self,
        registry_url: str = DEFAULT_REGISTRY_URL,
        token: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
        opener: Optional[urllib.request.OpenerDirector] = None,
    ) -> None:
        self.base = _normalize_registry_url(registry_url)
        self.token = token.strip() if token and token.strip() else None
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.timeout = timeout
        self._opener = opener or urllib.request.build_opener(_NoRedirectHandler())

    def __repr__(self) -> str:
        return f"ZedClient(base={self.base!r}, token='[REDACTED]')"

    def _allowed_download_url(self, raw: str) -> str:
        parsed = urllib.parse.urlsplit(raw)
        if parsed.username is not None or parsed.password is not None or parsed.fragment:
            raise ZedApiError(0, "bad_download_url", "download URL contains credentials or fragment")
        loopback = _is_loopback_host(parsed.hostname or "")
        if parsed.scheme == "https":
            return raw
        if parsed.scheme == "http" and (loopback or self.base.startswith("http://")):
            return raw
        raise ZedApiError(0, "insecure_download_url", f"refusing download over {parsed.scheme!r}")

    def _headers(self, *, authorized: bool) -> dict[str, str]:
        headers = {"user-agent": USER_AGENT, "accept": "application/json"}
        if authorized and self.token:
            headers["authorization"] = f"Bearer {self.token}"
        return headers

    def _open(self, request: urllib.request.Request):
        return self._opener.open(request, timeout=self.timeout)

    def _request(
        self,
        path: str,
        method: str = "GET",
        body: Any = None,
        data: Optional[bytes] = None,
        content_type: Optional[str] = None,
        *,
        authorized: bool = False,
    ) -> Any:
        headers = self._headers(authorized=authorized)
        if body is not None:
            data = json.dumps(body).encode()
            headers["content-type"] = "application/json"
        elif content_type is not None:
            headers["content-type"] = content_type
        request = urllib.request.Request(
            f"{self.base}{path}",
            data=data,
            method=method,
            headers=headers,
        )
        try:
            with self._open(request) as response:
                payload = _read_bounded(
                    response,
                    MAX_JSON_RESPONSE_BYTES,
                    fail_on_overflow=True,
                )
        except urllib.error.HTTPError as error:
            raise _api_error(error) from None
        try:
            return json.loads(payload.decode())
        except (UnicodeDecodeError, ValueError) as error:
            raise ZedApiError(0, "invalid_response", f"invalid registry JSON: {error}") from None

    def get_package(self, org: str, name: str) -> PackageMetadata:
        return _from_wire(PackageMetadata, self._request(package_path(org, name)))

    def get_version(self, org: str, name: str, version: str) -> VersionMetadata:
        return _from_wire(VersionMetadata, self._request(version_path(org, name, version)))

    def search(self, query: str) -> list[PackageSummary]:
        data = self._request(f"/v1/search?q={urllib.parse.quote(query)}")
        return [_from_wire(PackageSummary, item) for item in data.get("items", [])]

    def claim_org(self, slug: str) -> ClaimOrgResponse:
        return _from_wire(
            ClaimOrgResponse,
            self._request(
                "/v1/orgs",
                method="POST",
                body={"slug": slug},
                authorized=True,
            ),
        )

    def yank(self, org: str, name: str, version: str, yanked: bool) -> YankResponse:
        return _from_wire(
            YankResponse,
            self._request(
                yank_path(org, name, version),
                method="POST",
                body={"yanked": yanked},
                authorized=True,
            ),
        )

    def download_artifact(self, version: VersionMetadata, dest_path: str) -> None:
        raw = version.download_url.strip()
        if not raw:
            url = f"{self.base}{artifact_path(version.sha256)}"
        elif urllib.parse.urlsplit(raw).scheme:
            url = self._allowed_download_url(raw)
        else:
            url = self._allowed_download_url(urllib.parse.urljoin(f"{self.base}/", raw))

        request = urllib.request.Request(url, headers={"user-agent": USER_AGENT})
        limit = _download_limit(version.size)
        try:
            with self._open(request) as response:
                declared = response.headers.get("Content-Length")
                if declared is not None:
                    try:
                        if int(declared) > limit:
                            raise ZedApiError(
                                0,
                                "artifact_too_large",
                                f"artifact exceeded {limit} bytes",
                            )
                    except ValueError:
                        pass
                payload = response.read(limit + 1)
        except urllib.error.HTTPError as error:
            raise _api_error(error) from None
        if len(payload) > limit:
            raise ZedApiError(0, "artifact_too_large", f"artifact exceeded {limit} bytes")
        actual = hashlib.sha256(payload).hexdigest()
        if actual != version.sha256:
            raise ZedApiError(0, "sha256_mismatch", f"expected {version.sha256}, got {actual}")
        with open(dest_path, "wb") as handle:
            handle.write(payload)

    def publish(
        self,
        org: str,
        name: str,
        version: str,
        meta: Any,
        artifact: bytes,
    ) -> PublishResponse:
        meta_json = meta if isinstance(meta, str) else json.dumps(meta)
        boundary = uuid.uuid4().hex
        multipart = b"".join(
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
            data=multipart,
            content_type=f"multipart/form-data; boundary={boundary}",
            authorized=True,
        )
        return _from_wire(PublishResponse, data)
