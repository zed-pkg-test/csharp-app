package zedclient

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
)

func TestPathsMatchContract(t *testing.T) {
	if got := PackagePath("acme", "kit"); got != "/v1/packages/acme/kit" {
		t.Fatalf("PackagePath = %q", got)
	}
	if got := VersionPath("acme", "kit", "1.2.0"); got != "/v1/packages/acme/kit/versions/1.2.0" {
		t.Fatalf("VersionPath = %q", got)
	}
	if got := ArtifactPath("abc"); got != "/v1/artifacts/abc" {
		t.Fatalf("ArtifactPath = %q", got)
	}
}

func TestPathSegmentsAreEscaped(t *testing.T) {
	if got := VersionPath("acme", "kit", "release candidate/1"); got != "/v1/packages/acme/kit/versions/release%20candidate%2F1" {
		t.Fatalf("VersionPath = %q", got)
	}
	if got := PackagePath("a?b", "c#d"); got != "/v1/packages/a%3Fb/c%23d" {
		t.Fatalf("PackagePath = %q", got)
	}
}

func TestPackageMetadataVersionScheme(t *testing.T) {
	var meta PackageMetadata
	payload := `{"org":"acme","name":"kit","vcs":"git","repo_url":"x","versions":[],"version_scheme":"calver"}`
	if err := json.Unmarshal([]byte(payload), &meta); err != nil {
		t.Fatal(err)
	}
	if meta.VersionScheme != "calver" {
		t.Fatalf("VersionScheme = %q", meta.VersionScheme)
	}
}

func TestDownloadArtifactStatusCheck(t *testing.T) {
	var gotAuth string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"code":"not_found","message":"no such artifact"}`))
	}))
	defer server.Close()

	c := New(server.URL)
	c.Token = "zpkg_t"
	v := &VersionMetadata{Sha256: "abc", DownloadURL: "/v1/artifacts/abc"}
	err := c.DownloadArtifact(v, filepath.Join(t.TempDir(), "artifact.tar.gz"))

	var apiErr *APIError
	if !errors.As(err, &apiErr) {
		t.Fatalf("want *APIError, got %v", err)
	}
	if apiErr.Status != http.StatusNotFound || apiErr.Code != "not_found" || apiErr.Message != "no such artifact" {
		t.Fatalf("apiErr = %+v", apiErr)
	}
	// The bearer token must never travel to the artifact download host.
	if gotAuth != "" {
		t.Fatalf("Authorization = %q, want none", gotAuth)
	}
}

// TestDownloadArtifactOmitsAuthOnSuccess proves no Authorization header reaches
// the download host even for a successful, sha256-verified fetch.
func TestDownloadArtifactOmitsAuthOnSuccess(t *testing.T) {
	body := []byte("artifact-bytes")
	sum := sha256.Sum256(body)
	var gotAuth string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		_, _ = w.Write(body)
	}))
	defer server.Close()

	c := New(server.URL)
	c.Token = "zpkg_t"
	v := &VersionMetadata{Sha256: hex.EncodeToString(sum[:]), Size: uint64(len(body)), DownloadURL: server.URL + "/artifact"}
	if err := c.DownloadArtifact(v, filepath.Join(t.TempDir(), "artifact.tar.gz")); err != nil {
		t.Fatalf("DownloadArtifact: %v", err)
	}
	if gotAuth != "" {
		t.Fatalf("Authorization = %q, want none", gotAuth)
	}
}

func TestDownloadArtifactRejectsInsecureURL(t *testing.T) {
	c := New("https://registry.zpkg.tech")
	for _, target := range []string{"http://evil.example/artifact", "file:///etc/passwd"} {
		v := &VersionMetadata{Sha256: "abc", DownloadURL: target}
		err := c.DownloadArtifact(v, filepath.Join(t.TempDir(), "a"))
		var apiErr *APIError
		if !errors.As(err, &apiErr) || apiErr.Code != "insecure_download_url" {
			t.Fatalf("target %q: want insecure_download_url, got %v", target, err)
		}
	}
}

func TestDownloadArtifactAllowsLoopbackHTTP(t *testing.T) {
	body := []byte("ok")
	sum := sha256.Sum256(body)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(body)
	}))
	defer server.Close()
	// A registry configured over https must still accept a loopback http url.
	c := New("https://registry.zpkg.tech")
	v := &VersionMetadata{Sha256: hex.EncodeToString(sum[:]), Size: uint64(len(body)), DownloadURL: server.URL + "/artifact"}
	if err := c.DownloadArtifact(v, filepath.Join(t.TempDir(), "a")); err != nil {
		t.Fatalf("loopback http download: %v", err)
	}
}

func TestDownloadArtifactRejectsOversizeBody(t *testing.T) {
	// Declared size 1 -> limit = 1 + 1 MiB slack. Serve just over that.
	limit := downloadLimit(1)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(make([]byte, limit+64))
	}))
	defer server.Close()
	c := New(server.URL)
	v := &VersionMetadata{Sha256: "abc", Size: 1, DownloadURL: server.URL + "/artifact"}
	err := c.DownloadArtifact(v, filepath.Join(t.TempDir(), "a"))
	var apiErr *APIError
	if !errors.As(err, &apiErr) || apiErr.Code != "artifact_too_large" {
		t.Fatalf("want artifact_too_large, got %v", err)
	}
}

func TestDownloadLimit(t *testing.T) {
	if got := downloadLimit(0); got != maxArtifactBytes {
		t.Fatalf("downloadLimit(0) = %d", got)
	}
	if got := downloadLimit(10); got != 10+downloadSlack {
		t.Fatalf("downloadLimit(10) = %d", got)
	}
	if got := downloadLimit(maxArtifactBytes); got != maxArtifactBytes {
		t.Fatalf("downloadLimit(cap) = %d", got)
	}
}

func TestBaseTrimmed(t *testing.T) {
	c := New("https://registry.zpkg.tech///")
	if c.Base != "https://registry.zpkg.tech" {
		t.Fatalf("Base = %q", c.Base)
	}
}
