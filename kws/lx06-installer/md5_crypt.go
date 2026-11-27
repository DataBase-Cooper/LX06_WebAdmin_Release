package main

// MD5-crypt ("$1$") hash for generating the root /etc/shadow entry in the
// SSH firmware package.
//
// Algorithm reference: libxcrypt lib/crypt-md5.c (besser82/libxcrypt,
// default branch "develop"), identical to glibc crypt(3) and the original
// FreeBSD/Poul-Henning Kamp implementation. Verified byte-for-byte against
// glibc crypt(3) output on 19 vectors (see md5_vectors.go + go test).

import (
	"crypto/md5"
	crand "crypto/rand"
	"strings"
	"time"
)

// md5b64t is the crypt(3) base64 alphabet. CRITICAL: the canonical $1$
// table is "./" + digits + A-Z + a-z (libxcrypt lib/util-base64.c
// ascii64, same as OpenSSL cov_2char) — NOT the letter-first variant.
const md5b64t = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

// md5CryptSalt normalizes a salt the way crypt(3) does: strip an optional
// "$1$" prefix, stop at the next '$' (':' and '\n' are also invalid and
// treated as terminators by libxcrypt), cap at 8 chars.
func md5CryptSalt(salt string) string {
	if strings.HasPrefix(salt, "$1$") {
		salt = salt[3:]
	}
	i := strings.IndexAny(salt, "$:\n")
	if i >= 0 {
		salt = salt[:i]
	}
	if len(salt) > 8 {
		salt = salt[:8]
	}
	return salt
}

// md5CryptHash returns the full "$1$<salt>$<22chars>" hash of password with
// the given salt. The salt argument may or may not include the "$1$"
// prefix; the stored salt in the output is the normalized (<=8 char) form.
func md5CryptHash(password, salt string) string {
	salt = md5CryptSalt(salt)
	pw := []byte(password)

	// Phase 1: alternate sum = MD5(password + salt + password)
	h1 := md5.New()
	h1.Write(pw)
	h1.Write([]byte(salt))
	h1.Write(pw)
	result := h1.Sum(nil) // 16 bytes

	// Phase 2: base = MD5(password + "$1$" + salt), then add len(pw) bytes
	// of the alternate sum.
	h2 := md5.New()
	h2.Write(pw)
	h2.Write([]byte("$1$"))
	h2.Write([]byte(salt))
	// Add for every byte of the password one byte of the alternate sum,
	// wrapping at 16. libxcrypt writes (len%16)-style partial blocks plus a
	// final partial; together that is exactly len(pw) bytes of the 16-byte
	// sum (e.g. len=32: two full 16-byte blocks).
	for i := 0; i < len(pw); i++ {
		h2.Write(result[i%16 : (i%16)+1])
	}

	// NUL the first byte of the alternate sum (C: *result = '\0').
	alt := make([]byte, 16)
	copy(alt, result)
	alt[0] = 0

	// Parity loop: for each bit of the password length (LSB first), append
	// alt[0] (always 0x00) on a set bit, otherwise the first password byte.
	for cnt := len(pw); cnt > 0; cnt >>= 1 {
		if cnt&1 != 0 {
			h2.Write(alt[:1])
		} else {
			h2.Write(pw[:1])
		}
	}

	result = h2.Sum(nil)

	// Phase 3: 1000-iteration mixing loop.
	for cnt := 0; cnt < 1000; cnt++ {
		h := md5.New()
		if cnt&1 != 0 {
			h.Write(pw)
		} else {
			h.Write(result)
		}
		if cnt%3 != 0 {
			h.Write([]byte(salt))
		}
		if cnt%7 != 0 {
			h.Write(pw)
		}
		if cnt&1 != 0 {
			h.Write(result)
		} else {
			h.Write(pw)
		}
		result = h.Sum(nil)
	}

	// Output: "$1$" + salt + "$" + 22 b64 chars in the standard swizzle.
	var sb strings.Builder
	sb.WriteString("$1$")
	sb.WriteString(salt)
	sb.WriteByte('$')
	sb.Write(md5b64out(result[0], result[6], result[12], 4))
	sb.Write(md5b64out(result[1], result[7], result[13], 4))
	sb.Write(md5b64out(result[2], result[8], result[14], 4))
	sb.Write(md5b64out(result[3], result[9], result[15], 4))
	sb.Write(md5b64out(result[4], result[10], result[5], 4))
	sb.Write(md5b64out(0, 0, result[11], 2))
	return sb.String()
}

// md5b64out encodes 24 bits (B2<<16|B1<<8|B0) as n (4 or 2) base64 chars,
// most-significant group first, per the b64_from_24bit macro.
func md5b64out(b2, b1, b0 byte, n int) []byte {
	w := (uint32(b2) << 16) | (uint32(b1) << 8) | uint32(b0)
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		out[i] = md5b64t[w&0x3f]
		w >>= 6
	}
	return out
}

// verifyMd5Crypt reports whether password hashes to the given "$1$" hash.
// Argument order is (hash, password) to match all call sites.
func verifyMd5Crypt(hash, password string) bool {
	if !strings.HasPrefix(hash, "$1$") {
		return false
	}
	salt := hash[3:]
	if i := strings.IndexByte(salt, '$'); i >= 0 {
		salt = salt[:i]
	} else {
		return false
	}
	return md5CryptHash(password, salt) == hash
}

// randomSalt returns a random 8-character MD5-crypt salt from the crypt
// base64 alphabet (all 64 characters are legal salt bytes).
func randomSalt() string {
	b := make([]byte, 8)
	if _, err := crand.Read(b); err != nil {
		// Clock-based fallback: still unique in practice.
		ns := time.Now().UnixNano()
		for i := range b {
			b[i] = byte(ns >> (uint(i) * 8))
		}
	}
	out := make([]byte, 8)
	for i := range out {
		out[i] = md5b64t[int(b[i])&0x3f]
	}
	return string(out)
}
