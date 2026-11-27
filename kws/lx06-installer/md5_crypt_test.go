package main

import (
	"strings"
	"testing"
)

// TestMD5CryptVectors validates md5CryptHash + verifyMd5Crypt against
// glibc crypt(3) reference vectors (see md5_vectors.go).
func TestMD5CryptVectors(t *testing.T) {
	for _, v := range md5Vectors {
		parts := strings.SplitN(v, "|", 3)
		if len(parts) != 3 {
			t.Fatalf("malformed vector: %q", v)
		}
		salt, pwd, want := parts[0], parts[1], parts[2]
		got := md5CryptHash(pwd, salt)
		// glibc caps the stored salt at 8 chars even when the setting had
		// more (e.g. "$1$abcdefghij$" -> "$1$abcdefgh$").
		storedSalt := salt
		if len(storedSalt) > 8 {
			storedSalt = storedSalt[:8]
		}
		wantFull := "$1$" + storedSalt + "$" + want
		if got != wantFull {
			t.Errorf("salt=%q pwd=%q:\n  want %s\n  got  %s", salt, pwd, wantFull, got)
		}
		if !verifyMd5Crypt(wantFull, pwd) {
			t.Errorf("salt=%q pwd=%q: verify rejected the correct password", salt, pwd)
		}
		if verifyMd5Crypt(wantFull, pwd+"x") {
			t.Errorf("salt=%q pwd=%q: verify accepted a wrong password", salt, pwd)
		}
	}
	// passwd-style salt derivation roundtrip
	s := md5CryptSalt("hello123")
	if len(s) != 8 {
		t.Fatalf("md5CryptSalt length = %d, want 8", len(s))
	}
	h := md5CryptHash("hello123", s)
	if !verifyMd5Crypt(h, "hello123") {
		t.Fatalf("salt roundtrip verify failed: %s", h)
	}
	if verifyMd5Crypt(h, "hello124") {
		t.Fatal("wrong password accepted in salt roundtrip")
	}
	if verifyMd5Crypt("root:*:0:0", "x") {
		t.Fatal("non-md5 shadow entry accepted")
	}
	// prefix-form salt must behave identically
	if a, b := md5CryptHash("pw", "abc"), md5CryptHash("pw", "$1$abc"); a != b {
		t.Fatalf("prefix handling mismatch: %s vs %s", a, b)
	}
}
