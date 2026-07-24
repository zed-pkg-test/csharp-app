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
	"net/http"
	"net/url"
	"os"
	"strings"
)

const DefaultRegistryURL = "https://registry.zpkg.tech"

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
	return &Client{Base: strings.TrimRight(base, "/"), HTTP: http.DefaultClient}
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
	if !strings.HasPrefix(target, "http") {
		target = c.Base + ArtifactPath(v.Sha256)
	}
	req, err := http.NewRequest(http.MethodGet, target, nil)
	if err != nil {
		return err
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
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return newAPIError(resp.StatusCode, payload)
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
