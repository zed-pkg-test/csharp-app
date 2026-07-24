package zedclient

import (
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
	if gotAuth != "Bearer zpkg_t" {
		t.Fatalf("Authorization = %q", gotAuth)
	}
}

func TestBaseTrimmed(t *testing.T) {
	c := New("https://registry.zpkg.tech///")
	if c.Base != "https://registry.zpkg.tech" {
		t.Fatalf("Base = %q", c.Base)
	}
}
