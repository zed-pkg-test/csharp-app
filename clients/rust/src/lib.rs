//! Rust SDK for the zed-pkg registry. Reuses the DTOs and URL helpers from
//! `zed-interfaces`, so wire models and paths stay aligned with the contract.

use std::fmt;
use std::fs;
use std::io::Read;
use std::path::Path;

use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
use serde::de::DeserializeOwned;
use sha2::{Digest, Sha256};
use zed_interfaces::registry::{
    self, ApiError, ClaimOrgRequest, ClaimOrgResponse, PackageMetadata, PublishMeta,
    PublishResponse, SearchResponse, VersionMetadata, YankRequest, YankResponse,
};

const SEGMENT: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'.')
    .remove(b'_')
    .remove(b'~');

fn encode_segment(segment: &str) -> String {
    utf8_percent_encode(segment, SEGMENT).to_string()
}

const DEFAULT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
const MAX_ARTIFACT_BYTES: u64 = 100 * 1024 * 1024;
const DOWNLOAD_SLACK: u64 = 1024 * 1024;
pub const DEFAULT_MAX_RESPONSE_BYTES: u64 = 16 * 1024 * 1024;
pub const MAX_ERROR_BODY_BYTES: u64 = 16 * 1024;

fn download_limit(size: u64) -> u64 {
    if size > 0 {
        size.saturating_add(DOWNLOAD_SLACK).min(MAX_ARTIFACT_BYTES)
    } else {
        MAX_ARTIFACT_BYTES
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// `message` is bounded and explicitly inspectable; the Display output
    /// intentionally excludes arbitrary remote text.
    #[error("registry error {status}: {code}")]
    Api {
        status: u16,
        code: String,
        message: String,
    },
    #[error("invalid registry base URL")]
    InvalidBaseUrl,
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
    max_response_bytes: u64,
    http: reqwest::blocking::Client,
}

impl fmt::Debug for Client {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Client")
            .field("base", &self.base)
            .field("token", &self.token.as_ref().map(|_| "[REDACTED]"))
            .field("max_response_bytes", &self.max_response_bytes)
            .finish_non_exhaustive()
    }
}

impl Client {
    pub fn new(base_url: impl Into<String>) -> Result<Self> {
        let raw = base_url.into();
        let mut parsed = reqwest::Url::parse(raw.trim()).map_err(|_| Error::InvalidBaseUrl)?;
        if !matches!(parsed.scheme(), "http" | "https")
            || parsed.host_str().is_none()
            || !parsed.username().is_empty()
            || parsed.password().is_some()
            || parsed.query().is_some()
            || parsed.fragment().is_some()
        {
            return Err(Error::InvalidBaseUrl);
        }
        let path = parsed.path().trim_end_matches('/').to_string();
        parsed.set_path(&path);
        let base = parsed.as_str().trim_end_matches('/').to_string();

        Ok(Self {
            base,
            token: None,
            max_response_bytes: DEFAULT_MAX_RESPONSE_BYTES,
            http: reqwest::blocking::Client::builder()
                .user_agent(concat!("zed-client-rust/", env!("CARGO_PKG_VERSION")))
                .connect_timeout(DEFAULT_TIMEOUT)
                .timeout(DEFAULT_TIMEOUT)
                .redirect(reqwest::redirect::Policy::none())
                .build()?,
        })
    }

    #[must_use]
    pub fn with_token(mut self, token: impl Into<String>) -> Self {
        let token = token.into();
        self.token = (!token.trim().is_empty()).then(|| token.trim().to_string());
        self
    }

    #[must_use]
    pub fn with_max_response_bytes(mut self, limit: u64) -> Self {
        self.max_response_bytes = limit;
        self
    }

    fn url(&self, path: &str) -> String {
        format!("{}{path}", self.base)
    }

    fn read_capped(
        response: reqwest::blocking::Response,
        limit: u64,
        what: &str,
    ) -> Result<Vec<u8>> {
        let too_large = || Error::Other(format!("{what} exceeded {limit} bytes; refusing"));
        if response.content_length().is_some_and(|length| length > limit) {
            return Err(too_large());
        }
        let mut bytes = Vec::new();
        response
            .take(limit.saturating_add(1))
            .read_to_end(&mut bytes)?;
        if bytes.len() as u64 > limit {
            return Err(too_large());
        }
        Ok(bytes)
    }

    fn check(&self, response: reqwest::blocking::Response) -> Result<reqwest::blocking::Response> {
        if response.status().is_success() {
            return Ok(response);
        }
        let status = response.status().as_u16();
        let limit = self.max_response_bytes.min(MAX_ERROR_BODY_BYTES);
        let body = match Self::read_capped(response, limit, "registry error body") {
            Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
            Err(error) => {
                return Err(Error::Api {
                    status,
                    code: format!("http_{status}"),
                    message: format!("<bounded error body unavailable: {error}>"),
                });
            }
        };
        match serde_json::from_str::<ApiError>(&body) {
            Ok(error) => Err(Error::Api {
                status,
                code: error.code,
                message: error.message,
            }),
            Err(_) => Err(Error::Api {
                status,
                code: format!("http_{status}"),
                message: body,
            }),
        }
    }

    fn json<T: DeserializeOwned>(&self, response: reqwest::blocking::Response) -> Result<T> {
        let response = self.check(response)?;
        let bytes = Self::read_capped(response, self.max_response_bytes, "registry response")?;
        serde_json::from_slice(&bytes)
            .map_err(|error| Error::Other(format!("invalid registry response: {error}")))
    }

    fn bearer(
        &self,
        request: reqwest::blocking::RequestBuilder,
    ) -> reqwest::blocking::RequestBuilder {
        match &self.token {
            Some(token) => request.bearer_auth(token),
            None => request,
        }
    }

    pub fn get_package(&self, org: &str, name: &str) -> Result<PackageMetadata> {
        let path = registry::package_path(&encode_segment(org), &encode_segment(name));
        let response = self.http.get(self.url(&path)).send()?;
        self.json(response)
    }

    pub fn get_version(&self, org: &str, name: &str, version: &str) -> Result<VersionMetadata> {
        let path = registry::version_path(
            &encode_segment(org),
            &encode_segment(name),
            &encode_segment(version),
        );
        let response = self.http.get(self.url(&path)).send()?;
        self.json(response)
    }

    pub fn search(&self, query: &str) -> Result<SearchResponse> {
        let response = self
            .http
            .get(self.url(&registry::search_path()))
            .query(&[("q", query)])
            .send()?;
        self.json(response)
    }

    pub fn claim_org(&self, slug: &str) -> Result<ClaimOrgResponse> {
        let request = self
            .http
            .post(self.url(&registry::orgs_path()))
            .json(&ClaimOrgRequest {
                slug: slug.to_string(),
            });
        let response = self.bearer(request).send()?;
        self.json(response)
    }

    pub fn yank(&self, org: &str, name: &str, version: &str, yanked: bool) -> Result<YankResponse> {
        let request = self
            .http
            .post(self.url(&registry::yank_path(
                &encode_segment(org),
                &encode_segment(name),
                &encode_segment(version),
            )))
            .json(&YankRequest { yanked });
        let response = self.bearer(request).send()?;
        self.json(response)
    }

    fn allowed_download_url(&self, raw: &str) -> Result<reqwest::Url> {
        let url = reqwest::Url::parse(raw)
            .map_err(|_| Error::Other("download URL is invalid".to_string()))?;
        if !url.username().is_empty() || url.password().is_some() || url.fragment().is_some() {
            return Err(Error::Other(
                "download URL contains credentials or fragment".to_string(),
            ));
        }
        let loopback = matches!(url.host_str(), Some("localhost"))
            || url
                .host_str()
                .and_then(|host| host.parse::<std::net::IpAddr>().ok())
                .is_some_and(|ip| ip.is_loopback());
        match url.scheme() {
            "https" => Ok(url),
            "http" if loopback || self.base.starts_with("http://") => Ok(url),
            scheme => Err(Error::Other(format!(
                "refusing artifact download over `{scheme}`"
            ))),
        }
    }

    fn resolve_download_url(&self, raw: &str, sha256: &str) -> Result<reqwest::Url> {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            return reqwest::Url::parse(
                &self.url(&registry::artifact_path(&encode_segment(sha256))),
            )
            .map_err(|_| Error::InvalidBaseUrl);
        }
        if let Ok(absolute) = reqwest::Url::parse(trimmed) {
            return self.allowed_download_url(absolute.as_str());
        }
        let base = reqwest::Url::parse(&(self.base.clone() + "/"))
            .map_err(|_| Error::InvalidBaseUrl)?;
        let resolved = base
            .join(trimmed)
            .map_err(|_| Error::Other("relative download URL is invalid".to_string()))?;
        self.allowed_download_url(resolved.as_str())
    }

    pub fn download_artifact(&self, version: &VersionMetadata, dest: &Path) -> Result<()> {
        let url = self.resolve_download_url(&version.download_url, &version.sha256)?;
        // The shared HTTP client refuses redirects. No bearer token is attached:
        // download_url may be a third-party presigned URL.
        let response = self.check(self.http.get(url).send()?)?;
        let limit = download_limit(version.size);
        let bytes = Self::read_capped(response, limit, "artifact")?;
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
                serde_json::to_string(meta).map_err(|error| Error::Other(error.to_string()))?,
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
        self.json(response)
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
    fn base_url_is_validated_and_token_is_redacted() {
        let client = Client::new(" https://registry.zpkg.tech/gateway/// ")
            .unwrap()
            .with_token("very-secret");
        assert_eq!(
            client.url(&registry::package_path("acme", "kit")),
            "https://registry.zpkg.tech/gateway/v1/packages/acme/kit"
        );
        let debug = format!("{client:?}");
        assert!(debug.contains("[REDACTED]"));
        assert!(!debug.contains("very-secret"));
        for invalid in [
            "relative/path",
            "ftp://registry.test",
            "https://user:secret@registry.test",
            "https://registry.test?tenant=one",
            "https://registry.test#fragment",
        ] {
            assert!(Client::new(invalid).is_err(), "accepted {invalid}");
        }
    }

    #[test]
    fn api_error_display_excludes_remote_message() {
        let error = Error::Api {
            status: 502,
            code: "http_502".to_string(),
            message: "provider-secret".to_string(),
        };
        assert_eq!(error.to_string(), "registry error 502: http_502");
        assert!(!error.to_string().contains("provider-secret"));
    }

    #[test]
    fn path_segments_are_percent_encoded() {
        assert_eq!(encode_segment("1.2.0"), "1.2.0");
        assert_eq!(
            encode_segment("release candidate/1"),
            "release%20candidate%2F1"
        );
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
    fn download_policy_and_limits() {
        let client = Client::new("https://registry.zpkg.tech").unwrap();
        assert_eq!(download_limit(0), MAX_ARTIFACT_BYTES);
        assert_eq!(download_limit(10), 10 + DOWNLOAD_SLACK);
        for raw in ["http://evil.example/artifact", "file:///etc/passwd"] {
            assert!(client.allowed_download_url(raw).is_err(), "accepted {raw}");
        }
        assert!(client
            .allowed_download_url("http://127.0.0.1:8080/a")
            .is_ok());
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

    fn spawn_server(body: Vec<u8>) -> (String, mpsc::Receiver<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let (sender, receiver) = mpsc::channel();
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buffer = [0_u8; 4096];
                let mut request = Vec::new();
                loop {
                    let read = stream.read(&mut buffer).unwrap_or(0);
                    if read == 0 {
                        break;
                    }
                    request.extend_from_slice(&buffer[..read]);
                    if request.windows(4).any(|window| window == b"\r\n\r\n") {
                        break;
                    }
                }
                let _ = sender.send(String::from_utf8_lossy(&request).into_owned());
                let header = format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = stream.write_all(header.as_bytes());
                let _ = stream.write_all(&body);
                let _ = stream.flush();
            }
        });
        (format!("http://{addr}"), receiver)
    }

    #[test]
    fn download_omits_auth_resolves_relative_and_verifies_sha() {
        let body = b"artifact-bytes".to_vec();
        let sha = hex::encode(Sha256::digest(&body));
        let (base, receiver) = spawn_server(body.clone());
        let client = Client::new(format!("{base}/gateway"))
            .unwrap()
            .with_token("zpkg_t");
        let directory = std::env::temp_dir().join(format!(
            "zed-dl-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let destination = directory.join("artifact.tar.gz");
        let metadata = version("artifact", &sha, body.len() as u64);
        client.download_artifact(&metadata, &destination).unwrap();
        let request = receiver.recv().unwrap();
        assert!(request.starts_with("GET /gateway/artifact "), "request={request}");
        assert!(!request.to_lowercase().contains("authorization"));
        assert_eq!(std::fs::read(&destination).unwrap(), body);
        let _ = std::fs::remove_dir_all(&directory);
    }

    #[test]
    fn download_rejects_oversize_body() {
        let limit = download_limit(1);
        let body = vec![0_u8; (limit + 64) as usize];
        let (base, _receiver) = spawn_server(body);
        let client = Client::new(&base).unwrap();
        let directory = std::env::temp_dir().join(format!("zed-dl-big-{}", std::process::id()));
        let destination = directory.join("artifact.tar.gz");
        let metadata = version(&format!("{base}/artifact"), "deadbeef", 1);
        let error = client.download_artifact(&metadata, &destination).unwrap_err();
        assert!(matches!(&error, Error::Other(message) if message.contains("exceeded")));
        let _ = std::fs::remove_dir_all(&directory);
    }
}
