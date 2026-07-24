// Package zedclient is the Go SDK for the zed-pkg registry. Stdlib only;
// structs mirror the JSON Schemas in zed-interfaces/schemas/.
package zedclient

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

const DefaultRegistryURL = "https://registry.zpkg.tech"

// DefaultTimeout bounds each HTTP request (connect + read).
const DefaultTimeout = 30 * time.Second

const (
	// maxArtifactBytes is the hard ceiling on artifact downloads, matching the
	// server's MAX_ARTIFACT_BYTES default (100 MiB).
	maxArtifactBytes = 100 * 1024 * 1024
	// downloadSlack is the allowance added to a version's declared size.
	downloadSlack = 1024 * 1024
)

// downloadLimit bounds an artifact read: the declared size (when sane) plus
// slack, capped by the static ceiling. A zero/unknown size falls back to the
// ceiling.
func downloadLimit(size uint64) uint64 {
	if size == 0 {
		return maxArtifactBytes
	}
	limit := size + downloadSlack
	if limit < size || limit > maxArtifactBytes { // overflow or over ceiling
		return maxArtifactBytes
	}
	return limit
}

// APIError carries the registry's stable error code.
type APIError struct {
	Status  int
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *APIError) Error() string {
	return fmt.Sprintf("registry error %d: %s: %s", e.Status, e.Code, e.Message)
}

type PackageSummary struct {
	Org         string  `json:"org"`
	Name        string  `json:"name"`
	Description *string `json:"description,omitempty"`
	Latest      *string `json:"latest,omitempty"`
}

type PackageMetadata struct {
	Org           string   `json:"org"`
	Name          string   `json:"name"`
	Description   *string  `json:"description,omitempty"`
	Vcs           string   `json:"vcs"`
	RepoURL       string   `json:"repo_url"`
	Latest        *string  `json:"latest,omitempty"`
	Versions      []string `json:"versions"`
	VersionScheme string   `json:"version_scheme,omitempty"`
}

type VersionMetadata struct {
	Org         string  `json:"org"`
	Name        string  `json:"name"`
	Version     string  `json:"version"`
	Sha256      string  `json:"sha256"`
	Size        uint64  `json:"size"`
	Format      string  `json:"format"`
	VcsTag      string  `json:"vcs_tag"`
	VcsCommit   *string `json:"vcs_commit,omitempty"`
	DownloadURL string  `json:"download_url"`
	PublishedAt string  `json:"published_at"`
	Yanked      bool    `json:"yanked"`
}

type SearchResponse struct {
	Query string           `json:"query"`
	Items []PackageSummary `json:"items"`
}

type ClaimOrgResponse struct {
	Slug    string `json:"slug"`
	Created bool   `json:"created"`
}

type PublishResponse struct {
	Org     string `json:"org"`
	Name    string `json:"name"`
	Version string `json:"version"`
	Sha256  string `json:"sha256"`
}

func PackagePath(org, name string) string {
	return fmt.Sprintf("/v1/packages/%s/%s", url.PathEscape(org), url.PathEscape(name))
}

func VersionPath(org, name, version string) string {
	return fmt.Sprintf("/v1/packages/%s/%s/versions/%s", url.PathEscape(org), url.PathEscape(name), url.PathEscape(version))
}

func ArtifactPath(sha256 string) string {
	return "/v1/artifacts/" + url.PathEscape(sha256)
}

type Client struct {
	Base  string
	Token string
	HTTP  *http.Client
}

func New(base string) *Client {
	return &Client{Base: strings.TrimRight(base, "/"), HTTP: &http.Client{Timeout: DefaultTimeout}}
}

// allowedDownloadURL enforces the download-url scheme policy: https is always
// allowed; http only for loopback hosts or when the registry base is itself
// http (the operator already accepted plaintext for that registry). A
// malicious registry response must not be able to redirect fetches to
// plaintext or unexpected hosts.
func (c *Client) allowedDownloadURL(raw string) (string, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return "", &APIError{Code: "bad_download_url", Message: fmt.Sprintf("bad download url %q: %v", raw, err)}
	}
	host := u.Hostname()
	loopback := host == "localhost"
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		loopback = true
	}
	switch u.Scheme {
	case "https":
		return raw, nil
	case "http":
		if loopback || strings.HasPrefix(c.Base, "http://") {
			return raw, nil
		}
	}
	return "", &APIError{
		Code:    "insecure_download_url",
		Message: fmt.Sprintf("refusing artifact download over %q from %s (https required for non-local registries)", u.Scheme, raw),
	}
}

func (c *Client) do(method, path string, body io.Reader, contentType string, out any) error {
	req, err := http.NewRequest(method, c.Base+path, body)
	if err != nil {
		return err
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	payload, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	if resp.StatusCode >= 400 {
		return newAPIError(resp.StatusCode, payload)
	}
	if out != nil {
		return json.Unmarshal(payload, out)
	}
	return nil
}

// newAPIError builds the typed error from an error-response body, keeping the
// "unknown" code when the body is not ApiError JSON.
func newAPIError(status int, payload []byte) *APIError {
	apiErr := &APIError{Status: status, Code: "unknown", Message: string(payload)}
	_ = json.Unmarshal(payload, apiErr)
	apiErr.Status = status
	return apiErr
}

func (c *Client) GetPackage(org, name string) (*PackageMetadata, error) {
	var out PackageMetadata
	if err := c.do(http.MethodGet, PackagePath(org, name), nil, "", &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) GetVersion(org, name, version string) (*VersionMetadata, error) {
	var out VersionMetadata
	if err := c.do(http.MethodGet, VersionPath(org, name, version), nil, "", &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) Search(query string) (*SearchResponse, error) {
	var out SearchResponse
	path := "/v1/search?q=" + url.QueryEscape(query)
	if err := c.do(http.MethodGet, path, nil, "", &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) ClaimOrg(slug string) (*ClaimOrgResponse, error) {
	body, _ := json.Marshal(map[string]string{"slug": slug})
	var out ClaimOrgResponse
	if err := c.do(http.MethodPost, "/v1/orgs", bytes.NewReader(body), "application/json", &out); err != nil {
		return nil, err
	}
	return &out, nil
}

// DownloadArtifact fetches and sha256-verifies an artifact to destPath.
func (c *Client) DownloadArtifact(v *VersionMetadata, destPath string) error {
	target := v.DownloadURL
	// An absolute url (any scheme) must clear the scheme/host policy; a bare
	// path is resolved against the trusted registry base.
	if strings.Contains(target, "://") {
		validated, err := c.allowedDownloadURL(target)
		if err != nil {
			return err
		}
		target = validated
	} else {
		target = c.Base + ArtifactPath(v.Sha256)
	}
	req, err := http.NewRequest(http.MethodGet, target, nil)
	if err != nil {
		return err
	}
	// Deliberately no Authorization header: download_url may point at a
	// third-party host (e.g. a presigned S3/R2 URL from the registry
	// response), and sending the bearer token there would leak it.
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	// Bound the read to guard against OOM / disk exhaustion; read one extra
	// byte so an over-limit body can be detected.
	limit := downloadLimit(v.Size)
	payload, err := io.ReadAll(io.LimitReader(resp.Body, int64(limit)+1))
	if err != nil {
		return err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return newAPIError(resp.StatusCode, payload)
	}
	if uint64(len(payload)) > limit {
		return &APIError{Code: "artifact_too_large", Message: fmt.Sprintf("artifact exceeded %d bytes; refusing", limit)}
	}
	sum := sha256.Sum256(payload)
	if actual := hex.EncodeToString(sum[:]); actual != v.Sha256 {
		return &APIError{Code: "sha256_mismatch", Message: fmt.Sprintf("expected %s, got %s", v.Sha256, actual)}
	}
	return os.WriteFile(destPath, payload, 0o644)
}

// Publish uploads multipart meta (PublishMeta JSON) + the artifact file.
func (c *Client) Publish(org, name, version string, metaJSON []byte, artifactPath string) (*PublishResponse, error) {
	var buf bytes.Buffer
	writer := multipart.NewWriter(&buf)
	if err := writer.WriteField("meta", string(metaJSON)); err != nil {
		return nil, err
	}
	part, err := writer.CreateFormFile("artifact", "artifact.tar.gz")
	if err != nil {
		return nil, err
	}
	file, err := os.Open(artifactPath)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	if _, err := io.Copy(part, file); err != nil {
		return nil, err
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	var out PublishResponse
	if err := c.do(http.MethodPut, VersionPath(org, name, version), &buf, writer.FormDataContentType(), &out); err != nil {
		return nil, err
	}
	return &out, nil
}
