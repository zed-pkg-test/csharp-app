//! Rust SDK for the zed-pkg registry. Reuses the DTOs and URL helpers from
//! `zed-interfaces`, so it cannot drift from the contract.

use std::fs;
use std::io::Read;
use std::path::Path;

use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
use sha2::{Digest, Sha256};
use zed_interfaces::registry::{
    self, ApiError, ClaimOrgRequest, ClaimOrgResponse, PackageMetadata, PublishMeta,
    PublishResponse, SearchResponse, VersionMetadata,
};

/// Path-segment encoding: keep RFC 3986 unreserved characters, escape
/// everything else (including `/`). Org and name are slugs today, but opaque
/// version tags can contain arbitrary characters that must not break out of
/// their URL segment.
const SEGMENT: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'.')
    .remove(b'_')
    .remove(b'~');

fn encode_segment(segment: &str) -> String {
    utf8_percent_encode(segment, SEGMENT).to_string()
}

/// Bounds every request (connect + read).
const DEFAULT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);

/// Hard ceiling on artifact downloads, matching the server's
/// `MAX_ARTIFACT_BYTES` default (100 MiB); plus the slack added to a version's
/// declared size.
const MAX_ARTIFACT_BYTES: u64 = 100 * 1024 * 1024;
const DOWNLOAD_SLACK: u64 = 1024 * 1024;

/// The declared size (when sane) plus slack, capped by the ceiling.
fn download_limit(size: u64) -> u64 {
    if size > 0 {
        size.saturating_add(DOWNLOAD_SLACK).min(MAX_ARTIFACT_BYTES)
    } else {
        MAX_ARTIFACT_BYTES
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// The registry answered with a structured error body.
    #[error("registry error {status}: {code}: {message}")]
    Api {
        status: u16,
        code: String,
        message: String,
    },
    #[error("http error: {0}")]
    Http(#[from] reqwest::Error),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("artifact sha256 mismatch: expected {expected}, got {actual}")]
    Sha256Mismatch { expected: String, actual: String },
    #[error("{0}")]
    Other(String),
}

pub type Result<T> = std::result::Result<T, Error>;

pub struct Client {
    base: String,
    token: Option<String>,
    http: reqwest::blocking::Client,
}

impl Client {
    pub fn new(base_url: impl Into<String>) -> Result<Self> {
        Ok(Self {
            base: base_url.into().trim_end_matches('/').to_string(),
            token: None,
            http: reqwest::blocking::Client::builder()
                .user_agent(concat!("zed-client-rust/", env!("CARGO_PKG_VERSION")))
                .timeout(DEFAULT_TIMEOUT)
                .build()?,
        })
    }

    pub fn with_token(mut self, token: impl Into<String>) -> Self {
        self.token = Some(token.into());
        self
    }

    fn url(&self, path: &str) -> String {
        format!("{}{path}", self.base)
    }

    fn check(response: reqwest::blocking::Response) -> Result<reqwest::blocking::Response> {
        if response.status().is_success() {
            return Ok(response);
        }
        let status = response.status().as_u16();
        let body = response.text().unwrap_or_default();
        match serde_json::from_str::<ApiError>(&body) {
            Ok(err) => Err(Error::Api {
                status,
                code: err.code,
                message: err.message,
            }),
            Err(_) => Err(Error::Api {
                status,
                code: "unknown".into(),
                message: body,
            }),
        }
    }

    fn bearer(&self, request: reqwest::blocking::RequestBuilder) -> reqwest::blocking::RequestBuilder {
        match &self.token {
            Some(token) => request.bearer_auth(token),
            None => request,
        }
    }

    pub fn get_package(&self, org: &str, name: &str) -> Result<PackageMetadata> {
        let path = registry::package_path(&encode_segment(org), &encode_segment(name));
        let response = self.http.get(self.url(&path)).send()?;
        Ok(Self::check(response)?.json()?)
    }

    pub fn get_version(&self, org: &str, name: &str, version: &str) -> Result<VersionMetadata> {
        let path = registry::version_path(
            &encode_segment(org),
            &encode_segment(name),
            &encode_segment(version),
        );
        let response = self.http.get(self.url(&path)).send()?;
        Ok(Self::check(response)?.json()?)
    }

    pub fn search(&self, query: &str) -> Result<SearchResponse> {
        let response = self
            .http
            .get(self.url(&registry::search_path()))
            .query(&[("q", query)])
            .send()?;
        Ok(Self::check(response)?.json()?)
    }

    pub fn claim_org(&self, slug: &str) -> Result<ClaimOrgResponse> {
        let request = self.http.post(self.url(&registry::orgs_path())).json(&ClaimOrgRequest {
            slug: slug.to_string(),
        });
        let response = self.bearer(request).send()?;
        Ok(Self::check(response)?.json()?)
    }

    /// Enforce the download-url scheme policy: https is always allowed; http
    /// only for loopback hosts or when the registry base is itself http. A
    /// malicious registry response must not redirect fetches to plaintext or
    /// unexpected hosts.
    fn allowed_download_url(&self, raw: &str) -> Result<reqwest::Url> {
        let url = reqwest::Url::parse(raw)
            .map_err(|e| Error::Other(format!("bad download url {raw}: {e}")))?;
        let loopback = matches!(url.host_str(), Some("localhost"))
            || url
                .host_str()
                .and_then(|h| h.parse::<std::net::IpAddr>().ok())
                .is_some_and(|ip| ip.is_loopback());
        match url.scheme() {
            "https" => Ok(url),
            "http" if loopback || self.base.starts_with("http://") => Ok(url),
            other => Err(Error::Other(format!(
                "refusing artifact download over `{other}` from {raw} \
                 (https required for non-local registries)"
            ))),
        }
    }

    /// Download an artifact to `dest` and verify its sha256.
    pub fn download_artifact(&self, version: &VersionMetadata, dest: &Path) -> Result<()> {
        // An absolute url (any scheme) must clear the scheme/host policy; a
        // bare path is resolved against the trusted registry base. No bearer
        // token is attached: download_url may point at a third-party host
        // (e.g. a presigned S3/R2 url) and the token must not leak there.
        let url = if version.download_url.contains("://") {
            self.allowed_download_url(&version.download_url)?
        } else {
            reqwest::Url::parse(&self.url(&registry::artifact_path(&encode_segment(&version.sha256))))
                .map_err(|e| Error::Other(format!("bad registry url: {e}")))?
        };
        let response = Self::check(self.http.get(url).send()?)?;
        // Bound the read to guard against OOM; read one extra byte so an
        // over-limit body is detected without buffering it whole.
        let limit = download_limit(version.size);
        let mut bytes = Vec::new();
        let mut limited = response.take(limit.saturating_add(1));
        limited.read_to_end(&mut bytes)?;
        if bytes.len() as u64 > limit {
            return Err(Error::Other(format!(
                "artifact exceeded {limit} bytes; refusing"
            )));
        }
        verify_sha256(&bytes, &version.sha256)?;
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(dest, bytes)?;
        Ok(())
    }

    pub fn publish(&self, meta: &PublishMeta, artifact: &Path) -> Result<PublishResponse> {
        let package = &meta.manifest.package;
        let form = reqwest::blocking::multipart::Form::new()
            .text(
                registry::PUBLISH_META_FIELD,
                serde_json::to_string(meta).map_err(|e| Error::Other(e.to_string()))?,
            )
            .file(registry::PUBLISH_ARTIFACT_FIELD, artifact)?;
        let request = self
            .http
            .put(self.url(&registry::version_path(
                &encode_segment(&package.org),
                &encode_segment(&package.name),
                &encode_segment(&package.version),
            )))
            .multipart(form);
        let response = self.bearer(request).send()?;
        Ok(Self::check(response)?.json()?)
    }
}

pub fn verify_sha256(bytes: &[u8], expected: &str) -> Result<()> {
    let actual = hex::encode(Sha256::digest(bytes));
    if actual != expected {
        return Err(Error::Sha256Mismatch {
            expected: expected.to_string(),
            actual,
        });
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha_verification() {
        let bytes = b"zed";
        let good = hex::encode(Sha256::digest(bytes));
        assert!(verify_sha256(bytes, &good).is_ok());
        assert!(matches!(
            verify_sha256(bytes, "00"),
            Err(Error::Sha256Mismatch { .. })
        ));
    }

    #[test]
    fn api_error_mapping() {
        let err: ApiError =
            serde_json::from_str(r#"{"code":"org_taken","message":"claimed"}"#).unwrap();
        assert_eq!(err.code, "org_taken");
    }

    #[test]
    fn path_segments_are_percent_encoded() {
        assert_eq!(encode_segment("1.2.0"), "1.2.0");
        assert_eq!(encode_segment("release candidate/1"), "release%20candidate%2F1");
        assert_eq!(
            registry::version_path(
                &encode_segment("acme"),
                &encode_segment("kit"),
                &encode_segment("v/2?x"),
            ),
            "/v1/packages/acme/kit/versions/v%2F2%3Fx"
        );
    }

    #[test]
    fn base_url_is_trimmed() {
        let client = Client::new("https://registry.zpkg.tech/").unwrap();
        assert_eq!(
            client.url(&registry::package_path("acme", "kit")),
            "https://registry.zpkg.tech/v1/packages/acme/kit"
        );
    }

    #[test]
    fn download_limit_caps_at_ceiling() {
        assert_eq!(download_limit(0), MAX_ARTIFACT_BYTES);
        assert_eq!(download_limit(10), 10 + DOWNLOAD_SLACK);
        assert_eq!(download_limit(MAX_ARTIFACT_BYTES), MAX_ARTIFACT_BYTES);
    }

    #[test]
    fn insecure_download_urls_are_rejected() {
        let client = Client::new("https://registry.zpkg.tech").unwrap();
        for raw in ["http://evil.example/artifact", "file:///etc/passwd"] {
            let err = client.allowed_download_url(raw).unwrap_err();
            assert!(
                matches!(&err, Error::Other(m) if m.contains("refusing")),
                "expected refusal for {raw}, got {err:?}"
            );
        }
    }

    #[test]
    fn loopback_http_download_url_is_allowed() {
        let client = Client::new("https://registry.zpkg.tech").unwrap();
        assert!(client.allowed_download_url("http://127.0.0.1:8080/a").is_ok());
        assert!(client.allowed_download_url("http://localhost/a").is_ok());
        assert!(client.allowed_download_url("https://cdn.example/a").is_ok());
    }
}

#[cfg(test)]
mod download_tests {
    use super::*;
    use std::io::Write as _;
    use std::net::TcpListener;
    use std::sync::mpsc;

    fn version(url: &str, sha256: &str, size: u64) -> VersionMetadata {
        VersionMetadata {
            org: "acme".into(),
            name: "kit".into(),
            version: "1.2.0".into(),
            sha256: sha256.into(),
            size,
            format: Default::default(),
            vcs_tag: "v1.2.0".into(),
            vcs_commit: None,
            download_url: url.into(),
            published_at: "2024-01-01T00:00:00Z".into(),
            yanked: false,
        }
    }

    /// One-shot localhost server returning `body`; the captured raw request
    /// (headers included) comes back over the channel.
    fn spawn_server(body: Vec<u8>) -> (String, mpsc::Receiver<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buf = [0u8; 4096];
                let mut req = Vec::new();
                loop {
                    let n = stream.read(&mut buf).unwrap_or(0);
                    if n == 0 {
                        break;
                    }
                    req.extend_from_slice(&buf[..n]);
                    if req.windows(4).any(|w| w == b"\r\n\r\n") {
                        break;
                    }
                }
                let _ = tx.send(String::from_utf8_lossy(&req).into_owned());
                let header = format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = stream.write_all(header.as_bytes());
                let _ = stream.write_all(&body);
                let _ = stream.flush();
            }
        });
        (format!("http://{addr}"), rx)
    }

    #[test]
    fn download_omits_auth_and_verifies_sha() {
        let body = b"artifact-bytes".to_vec();
        let sha = hex::encode(Sha256::digest(&body));
        let (base, rx) = spawn_server(body.clone());
        let client = Client::new(&base).unwrap().with_token("zpkg_t");
        let dir =
            std::env::temp_dir().join(format!("zed-dl-{}-{:?}", std::process::id(), std::thread::current().id()));
        let dest = dir.join("artifact.tar.gz");
        let v = version(&format!("{base}/artifact"), &sha, body.len() as u64);
        client.download_artifact(&v, &dest).unwrap();
        let req = rx.recv().unwrap();
        assert!(
            !req.to_lowercase().contains("authorization"),
            "bearer token leaked to download host: {req}"
        );
        assert_eq!(std::fs::read(&dest).unwrap(), body);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn download_rejects_oversize_body() {
        let limit = download_limit(1);
        let body = vec![0u8; (limit + 64) as usize];
        let (base, _rx) = spawn_server(body);
        let client = Client::new(&base).unwrap();
        let dir = std::env::temp_dir().join(format!("zed-dl-big-{}", std::process::id()));
        let dest = dir.join("artifact.tar.gz");
        let v = version(&format!("{base}/artifact"), "deadbeef", 1);
        let err = client.download_artifact(&v, &dest).unwrap_err();
        assert!(
            matches!(&err, Error::Other(m) if m.contains("exceeded")),
            "expected size-cap error, got {err:?}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
