# zed-clients

Official client SDKs for the [zed-pkg](https://zpkg.tech) registry API, one
folder per language under `clients/`. All of them speak the contract defined
in [zed-interfaces](https://github.com/zed-pkg/zed-interfaces) (the Rust and
WebAssembly SDKs reuse its types directly; the others mirror the generated
JSON Schemas in `zed-interfaces/schemas/`).

| SDK | Package | Deps | Verify |
| --- | --- | --- | --- |
| [clients/rust/](clients/rust/) | `zed-client` | reqwest (rustls) | `cargo test` |
| [clients/wasm/](clients/wasm/) | `@zed-pkg/client-wasm` | wasm-bindgen + global fetch | `wasm-pack build --target web && cargo test` |
| [clients/typescript/](clients/typescript/) | `@zed-pkg/client` | zero runtime deps (global fetch) | `npm run build && npm test` |
| [clients/python/](clients/python/) | `zed-pkg-client` | stdlib only (urllib) | `python3 -m unittest` |
| [clients/go/](clients/go/) | `github.com/zed-pkg/zed-clients/clients/go` | stdlib only (net/http) | `go test ./...` |
| [clients/dart/](clients/dart/) | `zed_pkg_client` | http, crypto | `dart analyze && dart test` |
| [clients/gleam/](clients/gleam/) | `zed_pkg_client` | gleam_http, gleam_httpc, gleam_json, gleam_crypto | `gleam test` |
| [clients/erlang/](clients/erlang/) | `zed_pkg_client` | stdlib only (httpc + json + crypto) | `rebar3 eunit` |

Every SDK implements: `getPackage`, `getVersion`, `search`,
`downloadArtifact` (with sha256 verification), `claimOrg`, and `publish`
(multipart `meta` + `artifact`), with bearer-token auth and typed errors
carrying the registry's `ApiError.code`.

Endpoints (see zed-interfaces README for bodies):

```
GET  /v1/packages/{org}/{name}
GET  /v1/packages/{org}/{name}/versions/{version}
PUT  /v1/packages/{org}/{name}/versions/{version}   multipart, bearer
GET  /v1/artifacts/{sha256}
GET  /v1/search?q=
POST /v1/orgs                                        bearer
GET  /v1/files/{org}/{name}/{version}/{path}
```

## License

MIT
