import type {
  ApiErrorBody,
  ClaimOrgResponse,
  PackageMetadata,
  PublishResponse,
  SearchResponse,
  VersionMetadata,
  YankResponse,
} from "./types.js";

export const DEFAULT_REGISTRY_URL = "https://registry.zpkg.tech";

/** Bounds every request (connect + read), in milliseconds. */
export const DEFAULT_TIMEOUT_MS = 30_000;

/** Successful JSON documents are never allowed to grow without bound. */
export const MAX_JSON_RESPONSE_BYTES = 16 * 1024 * 1024;

/** Remote error text is retained only through this bounded explicit field. */
export const MAX_ERROR_BODY_BYTES = 16 * 1024;

/**
 * Hard ceiling on artifact downloads, matching the server's MAX_ARTIFACT_BYTES
 * default (100 MiB); plus the slack added to a version's declared size.
 */
export const MAX_ARTIFACT_BYTES = 100 * 1024 * 1024;
const DOWNLOAD_SLACK = 1024 * 1024;

/** The declared size (when sane) plus slack, capped by the ceiling. */
function downloadLimit(size: number): number {
  if (Number.isFinite(size) && size > 0) {
    return Math.min(size + DOWNLOAD_SLACK, MAX_ARTIFACT_BYTES);
  }
  return MAX_ARTIFACT_BYTES;
}

interface BoundedBytes {
  bytes: Uint8Array<ArrayBuffer>;
  truncated: boolean;
}

/** Stream no more than `limit` bytes, cancelling once the bound is exceeded. */
async function readAtMost(response: Response, limit: number): Promise<BoundedBytes> {
  const body = response.body;
  if (!body) {
    const source = new Uint8Array(await response.arrayBuffer());
    return {
      bytes: source.slice(0, limit),
      truncated: source.byteLength > limit,
    };
  }

  const reader = body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  let truncated = false;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (!value) continue;
    const remaining = limit - total;
    if (value.byteLength > remaining) {
      if (remaining > 0) chunks.push(value.slice(0, remaining));
      total = limit;
      truncated = true;
      await reader.cancel();
      break;
    }
    chunks.push(value);
    total += value.byteLength;
  }

  const output = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    output.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return { bytes: output, truncated };
}

async function readCapped(
  response: Response,
  limit: number,
  code: string,
  description: string,
): Promise<Uint8Array<ArrayBuffer>> {
  const declared = response.headers.get("content-length");
  if (declared !== null && Number(declared) > limit) {
    throw new ZedApiError(0, code, `${description} exceeded ${limit} bytes; refusing`);
  }
  const bounded = await readAtMost(response, limit);
  if (bounded.truncated) {
    throw new ZedApiError(0, code, `${description} exceeded ${limit} bytes; refusing`);
  }
  return bounded.bytes;
}

/**
 * Registry failures keep bounded remote text in `registryMessage`, while the
 * ordinary Error message exposes only status and stable machine-readable code.
 */
export class ZedApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    public readonly registryMessage: string,
  ) {
    super(`registry error ${status}: ${code}`);
    this.name = "ZedApiError";
  }
}

export interface ClientOptions {
  registryUrl?: string;
  token?: string;
  fetchImpl?: typeof fetch;
  /** Per-request timeout in milliseconds (default {@link DEFAULT_TIMEOUT_MS}). */
  timeoutMs?: number;
}

/** Validate and normalize one registry base URL while preserving a path prefix. */
export function normalizeRegistryUrl(raw: string): string {
  let url: URL;
  try {
    url = new URL(raw.trim());
  } catch {
    throw new TypeError("registryUrl must be an absolute HTTP(S) URL");
  }
  if (
    !["http:", "https:"].includes(url.protocol) ||
    url.hostname === "" ||
    url.username !== "" ||
    url.password !== "" ||
    url.search !== "" ||
    url.hash !== ""
  ) {
    throw new TypeError(
      "registryUrl must be a credential-free absolute HTTP(S) URL without query or fragment",
    );
  }
  url.pathname = url.pathname.replace(/\/+$/, "");
  return url.toString().replace(/\/$/, "");
}

/**
 * Enforce the download-url scheme policy: https is always allowed; http only
 * for loopback hosts or when the registry base is itself http. Userinfo is
 * rejected. Query strings remain allowed because S3/R2 presigned URLs need
 * them. No registry bearer token is ever attached to this URL.
 */
export function allowedDownloadUrl(raw: string, base: string): string {
  let url: URL;
  try {
    url = new URL(raw);
  } catch {
    throw new ZedApiError(0, "bad_download_url", `bad download url ${raw}`);
  }
  if (url.username !== "" || url.password !== "" || url.hash !== "") {
    throw new ZedApiError(0, "bad_download_url", "download URL contains credentials or fragment");
  }
  const host = url.hostname;
  const loopback =
    host === "localhost" || host === "127.0.0.1" || host === "[::1]" || host === "::1";
  if (url.protocol === "https:") return url.toString();
  if (url.protocol === "http:" && (loopback || base.startsWith("http://"))) {
    return url.toString();
  }
  throw new ZedApiError(
    0,
    "insecure_download_url",
    `refusing artifact download over ${url.protocol} from ${raw}`,
  );
}

export function packagePath(org: string, name: string): string {
  return `/v1/packages/${encodeURIComponent(org)}/${encodeURIComponent(name)}`;
}

export function versionPath(org: string, name: string, version: string): string {
  return `/v1/packages/${encodeURIComponent(org)}/${encodeURIComponent(name)}/versions/${encodeURIComponent(version)}`;
}

export function yankPath(org: string, name: string, version: string): string {
  return `${versionPath(org, name, version)}/yank`;
}

export function artifactPath(sha256: string): string {
  return `/v1/artifacts/${encodeURIComponent(sha256)}`;
}

export function filePath(org: string, name: string, version: string, path: string): string {
  const encodedPath = path.split("/").map(encodeURIComponent).join("/");
  return `/v1/files/${encodeURIComponent(org)}/${encodeURIComponent(name)}/${encodeURIComponent(version)}/${encodedPath}`;
}

export class ZedClient {
  private readonly base: string;
  private readonly token: string | undefined;
  private readonly fetchImpl: typeof fetch;
  private readonly timeoutMs: number;

  constructor(options: ClientOptions = {}) {
    this.base = normalizeRegistryUrl(options.registryUrl ?? DEFAULT_REGISTRY_URL);
    this.token = options.token?.trim() || undefined;
    this.fetchImpl = options.fetchImpl ?? fetch;
    this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    if (!Number.isFinite(this.timeoutMs) || this.timeoutMs <= 0) {
      throw new TypeError("timeoutMs must be a positive finite number");
    }
  }

  private async request<T>(
    path: string,
    init: RequestInit = {},
    authorized = false,
  ): Promise<T> {
    const headers = new Headers(init.headers);
    headers.set("accept", "application/json");
    if (authorized && this.token) headers.set("authorization", `Bearer ${this.token}`);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.base}${path}`, {
        ...init,
        headers,
        redirect: "error",
        signal: controller.signal,
      });
    } finally {
      clearTimeout(timer);
    }

    if (!response.ok) {
      const bounded = await readAtMost(response, MAX_ERROR_BODY_BYTES);
      const text = new TextDecoder().decode(bounded.bytes);
      let body: ApiErrorBody = {
        code: `http_${response.status}`,
        message: text,
      };
      try {
        const parsed = JSON.parse(text) as Partial<ApiErrorBody> | null;
        body = {
          code: typeof parsed?.code === "string" ? parsed.code : `http_${response.status}`,
          message: typeof parsed?.message === "string" ? parsed.message : text,
        };
      } catch {
        // Non-JSON error body remains available only through registryMessage.
      }
      if (bounded.truncated) body.message += "…";
      throw new ZedApiError(response.status, body.code, body.message);
    }

    const bytes = await readCapped(
      response,
      MAX_JSON_RESPONSE_BYTES,
      "response_too_large",
      "registry JSON response",
    );
    try {
      return JSON.parse(new TextDecoder().decode(bytes)) as T;
    } catch (error) {
      throw new ZedApiError(0, "invalid_response", `invalid registry JSON: ${String(error)}`);
    }
  }

  getPackage(org: string, name: string): Promise<PackageMetadata> {
    return this.request(packagePath(org, name));
  }

  getVersion(org: string, name: string, version: string): Promise<VersionMetadata> {
    return this.request(versionPath(org, name, version));
  }

  search(query: string): Promise<SearchResponse> {
    return this.request(`/v1/search?q=${encodeURIComponent(query)}`);
  }

  claimOrg(slug: string): Promise<ClaimOrgResponse> {
    return this.request(
      "/v1/orgs",
      {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ slug }),
      },
      true,
    );
  }

  yank(org: string, name: string, version: string, yanked: boolean): Promise<YankResponse> {
    return this.request(
      yankPath(org, name, version),
      {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ yanked }),
      },
      true,
    );
  }

  /** Download an artifact and verify its sha256 (WebCrypto). */
  async downloadArtifact(version: VersionMetadata): Promise<Uint8Array<ArrayBuffer>> {
    const raw = version.download_url.trim();
    let url: string;
    if (raw === "") {
      url = `${this.base}${artifactPath(version.sha256)}`;
    } else if (raw.includes("://")) {
      url = allowedDownloadUrl(raw, this.base);
    } else {
      url = allowedDownloadUrl(new URL(raw, `${this.base}/`).toString(), this.base);
    }

    const limit = downloadLimit(version.size);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let response: Response;
    try {
      // Deliberately no auth header: download_url may point at a third-party
      // host (e.g. a presigned S3/R2 URL), and the token must not leak there.
      response = await this.fetchImpl(url, {
        redirect: "error",
        signal: controller.signal,
      });
    } finally {
      clearTimeout(timer);
    }
    if (!response.ok) {
      const bounded = await readAtMost(response, MAX_ERROR_BODY_BYTES);
      throw new ZedApiError(
        response.status,
        "download_failed",
        new TextDecoder().decode(bounded.bytes),
      );
    }
    const bytes = await readCapped(response, limit, "artifact_too_large", "artifact");
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    const actual = [...new Uint8Array(digest)]
      .map((byte) => byte.toString(16).padStart(2, "0"))
      .join("");
    if (actual !== version.sha256) {
      throw new ZedApiError(0, "sha256_mismatch", `expected ${version.sha256}, got ${actual}`);
    }
    return bytes;
  }

  /** Publish: multipart meta (PublishMeta JSON) + artifact bytes. */
  async publish(
    meta: { manifest: { package: { org: string; name: string; version: string } } } & Record<
      string,
      unknown
    >,
    artifact: Blob,
  ): Promise<PublishResponse> {
    const pkg = meta.manifest.package;
    const form = new FormData();
    form.set("meta", JSON.stringify(meta));
    form.set("artifact", artifact, `${pkg.org}-${pkg.name}-${pkg.version}.tar.gz`);
    return this.request(
      versionPath(pkg.org, pkg.name, pkg.version),
      {
        method: "PUT",
        body: form,
      },
      true,
    );
  }
}
