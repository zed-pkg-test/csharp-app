import test from "node:test";
import assert from "node:assert/strict";
import {
  ZedClient,
  ZedApiError,
  packagePath,
  versionPath,
  filePath,
} from "../dist/index.js";

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
