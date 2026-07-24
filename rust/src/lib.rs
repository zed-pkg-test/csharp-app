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

    /// Download an artifact to `dest` and verify its sha256.
    pub fn download_artifact(&self, version: &VersionMetadata, dest: &Path) -> Result<()> {
        let url = if version.download_url.starts_with("http") {
            version.download_url.clone()
        } else {
            self.url(&registry::artifact_path(&encode_segment(&version.sha256)))
        };
        let mut response = Self::check(self.http.get(url).send()?)?;
        let mut bytes = Vec::new();
        response.read_to_end(&mut bytes)?;
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
}
