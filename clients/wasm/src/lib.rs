//! WebAssembly SDK for the zed-pkg registry (browsers and workers via
//! `wasm-pack`). Reuses the DTOs and URL helpers from `zed-interfaces`, so it
//! cannot drift from the contract; responses are validated against those types
//! before being handed to JavaScript.

use js_sys::{Array, Uint8Array};
use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
use serde::de::DeserializeOwned;
use sha2::{Digest, Sha256};
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use wasm_bindgen_futures::JsFuture;
use web_sys::{Blob, BlobPropertyBag, FormData, Request, RequestInit, Response};
use zed_interfaces::registry::{
    self, ApiError, ClaimOrgResponse, PackageMetadata, PublishResponse, SearchResponse,
    VersionMetadata, YankResponse, DEFAULT_REGISTRY_URL,
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

/// Enforce the download-url scheme policy: https is always allowed; http only
/// for loopback hosts or when the registry base is itself http. A malicious
/// registry response must not redirect fetches to plaintext or unexpected
/// hosts.
fn allowed_download_url(raw: &str, base: &str) -> Result<String, String> {
    let url = url::Url::parse(raw).map_err(|e| format!("bad download url {raw}: {e}"))?;
    let loopback = matches!(url.host_str(), Some("localhost"))
        || url
            .host_str()
            .and_then(|h| h.trim_matches(['[', ']']).parse::<std::net::IpAddr>().ok())
            .is_some_and(|ip| ip.is_loopback());
    match url.scheme() {
        "https" => Ok(raw.to_string()),
        "http" if loopback || base.starts_with("http://") => Ok(raw.to_string()),
        other => Err(format!(
            "refusing artifact download over `{other}` from {raw} \
             (https required for non-local registries)"
        )),
    }
}

fn verify_sha256(bytes: &[u8], expected: &str) -> Result<(), String> {
    let actual = hex::encode(Sha256::digest(bytes));
    if actual != expected {
        return Err(format!(
            "artifact sha256 mismatch: expected {expected}, got {actual}"
        ));
    }
    Ok(())
}

fn js_error(message: impl AsRef<str>) -> JsValue {
    js_sys::Error::new(message.as_ref()).into()
}

/// Call the global `fetch` (works in windows and workers alike, without
/// binding a specific global scope type).
fn global_fetch(request: &Request) -> Result<js_sys::Promise, JsValue> {
    let global = js_sys::global();
    let fetch = js_sys::Reflect::get(&global, &JsValue::from_str("fetch"))?;
    let fetch: js_sys::Function = fetch
        .dyn_into()
        .map_err(|_| js_error("global fetch is unavailable in this runtime"))?;
    fetch.call1(&global, request)?.dyn_into().map_err(JsValue::from)
}

#[wasm_bindgen]
pub struct ZedClient {
    base: String,
    token: Option<String>,
}

#[wasm_bindgen]
impl ZedClient {
    /// Create a client. `base_url` defaults to the public registry.
    #[wasm_bindgen(constructor)]
    pub fn new(base_url: Option<String>) -> ZedClient {
        let base = base_url.unwrap_or_else(|| DEFAULT_REGISTRY_URL.to_string());
        ZedClient {
            base: base.trim_end_matches('/').to_string(),
            token: None,
        }
    }

    /// Attach a bearer token used for authenticated calls (claimOrg, yank,
    /// publish). Never sent on artifact downloads.
    #[wasm_bindgen(js_name = withToken)]
    pub fn with_token(&mut self, token: String) {
        self.token = if token.is_empty() { None } else { Some(token) };
    }

    fn url(&self, path: &str) -> String {
        format!("{}{path}", self.base)
    }

    async fn send(&self, request: Request, authorized: bool) -> Result<Response, JsValue> {
        if authorized {
            if let Some(token) = &self.token {
                request
                    .headers()
                    .set("authorization", &format!("Bearer {token}"))?;
            }
        }
        request.headers().set("accept", "application/json")?;
        let response = JsFuture::from(global_fetch(&request)?).await?;
        response.dyn_into::<Response>().map_err(JsValue::from)
    }

    /// Read the body, surface an `ApiError`-shaped failure on non-2xx.
    async fn check(&self, response: Response) -> Result<Vec<u8>, JsValue> {
        let buffer = JsFuture::from(response.array_buffer()?).await?;
        let bytes = Uint8Array::new(&buffer).to_vec();
        if response.ok() {
            return Ok(bytes);
        }
        let status = response.status();
        let body = String::from_utf8_lossy(&bytes);
        let (code, message) = match serde_json::from_slice::<ApiError>(&bytes) {
            Ok(err) => (err.code, err.message),
            Err(_) => ("unknown".to_string(), body.into_owned()),
        };
        Err(js_error(format!(
            "registry error {status}: {code}: {message}"
        )))
    }

    /// Decode a JSON body into the contract type, then hand it to JS.
    async fn json<T: DeserializeOwned + serde::Serialize>(
        &self,
        response: Response,
    ) -> Result<JsValue, JsValue> {
        let bytes = self.check(response).await?;
        let value: T = serde_json::from_slice(&bytes)
            .map_err(|e| js_error(format!("invalid registry response: {e}")))?;
        serde_wasm_bindgen::to_value(&value).map_err(JsValue::from)
    }

    fn get_request(&self, path: &str) -> Result<Request, JsValue> {
        let init = RequestInit::new();
        init.set_method("GET");
        Request::new_with_str_and_init(&self.url(path), &init).map_err(JsValue::from)
    }

    fn json_request(&self, method: &str, path: &str, body: &str) -> Result<Request, JsValue> {
        let init = RequestInit::new();
        init.set_method(method);
        init.set_body(&JsValue::from_str(body));
        let request = Request::new_with_str_and_init(&self.url(path), &init)?;
        request.headers().set("content-type", "application/json")?;
        Ok(request)
    }

    /// `GET /v1/packages/{org}/{name}` — package metadata + version list.
    #[wasm_bindgen(js_name = getPackage)]
    pub async fn get_package(&self, org: String, name: String) -> Result<JsValue, JsValue> {
        let path = registry::package_path(&encode_segment(&org), &encode_segment(&name));
        let response = self.send(self.get_request(&path)?, false).await?;
        self.json::<PackageMetadata>(response).await
    }

    /// `GET /v1/packages/{org}/{name}/versions/{version}`.
    #[wasm_bindgen(js_name = getVersion)]
    pub async fn get_version(
        &self,
        org: String,
        name: String,
        version: String,
    ) -> Result<JsValue, JsValue> {
        let path = registry::version_path(
            &encode_segment(&org),
            &encode_segment(&name),
            &encode_segment(&version),
        );
        let response = self.send(self.get_request(&path)?, false).await?;
        self.json::<VersionMetadata>(response).await
    }

    /// `GET /v1/search?q=`.
    pub async fn search(&self, query: String) -> Result<JsValue, JsValue> {
        let path = format!(
            "{}?q={}",
            registry::search_path(),
            utf8_percent_encode(&query, SEGMENT)
        );
        let response = self.send(self.get_request(&path)?, false).await?;
        self.json::<SearchResponse>(response).await
    }

    /// `POST /v1/orgs` (bearer token).
    #[wasm_bindgen(js_name = claimOrg)]
    pub async fn claim_org(&self, slug: String) -> Result<JsValue, JsValue> {
        let body = serde_json::json!({ "slug": slug }).to_string();
        let request = self.json_request("POST", &registry::orgs_path(), &body)?;
        let response = self.send(request, true).await?;
        self.json::<ClaimOrgResponse>(response).await
    }

    /// `POST .../versions/{version}/yank` — yank (`true`) or restore
    /// (`false`) a published version. Requires a bearer token with publish
    /// rights on the org.
    pub async fn yank(
        &self,
        org: String,
        name: String,
        version: String,
        yanked: bool,
    ) -> Result<JsValue, JsValue> {
        let path = registry::yank_path(
            &encode_segment(&org),
            &encode_segment(&name),
            &encode_segment(&version),
        );
        let body = serde_json::json!({ "yanked": yanked }).to_string();
        let request = self.json_request("POST", &path, &body)?;
        let response = self.send(request, true).await?;
        self.json::<YankResponse>(response).await
    }

    /// Download an artifact, verify its sha256, and return the bytes.
    ///
    /// `version` is a `VersionMetadata` object as returned by `getVersion`.
    #[wasm_bindgen(js_name = downloadArtifact)]
    pub async fn download_artifact(&self, version: JsValue) -> Result<Uint8Array, JsValue> {
        let version: VersionMetadata =
            serde_wasm_bindgen::from_value(version).map_err(JsValue::from)?;
        // An absolute url (any scheme) must clear the scheme/host policy; a
        // bare path is resolved against the trusted registry base.
        let url = if version.download_url.contains("://") {
            allowed_download_url(&version.download_url, &self.base).map_err(js_error)?
        } else {
            self.url(&registry::artifact_path(&encode_segment(&version.sha256)))
        };
        let init = RequestInit::new();
        init.set_method("GET");
        // Deliberately no auth header: download_url may point at a third-party
        // host (e.g. a presigned S3/R2 url) and the token must not leak there.
        let request = Request::new_with_str_and_init(&url, &init)?;
        let response = JsFuture::from(global_fetch(&request)?).await?;
        let response: Response = response.dyn_into()?;
        if !response.ok() {
            return Err(self.check(response).await.expect_err("non-ok response"));
        }
        let limit = download_limit(version.size);
        if let Ok(Some(declared)) = response.headers().get("content-length") {
            if declared.parse::<u64>().is_ok_and(|len| len > limit) {
                return Err(js_error(format!("artifact exceeded {limit} bytes; refusing")));
            }
        }
        let buffer = JsFuture::from(response.array_buffer()?).await?;
        let bytes = Uint8Array::new(&buffer).to_vec();
        if bytes.len() as u64 > limit {
            return Err(js_error(format!("artifact exceeded {limit} bytes; refusing")));
        }
        verify_sha256(&bytes, &version.sha256).map_err(js_error)?;
        Ok(Uint8Array::from(bytes.as_slice()))
    }

    /// Publish: multipart `meta` (PublishMeta JSON string) + `artifact` bytes.
    /// Requires a bearer token.
    pub async fn publish(&self, meta_json: String, artifact: Uint8Array) -> Result<JsValue, JsValue> {
        // Parse locally so the org/name/version segments come from the meta
        // itself, exactly as the native SDKs do.
        let meta: zed_interfaces::registry::PublishMeta = serde_json::from_str(&meta_json)
            .map_err(|e| js_error(format!("invalid publish meta: {e}")))?;
        let package = &meta.manifest.package;
        let path = registry::version_path(
            &encode_segment(&package.org),
            &encode_segment(&package.name),
            &encode_segment(&package.version),
        );
        let filename = format!("{}-{}-{}.tar.gz", package.org, package.name, package.version);

        let parts = Array::new();
        parts.push(&Uint8Array::from(artifact.to_vec().as_slice()));
        let options = BlobPropertyBag::new();
        options.set_type("application/gzip");
        let blob = Blob::new_with_u8_array_sequence_and_options(&parts, &options)?;

        let form = FormData::new()?;
        form.append_with_str(registry::PUBLISH_META_FIELD, &meta_json)?;
        form.append_with_blob_and_filename(registry::PUBLISH_ARTIFACT_FIELD, &blob, &filename)?;

        let init = RequestInit::new();
        init.set_method("PUT");
        init.set_body(&form);
        let request = Request::new_with_str_and_init(&self.url(&path), &init)?;
        let response = self.send(request, true).await?;
        self.json::<PublishResponse>(response).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha_verification() {
        let bytes = b"zed";
        let good = hex::encode(Sha256::digest(bytes));
        assert!(verify_sha256(bytes, &good).is_ok());
        assert!(verify_sha256(bytes, "00").is_err());
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
    fn download_limit_caps_at_ceiling() {
        assert_eq!(download_limit(0), MAX_ARTIFACT_BYTES);
        assert_eq!(download_limit(10), 10 + DOWNLOAD_SLACK);
        assert_eq!(download_limit(MAX_ARTIFACT_BYTES), MAX_ARTIFACT_BYTES);
    }

    #[test]
    fn insecure_download_urls_are_rejected() {
        let base = "https://registry.zpkg.tech";
        for raw in ["http://evil.example/artifact", "file:///etc/passwd"] {
            let err = allowed_download_url(raw, base).unwrap_err();
            assert!(err.contains("refusing"), "expected refusal for {raw}: {err}");
        }
    }

    #[test]
    fn loopback_http_download_url_is_allowed() {
        let base = "https://registry.zpkg.tech";
        assert!(allowed_download_url("http://127.0.0.1:8080/a", base).is_ok());
        assert!(allowed_download_url("http://localhost/a", base).is_ok());
        assert!(allowed_download_url("http://[::1]:9/a", base).is_ok());
        assert!(allowed_download_url("https://cdn.example/a", base).is_ok());
        assert!(allowed_download_url("http://10.0.0.1/a", base).is_err());
        // An http registry base opts in to plaintext downloads.
        assert!(allowed_download_url("http://mirror.internal/a", "http://registry.internal").is_ok());
    }
}
