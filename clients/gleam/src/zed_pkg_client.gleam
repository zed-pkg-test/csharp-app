//// Gleam SDK for the zed-pkg registry. Types mirror the JSON Schemas in
//// `zed-interfaces/schemas/`; the transport is injectable so tests run
//// without a network.

import gleam/bit_array
import gleam/crypto
import gleam/dynamic/decode
import gleam/http
import gleam/http/request.{type Request}
import gleam/http/response.{type Response}
import gleam/httpc
import gleam/int
import gleam/json
import gleam/option.{type Option, None, Some}
import gleam/result
import gleam/string
import gleam/uri
import zed_pkg_client/model.{
  type ClaimOrgResponse, type PackageMetadata, type PublishResponse,
  type SearchResponse, type VersionMetadata, type YankResponse,
}

pub const default_registry_url = "https://registry.zpkg.tech"

/// Bounds every request (connect + read), in milliseconds.
pub const default_timeout_ms = 30_000

/// Hard ceiling on artifact downloads, matching the server's
/// `MAX_ARTIFACT_BYTES` default (100 MiB); plus the slack added to a
/// version's declared size.
pub const max_artifact_bytes = 104_857_600

const download_slack = 1_048_576

/// The declared size (when sane) plus slack, capped by the ceiling.
pub fn download_limit(size: Int) -> Int {
  case size > 0 {
    True -> int.min(size + download_slack, max_artifact_bytes)
    False -> max_artifact_bytes
  }
}

pub type ClientError {
  /// The registry answered with a structured error body.
  ApiError(status: Int, code: String, message: String)
  TransportError(error: httpc.HttpError)
  InvalidResponse(message: String)
  Sha256Mismatch(expected: String, actual: String)
  ArtifactTooLarge(limit: Int)
  InsecureDownloadUrl(message: String)
}

/// Injectable transport used for deterministic tests and alternate runtimes.
pub type Transport =
  fn(Request(BitArray), Int) -> Result(Response(BitArray), httpc.HttpError)

pub opaque type Client {
  Client(
    base: String,
    token: Option(String),
    timeout_ms: Int,
    transport: Transport,
  )
}

/// Construct a client against `base` (trailing slashes are trimmed) with the
/// default httpc transport: TLS verification on, redirects disabled, and a
/// thirty-second deadline.
pub fn new(base: String) -> Client {
  Client(
    base: normalize_base(base),
    token: None,
    timeout_ms: default_timeout_ms,
    transport: httpc_transport,
  )
}

/// Attach a bearer token used for authenticated calls (claim_org, yank,
/// publish). Never sent on artifact downloads.
pub fn with_token(client: Client, token: String) -> Client {
  case token {
    "" -> Client(..client, token: None)
    _ -> Client(..client, token: Some(token))
  }
}

pub fn with_timeout(client: Client, timeout_ms: Int) -> Client {
  Client(..client, timeout_ms: timeout_ms)
}

pub fn with_transport(client: Client, transport: Transport) -> Client {
  Client(..client, transport: transport)
}

fn normalize_base(base: String) -> String {
  case string.ends_with(base, "/") {
    True -> normalize_base(string.drop_end(base, 1))
    False -> base
  }
}

fn httpc_transport(
  req: Request(BitArray),
  timeout_ms: Int,
) -> Result(Response(BitArray), httpc.HttpError) {
  httpc.configure()
  |> httpc.timeout(timeout_ms)
  |> httpc.follow_redirects(False)
  |> httpc.dispatch_bits(req)
}

/// Percent-encode one path segment (everything reserved, including `/`), so
/// opaque version tags cannot break out of their URL segment.
pub fn encode_segment(segment: String) -> String {
  uri.percent_encode(segment)
}

pub fn package_path(org: String, name: String) -> String {
  "/v1/packages/" <> encode_segment(org) <> "/" <> encode_segment(name)
}

pub fn version_path(org: String, name: String, version: String) -> String {
  package_path(org, name) <> "/versions/" <> encode_segment(version)
}

pub fn artifact_path(sha256: String) -> String {
  "/v1/artifacts/" <> encode_segment(sha256)
}

pub fn yank_path(org: String, name: String, version: String) -> String {
  version_path(org, name, version) <> "/yank"
}

/// Enforce the download-url scheme policy: https is always allowed; http only
/// for loopback hosts or when the registry base is itself http. A malicious
/// registry response must not redirect fetches to plaintext or unexpected
/// hosts.
pub fn allowed_download_url(
  raw: String,
  base: String,
) -> Result(String, ClientError) {
  case uri.parse(raw) {
    Error(_) ->
      Error(InsecureDownloadUrl(message: "bad download url " <> raw))
    Ok(parsed) -> {
      let loopback = case parsed.host {
        Some("localhost") | Some("127.0.0.1") | Some("[::1]") | Some("::1") ->
          True
        Some(host) -> string.starts_with(host, "127.")
        None -> False
      }
      case parsed.scheme {
        Some("https") -> Ok(raw)
        Some("http") ->
          case loopback || string.starts_with(base, "http://") {
            True -> Ok(raw)
            False ->
              Error(InsecureDownloadUrl(
                message: "refusing artifact download over `http` from "
                  <> raw
                  <> " (https required for non-local registries)",
              ))
          }
        _ ->
          Error(InsecureDownloadUrl(
            message: "refusing artifact download from "
              <> raw
              <> " (https required for non-local registries)",
          ))
      }
    }
  }
}

/// Lowercase hex sha256 of `bytes`.
pub fn sha256_hex(bytes: BitArray) -> String {
  crypto.hash(crypto.Sha256, bytes)
  |> bit_array.base16_encode
  |> string.lowercase
}

pub fn verify_sha256(
  bytes: BitArray,
  expected: String,
) -> Result(Nil, ClientError) {
  let actual = sha256_hex(bytes)
  case actual == expected {
    True -> Ok(Nil)
    False -> Error(Sha256Mismatch(expected: expected, actual: actual))
  }
}

fn base_request(client: Client, path: String) -> Result(Request(BitArray), ClientError) {
  case request.to(client.base <> path) {
    Ok(req) ->
      Ok(
        req
        |> request.set_header("accept", "application/json")
        |> request.set_body(<<>>),
      )
    Error(_) -> Error(InvalidResponse(message: "invalid url " <> client.base <> path))
  }
}

fn authorize(client: Client, req: Request(BitArray)) -> Request(BitArray) {
  case client.token {
    Some(token) -> request.set_header(req, "authorization", "Bearer " <> token)
    None -> req
  }
}

fn send(
  client: Client,
  req: Request(BitArray),
) -> Result(Response(BitArray), ClientError) {
  client.transport(req, client.timeout_ms)
  |> result.map_error(fn(error) { TransportError(error: error) })
}

/// Map a non-2xx response to a typed error carrying the registry's stable
/// `ApiError.code` ("unknown" when the body is not ApiError JSON).
fn check(response: Response(BitArray)) -> Result(BitArray, ClientError) {
  case response.status >= 200 && response.status < 300 {
    True -> Ok(response.body)
    False -> {
      let text =
        bit_array.to_string(response.body)
        |> result.unwrap("")
      let decoder = {
        use code <- decode.field("code", decode.string)
        use message <- decode.field("message", decode.string)
        decode.success(#(code, message))
      }
      case json.parse(from: text, using: decoder) {
        Ok(#(code, message)) ->
          Error(ApiError(status: response.status, code:, message:))
        Error(_) ->
          Error(ApiError(status: response.status, code: "unknown", message: text))
      }
    }
  }
}

fn decode_json(
  bytes: BitArray,
  decoder: decode.Decoder(t),
) -> Result(t, ClientError) {
  case bit_array.to_string(bytes) {
    Error(_) -> Error(InvalidResponse(message: "response body is not utf-8"))
    Ok(text) ->
      json.parse(from: text, using: decoder)
      |> result.map_error(fn(error) {
        InvalidResponse(message: "invalid registry response: " <> string.inspect(error))
      })
  }
}

fn request_json(
  client: Client,
  method: http.Method,
  path: String,
  body: Option(json.Json),
  authorized: Bool,
  decoder: decode.Decoder(t),
) -> Result(t, ClientError) {
  use req <- result.try(base_request(client, path))
  let req = request.set_method(req, method)
  let req = case authorized {
    True -> authorize(client, req)
    False -> req
  }
  let req = case body {
    Some(payload) ->
      req
      |> request.set_header("content-type", "application/json")
      |> request.set_body(bit_array.from_string(json.to_string(payload)))
    None -> req
  }
  use response <- result.try(send(client, req))
  use bytes <- result.try(check(response))
  decode_json(bytes, decoder)
}

/// `GET /v1/packages/{org}/{name}` — package metadata + version list.
pub fn get_package(
  client: Client,
  org: String,
  name: String,
) -> Result(PackageMetadata, ClientError) {
  request_json(
    client,
    http.Get,
    package_path(org, name),
    None,
    False,
    model.package_metadata_decoder(),
  )
}

/// `GET /v1/packages/{org}/{name}/versions/{version}`.
pub fn get_version(
  client: Client,
  org: String,
  name: String,
  version: String,
) -> Result(VersionMetadata, ClientError) {
  request_json(
    client,
    http.Get,
    version_path(org, name, version),
    None,
    False,
    model.version_metadata_decoder(),
  )
}

/// `GET /v1/search?q=`.
pub fn search(
  client: Client,
  query: String,
) -> Result(SearchResponse, ClientError) {
  request_json(
    client,
    http.Get,
    "/v1/search?q=" <> uri.percent_encode(query),
    None,
    False,
    model.search_response_decoder(),
  )
}

/// `POST /v1/orgs` (bearer token).
pub fn claim_org(
  client: Client,
  slug: String,
) -> Result(ClaimOrgResponse, ClientError) {
  request_json(
    client,
    http.Post,
    "/v1/orgs",
    Some(json.object([#("slug", json.string(slug))])),
    True,
    model.claim_org_response_decoder(),
  )
}

/// `POST .../versions/{version}/yank` — yank (`True`) or restore (`False`) a
/// published version. Requires a bearer token with publish rights.
pub fn yank(
  client: Client,
  org: String,
  name: String,
  version: String,
  yanked: Bool,
) -> Result(YankResponse, ClientError) {
  request_json(
    client,
    http.Post,
    yank_path(org, name, version),
    Some(json.object([#("yanked", json.bool(yanked))])),
    True,
    model.yank_response_decoder(),
  )
}

/// Download an artifact, verify its sha256, and return the bytes.
pub fn download_artifact(
  client: Client,
  version: VersionMetadata,
) -> Result(BitArray, ClientError) {
  // An absolute url (any scheme) must clear the scheme/host policy; a bare
  // path is resolved against the trusted registry base.
  use url <- result.try(case string.contains(version.download_url, "://") {
    True -> allowed_download_url(version.download_url, client.base)
    False -> Ok(client.base <> artifact_path(version.sha256))
  })
  use req <- result.try(case request.to(url) {
    Ok(req) -> Ok(request.set_body(req, <<>>))
    Error(_) -> Error(InvalidResponse(message: "invalid url " <> url))
  })
  // Deliberately no auth header: download_url may point at a third-party host
  // (e.g. a presigned S3/R2 url) and the token must not leak there.
  use response <- result.try(send(client, req))
  use bytes <- result.try(check(response))
  let limit = download_limit(version.size)
  case bit_array.byte_size(bytes) > limit {
    True -> Error(ArtifactTooLarge(limit: limit))
    False -> {
      use _ <- result.try(verify_sha256(bytes, version.sha256))
      Ok(bytes)
    }
  }
}

/// Build the `multipart/form-data` body for a publish: a `meta` field
/// carrying the PublishMeta JSON and an `artifact` file part.
pub fn multipart_body(
  boundary: String,
  meta_json: String,
  filename: String,
  artifact: BitArray,
) -> BitArray {
  bit_array.concat([
    bit_array.from_string(
      "--"
      <> boundary
      <> "\r\ncontent-disposition: form-data; name=\"meta\"\r\n\r\n"
      <> meta_json
      <> "\r\n--"
      <> boundary
      <> "\r\ncontent-disposition: form-data; name=\"artifact\"; filename=\""
      <> filename
      <> "\"\r\ncontent-type: application/octet-stream\r\n\r\n",
    ),
    artifact,
    bit_array.from_string("\r\n--" <> boundary <> "--\r\n"),
  ])
}

/// Publish: multipart `meta` (PublishMeta JSON) + `artifact` bytes via
/// `PUT /v1/packages/{org}/{name}/versions/{version}`. Requires a bearer
/// token. `org`, `name` and `version` must match `meta.manifest.package`.
pub fn publish(
  client: Client,
  org: String,
  name: String,
  version: String,
  meta_json: String,
  artifact: BitArray,
) -> Result(PublishResponse, ClientError) {
  let boundary =
    "zedpkg" <> sha256_hex(crypto.strong_random_bytes(16))
  let filename = org <> "-" <> name <> "-" <> version <> ".tar.gz"
  use req <- result.try(base_request(client, version_path(org, name, version)))
  let req =
    req
    |> request.set_method(http.Put)
    |> request.set_header(
      "content-type",
      "multipart/form-data; boundary=" <> boundary,
    )
    |> request.set_body(multipart_body(boundary, meta_json, filename, artifact))
  let req = authorize(client, req)
  use response <- result.try(send(client, req))
  use bytes <- result.try(check(response))
  decode_json(bytes, model.publish_response_decoder())
}
