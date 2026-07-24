# zed-clients

Official client SDKs for the [zed-pkg](https://zpkg.tech) registry API, one
folder per language. All of them speak the contract defined in
[zed-interfaces](https://github.com/zed-pkg/zed-interfaces) (the Rust SDK
reuses its types directly; the others mirror the generated JSON Schemas in
`zed-interfaces/schemas/`).

| SDK | Package | Deps | Verify |
| --- | --- | --- | --- |
| [rust/](rust/) | `zed-client` | reqwest (rustls) | `cargo test` |
| [typescript/](typescript/) | `@zed-pkg/client` | zero runtime deps (global fetch) | `npm run build && npm test` |
| [python/](python/) | `zed-pkg-client` | stdlib only (urllib) | `python3 -m unittest` |
| [go/](go/) | `github.com/zed-pkg/zed-clients/go` | stdlib only (net/http) | `go test ./...` |

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
