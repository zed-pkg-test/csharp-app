import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  ZedClient,
  ZedApiError,
  MAX_ARTIFACT_BYTES,
  packagePath,
  versionPath,
  filePath,
} from "../dist/index.js";

function makeVersion(overrides = {}) {
  return {
    org: "acme",
    name: "kit",
    version: "1.2.0",
    sha256: "",
    size: 0,
    format: "tar.gz",
    vcs_tag: "v1.2.0",
    download_url: "",
    published_at: "2024-01-01T00:00:00Z",
    yanked: false,
    ...overrides,
  };
}

const sha256Hex = (bytes) => createHash("sha256").update(bytes).digest("hex");

test("url helpers match the contract", () => {
  assert.equal(packagePath("acme", "kit"), "/v1/packages/acme/kit");
  assert.equal(versionPath("acme", "kit", "1.2.0"), "/v1/packages/acme/kit/versions/1.2.0");
  assert.equal(filePath("acme", "kit", "1.2.0", "dist/x.css"), "/v1/files/acme/kit/1.2.0/dist/x.css");
});

test("path segments are percent-encoded", () => {
  assert.equal(
    versionPath("acme", "kit", "release candidate/1"),
    "/v1/packages/acme/kit/versions/release%20candidate%2F1",
  );
  assert.equal(packagePath("a?b", "c#d"), "/v1/packages/a%3Fb/c%23d");
  assert.equal(
    filePath("acme", "kit", "v/2", "dist/a b.css"),
    "/v1/files/acme/kit/v%2F2/dist/a%20b.css",
  );
});

test("errors carry the registry code", async () => {
  const fakeFetch = async () =>
    new Response(JSON.stringify({ code: "org_taken", message: "nope" }), { status: 409 });
  const client = new ZedClient({ registryUrl: "https://x.test///", fetchImpl: fakeFetch });
  await assert.rejects(
    () => client.claimOrg("acme"),
    (err) => err instanceof ZedApiError && err.code === "org_taken" && err.status === 409,
  );
});

test("error code falls back to http_<status> when the JSON body lacks one", async () => {
  const fakeFetch = async () =>
    new Response(JSON.stringify({ message: "boom" }), { status: 500 });
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  await assert.rejects(
    () => client.search("x"),
    (err) =>
      err instanceof ZedApiError &&
      err.code === "http_500" &&
      err.status === 500 &&
      err.message.includes("boom"),
  );
});

test("non-JSON error bodies keep code unknown and the raw text", async () => {
  const fakeFetch = async () => new Response("bad gateway", { status: 502 });
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  await assert.rejects(
    () => client.search("x"),
    (err) =>
      err instanceof ZedApiError && err.code === "unknown" && err.message.includes("bad gateway"),
  );
});

test("bearer token is attached", async () => {
  let seen;
  const fakeFetch = async (url, init) => {
    seen = new Headers(init.headers).get("authorization");
    return new Response(JSON.stringify({ query: "", items: [] }), { status: 200 });
  };
  const client = new ZedClient({ registryUrl: "https://x.test", token: "zpkg_t", fetchImpl: fakeFetch });
  await client.search("http");
  assert.equal(seen, "Bearer zpkg_t");
});

test("download omits the auth token from the download request", async () => {
  const body = new TextEncoder().encode("artifact-bytes");
  let authSeen = "sentinel";
  const fakeFetch = async (url, init) => {
    authSeen = new Headers(init?.headers).get("authorization");
    return new Response(body, { status: 200 });
  };
  const client = new ZedClient({
    registryUrl: "https://x.test",
    token: "zpkg_t",
    fetchImpl: fakeFetch,
  });
  const version = makeVersion({
    sha256: sha256Hex(body),
    size: body.length,
    download_url: "https://cdn.example/artifact",
  });
  const out = await client.downloadArtifact(version);
  assert.equal(authSeen, null); // no Authorization header on the download
  assert.deepEqual(out, body);
});

test("download rejects insecure download_url schemes", async () => {
  const fakeFetch = async () => {
    throw new Error("fetch should not run for a rejected url");
  };
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  for (const url of ["http://evil.example/artifact", "file:///etc/passwd"]) {
    await assert.rejects(
      () => client.downloadArtifact(makeVersion({ download_url: url })),
      (err) => err instanceof ZedApiError && err.code === "insecure_download_url",
    );
  }
});

test("download allows loopback http download_url", async () => {
  const body = new TextEncoder().encode("ok");
  const fakeFetch = async () => new Response(body, { status: 200 });
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  const version = makeVersion({
    sha256: sha256Hex(body),
    size: body.length,
    download_url: "http://127.0.0.1:8080/artifact",
  });
  const out = await client.downloadArtifact(version);
  assert.deepEqual(out, body);
});

test("download rejects a body larger than the cap (streamed)", async () => {
  // size 1 -> limit = 1 + 1 MiB slack; serve just over it, no content-length.
  const limit = 1 + 1024 * 1024;
  const big = new Uint8Array(limit + 64);
  const fakeFetch = async () => new Response(big, { status: 200 });
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  const version = makeVersion({
    sha256: sha256Hex(big),
    size: 1,
    download_url: "https://cdn.example/artifact",
  });
  await assert.rejects(
    () => client.downloadArtifact(version),
    (err) => err instanceof ZedApiError && err.code === "artifact_too_large",
  );
});

test("download rejects an oversized content-length before reading", async () => {
  const body = new TextEncoder().encode("small");
  const fakeFetch = async () =>
    new Response(body, {
      status: 200,
      headers: { "content-length": String(MAX_ARTIFACT_BYTES + 1) },
    });
  const client = new ZedClient({ registryUrl: "https://x.test", fetchImpl: fakeFetch });
  await assert.rejects(
    () =>
      client.downloadArtifact(
        makeVersion({ sha256: sha256Hex(body), download_url: "https://cdn.example/artifact" }),
      ),
    (err) => err instanceof ZedApiError && err.code === "artifact_too_large",
  );
});
