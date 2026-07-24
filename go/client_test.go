package zedclient

import "testing"

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

func TestBaseTrimmed(t *testing.T) {
	c := New("https://registry.zpkg.tech///")
	if c.Base != "https://registry.zpkg.tech" {
		t.Fatalf("Base = %q", c.Base)
	}
}
