// Package zedclient is the Go SDK for the zed-pkg registry. It is stdlib only,
// transports bearer credentials without parsing them, never retries writes, and
// refuses redirects so credentials cannot be replayed to another target.
package zedclient

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
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
const DefaultTimeout = 30 * time.Second

const (
	maxArtifactBytes     = 100 * 1024 * 1024
	downloadSlack        = 1024 * 1024
	maxJSONResponseBytes = 16 * 1024 * 1024
	maxErrorBodyBytes    = 16 * 1024
)

var errRefuseRedirect = errors.New("zed registry client refuses redirects")

func downloadLimit(size uint64) uint64 {
	if size == 0 {
		return maxArtifactBytes
	}
	limit := size + downloadSlack
	if limit < size || limit > maxArtifactBytes {
		return maxArtifactBytes
	}
	return limit
}

// APIError carries the registry's stable error code. Message is bounded and
// explicitly inspectable; Error deliberately excludes arbitrary remote text.
type APIError struct {
	Status  int
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *APIError) Error() string {
	return fmt.Sprintf("registry error %d: %s", e.Status, e.Code)
}

type PackageSummary struct {
	Org         string   `json:"org"`
	Name        string   `json:"name"`
	Description *string  `json:"description,omitempty"`
	Latest      *string  `json:"latest,omitempty"`
	Tags        []string `json:"tags,omitempty"`
}

type PackageMetadata struct {
	Org           string   `json:"org"`
	Name          string   `json:"name"`
	Description   *string  `json:"description,omitempty"`
	Vcs           string   `json:"vcs"`
	RepoURL       string   `json:"repo_url"`
	Latest        *string  `json:"latest,omitempty"`
	Tags          []string `json:"tags,omitempty"`
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

type YankResponse struct {
	Org     string `json:"org"`
	Name    string `json:"name"`
	Version string `json:"version"`
	Yanked  bool   `json:"yanked"`
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
	return fmt.Sprintf(
		"/v1/packages/%s/%s/versions/%s",
		url.PathEscape(org),
		url.PathEscape(name),
		url.PathEscape(version),
	)
}

func YankPath(org, name, version string) string {
	return VersionPath(org, name, version) + "/yank"
}

func ArtifactPath(sha256 string) string {
	return "/v1/artifacts/" + url.PathEscape(sha256)
}

type Client struct {
	Base  string
	Token string
	HTTP  *http.Client
}

func New(base string) (*Client, error) {
	return NewWithHTTPClient(base, nil)
}

func NewWithHTTPClient(base string, supplied *http.Client) (*Client, error) {
	normalized, err := normalizeRegistryURL(base)
	if err != nil {
		return nil, err
	}
	client := &http.Client{Timeout: DefaultTimeout}
	if supplied != nil {
		clone := *supplied
		client = &clone
		if client.Timeout == 0 {
			client.Timeout = DefaultTimeout
		}
	}
	client.CheckRedirect = func(_ *http.Request, _ []*http.Request) error {
		return errRefuseRedirect
	}
	return &Client{Base: normalized, HTTP: client}, nil
}

func (c *Client) String() string {
	return fmt.Sprintf("ZedClient(base=%s, token=[REDACTED])", c.Base)
}

func normalizeRegistryURL(raw string) (string, error) {
	trimmed := strings.TrimSpace(raw)
	parsed, err := url.Parse(trimmed)
	if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") ||
		parsed.Host == "" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", fmt.Errorf(
			"registry URL must be a credential-free absolute HTTP(S) URL without query or fragment",
		)
	}
	parsed.Path = strings.TrimRight(parsed.Path, "/")
	return strings.TrimRight(parsed.String(), "/"), nil
}

func (c *Client) allowedDownloadURL(raw string) (string, error) {
	parsed, err := url.Parse(raw)
	if err != nil || parsed.User != nil || parsed.Fragment != "" {
		return "", &APIError{Code: "bad_download_url", Message: "download URL is invalid"}
	}
	host := parsed.Hostname()
	loopback := host == "localhost"
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		loopback = true
	}
	switch parsed.Scheme {
	case "https":
		return parsed.String(), nil
	case "http":
		if loopback || strings.HasPrefix(c.Base, "http://") {
			return parsed.String(), nil
		}
	}
	return "", &APIError{
		Code:    "insecure_download_url",
		Message: fmt.Sprintf("refusing artifact download over %q", parsed.Scheme),
	}
}

func (c *Client) resolveDownloadURL(raw, sha256 string) (string, error) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return c.Base + ArtifactPath(sha256), nil
	}
	parsed, err := url.Parse(trimmed)
	if err != nil {
		return "", &APIError{Code: "bad_download_url", Message: "download URL is invalid"}
	}
	if !parsed.IsAbs() {
		base, parseErr := url.Parse(c.Base + "/")
		if parseErr != nil {
			return "", parseErr
		}
		parsed = base.ResolveReference(parsed)
	}
	return c.allowedDownloadURL(parsed.String())
}

func (c *Client) do(
	method string,
	path string,
	body io.Reader,
	contentType string,
	authorized bool,
	out any,
) error {
	req, err := http.NewRequest(method, c.Base+path, body)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/json")
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	if authorized && c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+strings.TrimSpace(c.Token))
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		payload, readErr := readBounded(resp.Body, maxErrorBodyBytes, false)
		if readErr != nil {
			return readErr
		}
		return newAPIError(resp.StatusCode, payload)
	}
	payload, err := readBounded(resp.Body, maxJSONResponseBytes, true)
	if err != nil {
		return err
	}
	if out != nil {
		if err := json.Unmarshal(payload, out); err != nil {
			return fmt.Errorf("decode registry response: %w", err)
		}
	}
	return nil
}

func readBounded(reader io.Reader, limit int, failOnOverflow bool) ([]byte, error) {
	payload, err := io.ReadAll(io.LimitReader(reader, int64(limit+1)))
	if err != nil {
		return nil, err
	}
	if len(payload) > limit {
		if failOnOverflow {
			return nil, fmt.Errorf("registry response exceeded %d bytes", limit)
		}
		payload = payload[:limit]
	}
	return payload, nil
}

func newAPIError(status int, payload []byte) *APIError {
	apiErr := &APIError{Status: status, Code: fmt.Sprintf("http_%d", status), Message: string(payload)}
	var decoded struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	}
	if json.Unmarshal(payload, &decoded) == nil {
		if decoded.Code != "" {
			apiErr.Code = decoded.Code
		}
		if decoded.Message != "" {
			apiErr.Message = decoded.Message
		}
	}
	return apiErr
}

func (c *Client) GetPackage(org, name string) (*PackageMetadata, error) {
	var out PackageMetadata
	if err := c.do(http.MethodGet, PackagePath(org, name), nil, "", false, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) GetVersion(org, name, version string) (*VersionMetadata, error) {
	var out VersionMetadata
	if err := c.do(http.MethodGet, VersionPath(org, name, version), nil, "", false, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) Search(query string) (*SearchResponse, error) {
	var out SearchResponse
	path := "/v1/search?q=" + url.QueryEscape(query)
	if err := c.do(http.MethodGet, path, nil, "", false, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) ClaimOrg(slug string) (*ClaimOrgResponse, error) {
	body, err := json.Marshal(map[string]string{"slug": slug})
	if err != nil {
		return nil, err
	}
	var out ClaimOrgResponse
	if err := c.do(http.MethodPost, "/v1/orgs", bytes.NewReader(body), "application/json", true, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) Yank(org, name, version string, yanked bool) (*YankResponse, error) {
	body, err := json.Marshal(map[string]bool{"yanked": yanked})
	if err != nil {
		return nil, err
	}
	var out YankResponse
	if err := c.do(
		http.MethodPost,
		YankPath(org, name, version),
		bytes.NewReader(body),
		"application/json",
		true,
		&out,
	); err != nil {
		return nil, err
	}
	return &out, nil
}

func (c *Client) DownloadArtifact(version *VersionMetadata, destPath string) error {
	target, err := c.resolveDownloadURL(version.DownloadURL, version.Sha256)
	if err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodGet, target, nil)
	if err != nil {
		return err
	}
	// Deliberately no Authorization header: target may be a third-party
	// presigned URL and registry credentials must never leave the registry.
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	limit := downloadLimit(version.Size)
	if resp.ContentLength > int64(limit) {
		return &APIError{Code: "artifact_too_large", Message: fmt.Sprintf("artifact exceeded %d bytes", limit)}
	}
	payload, err := readBounded(resp.Body, int(limit), true)
	if err != nil {
		return &APIError{Code: "artifact_too_large", Message: err.Error()}
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		if len(payload) > maxErrorBodyBytes {
			payload = payload[:maxErrorBodyBytes]
		}
		return newAPIError(resp.StatusCode, payload)
	}
	sum := sha256.Sum256(payload)
	if actual := hex.EncodeToString(sum[:]); actual != version.Sha256 {
		return &APIError{Code: "sha256_mismatch", Message: fmt.Sprintf("expected %s, got %s", version.Sha256, actual)}
	}
	return os.WriteFile(destPath, payload, 0o644)
}

// Publish uploads multipart meta JSON plus raw artifact bytes. It does not retry.
func (c *Client) Publish(
	org string,
	name string,
	version string,
	metaJSON []byte,
	artifactPath string,
) (*PublishResponse, error) {
	var buffer bytes.Buffer
	writer := multipart.NewWriter(&buffer)
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
	if err := c.do(
		http.MethodPut,
		VersionPath(org, name, version),
		&buffer,
		writer.FormDataContentType(),
		true,
		&out,
	); err != nil {
		return nil, err
	}
	return &out, nil
}
