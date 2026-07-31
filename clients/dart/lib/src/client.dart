import 'dart:convert';
import 'dart:io' show InternetAddress;
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;

import 'models.dart';

const String defaultRegistryUrl = 'https://registry.zpkg.tech';

/// Bounds every request (connect + read).
const Duration defaultTimeout = Duration(seconds: 30);

/// Hard ceiling on artifact downloads, matching the server's
/// `MAX_ARTIFACT_BYTES` default (100 MiB); plus the slack added to a
/// version's declared size.
const int maxArtifactBytes = 100 * 1024 * 1024;
const int _downloadSlack = 1024 * 1024;

/// The declared size (when sane) plus slack, capped by the ceiling.
int downloadLimit(int size) {
  if (size > 0) {
    final limit = size + _downloadSlack;
    return limit < maxArtifactBytes ? limit : maxArtifactBytes;
  }
  return maxArtifactBytes;
}

String _segment(String value) => Uri.encodeComponent(value);

String packagePath(String org, String name) =>
    '/v1/packages/${_segment(org)}/${_segment(name)}';

String versionPath(String org, String name, String version) =>
    '/v1/packages/${_segment(org)}/${_segment(name)}/versions/${_segment(version)}';

String artifactPath(String sha256) => '/v1/artifacts/${_segment(sha256)}';

String yankPath(String org, String name, String version) =>
    '${versionPath(org, name, version)}/yank';

bool _isLoopbackHost(String host) {
  if (host == 'localhost') return true;
  final bare = host.replaceAll(RegExp(r'^\[|\]$'), '');
  final address = InternetAddress.tryParse(bare);
  return address?.isLoopback ?? false;
}

/// Enforce the download-url scheme policy: https is always allowed; http only
/// for loopback hosts or when the registry base is itself http. A malicious
/// registry response must not redirect fetches to plaintext or unexpected
/// hosts.
String allowedDownloadUrl(String raw, String base) {
  final Uri url;
  try {
    url = Uri.parse(raw);
  } on FormatException catch (error) {
    throw ZedApiError(0, 'bad_download_url', 'bad download url $raw: $error');
  }
  if (url.scheme == 'https') return raw;
  if (url.scheme == 'http' &&
      (_isLoopbackHost(url.host) || base.startsWith('http://'))) {
    return raw;
  }
  throw ZedApiError(
    0,
    'insecure_download_url',
    'refusing artifact download over `${url.scheme}` from $raw '
        '(https required for non-local registries)',
  );
}

/// Verify `bytes` against an expected lowercase hex sha256.
void verifySha256(List<int> bytes, String expected) {
  final actual = sha256.convert(bytes).toString();
  if (actual != expected) {
    throw ZedApiError(0, 'sha256_mismatch', 'expected $expected, got $actual');
  }
}

/// Client for the zed-pkg registry API.
final class ZedClient {
  ZedClient({
    String registryUrl = defaultRegistryUrl,
    this.token,
    http.Client? httpClient,
    Duration timeout = defaultTimeout,
  })  : base = registryUrl.replaceAll(RegExp(r'/+$'), ''),
        _http = httpClient ?? http.Client(),
        _ownsHttpClient = httpClient == null,
        _timeout = timeout;

  final String base;
  final String? token;
  final http.Client _http;
  final bool _ownsHttpClient;
  final Duration _timeout;

  void close() {
    if (_ownsHttpClient) {
      _http.close();
    }
  }

  Map<String, String> _headers({bool authorized = false, String? contentType}) {
    final headers = <String, String>{'accept': 'application/json'};
    if (authorized && token != null) {
      headers['authorization'] = 'Bearer $token';
    }
    if (contentType != null) {
      headers['content-type'] = contentType;
    }
    return headers;
  }

  Never _throwApiError(int status, List<int> body) {
    final text = utf8.decode(body, allowMalformed: true);
    var code = 'unknown';
    var message = text;
    try {
      final parsed = jsonDecode(text);
      if (parsed is Map<String, dynamic>) {
        code = parsed['code'] is String ? parsed['code'] as String : code;
        message =
            parsed['message'] is String ? parsed['message'] as String : text;
      }
    } on FormatException {
      // non-JSON error body; keep the raw text
    }
    throw ZedApiError(status, code, message);
  }

  Future<Map<String, dynamic>> _requestJson(
    String method,
    String path, {
    Object? body,
    bool authorized = false,
  }) async {
    final request = http.Request(method, Uri.parse('$base$path'));
    request.headers.addAll(_headers(
      authorized: authorized,
      contentType: body != null ? 'application/json' : null,
    ));
    if (body != null) {
      request.body = jsonEncode(body);
    }
    final streamed = await _http.send(request).timeout(_timeout);
    final response = await http.Response.fromStream(streamed).timeout(_timeout);
    if (response.statusCode < 200 || response.statusCode >= 300) {
      _throwApiError(response.statusCode, response.bodyBytes);
    }
    final decoded = jsonDecode(utf8.decode(response.bodyBytes));
    if (decoded is! Map<String, dynamic>) {
      throw ZedApiError(0, 'invalid_response', 'expected a JSON object');
    }
    return decoded;
  }

  /// `GET /v1/packages/{org}/{name}` — package metadata + version list.
  Future<PackageMetadata> getPackage(String org, String name) async =>
      PackageMetadata.fromJson(await _requestJson('GET', packagePath(org, name)));

  /// `GET /v1/packages/{org}/{name}/versions/{version}`.
  Future<VersionMetadata> getVersion(
          String org, String name, String version) async =>
      VersionMetadata.fromJson(
          await _requestJson('GET', versionPath(org, name, version)));

  /// `GET /v1/search?q=`.
  Future<SearchResponse> search(String query) async => SearchResponse.fromJson(
      await _requestJson('GET', '/v1/search?q=${Uri.encodeQueryComponent(query)}'));

  /// `POST /v1/orgs` (bearer token).
  Future<ClaimOrgResponse> claimOrg(String slug) async =>
      ClaimOrgResponse.fromJson(await _requestJson(
        'POST',
        '/v1/orgs',
        body: {'slug': slug},
        authorized: true,
      ));

  /// `POST .../versions/{version}/yank` — yank (`true`) or restore (`false`)
  /// a published version. Requires a bearer token with publish rights.
  Future<YankResponse> yank(
          String org, String name, String version, bool yanked) async =>
      YankResponse.fromJson(await _requestJson(
        'POST',
        yankPath(org, name, version),
        body: {'yanked': yanked},
        authorized: true,
      ));

  /// Download an artifact, verify its sha256, and return the bytes.
  Future<Uint8List> downloadArtifact(VersionMetadata version) async {
    // An absolute url (any scheme) must clear the scheme/host policy; a bare
    // path is resolved against the trusted registry base.
    final url = version.downloadUrl.contains('://')
        ? allowedDownloadUrl(version.downloadUrl, base)
        : '$base${artifactPath(version.sha256)}';
    // Deliberately no auth header: download_url may point at a third-party
    // host (e.g. a presigned S3/R2 url) and the token must not leak there.
    final request = http.Request('GET', Uri.parse(url));
    final streamed = await _http.send(request).timeout(_timeout);
    final limit = downloadLimit(version.size);
    final declared = streamed.contentLength;
    if (declared != null && declared > limit) {
      throw ZedApiError(
          0, 'artifact_too_large', 'artifact exceeded $limit bytes; refusing');
    }
    // Stream the body, refusing once more than `limit` bytes arrive, so an
    // over-limit body is detected without buffering the whole thing.
    final builder = BytesBuilder(copy: false);
    await for (final chunk in streamed.stream.timeout(_timeout)) {
      builder.add(chunk);
      if (builder.length > limit) {
        throw ZedApiError(0, 'artifact_too_large',
            'artifact exceeded $limit bytes; refusing');
      }
    }
    final bytes = builder.takeBytes();
    if (streamed.statusCode < 200 || streamed.statusCode >= 300) {
      _throwApiError(streamed.statusCode, bytes);
    }
    verifySha256(bytes, version.sha256);
    return bytes;
  }

  /// Publish: multipart `meta` (PublishMeta JSON) + `artifact` bytes.
  /// Requires a bearer token. `meta` must carry
  /// `manifest.package.{org,name,version}` as in the contract.
  Future<PublishResponse> publish(
      Map<String, dynamic> meta, List<int> artifact) async {
    final manifest = meta['manifest'];
    final package = manifest is Map<String, dynamic> ? manifest['package'] : null;
    if (package is! Map<String, dynamic>) {
      throw ZedApiError(
          0, 'invalid_publish_meta', 'meta.manifest.package is required');
    }
    final org = package['org'] as String;
    final name = package['name'] as String;
    final version = package['version'] as String;
    final request =
        http.MultipartRequest('PUT', Uri.parse('$base${versionPath(org, name, version)}'))
          ..headers.addAll(_headers(authorized: true))
          ..fields['meta'] = jsonEncode(meta)
          ..files.add(http.MultipartFile.fromBytes(
            'artifact',
            artifact,
            filename: '$org-$name-$version.tar.gz',
          ));
    final streamed = await _http.send(request).timeout(_timeout);
    final response = await http.Response.fromStream(streamed).timeout(_timeout);
    if (response.statusCode < 200 || response.statusCode >= 300) {
      _throwApiError(response.statusCode, response.bodyBytes);
    }
    return PublishResponse.fromJson(
        jsonDecode(utf8.decode(response.bodyBytes)) as Map<String, dynamic>);
  }
}
