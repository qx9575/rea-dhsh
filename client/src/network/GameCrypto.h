#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dhsh {

// Static protocol constants extracted read-only from the shipped client
// (pi-derived tables and a hard-coded login key). They are not account
// secrets, and nothing here depends on the original executable at runtime.
const std::array<std::uint8_t, 0x1000>& s2cKeyTable();
const std::array<std::uint32_t, 18>& blowfishPInit();
const std::array<std::uint8_t, 103>& loginMasterKey();

// s2c table cipher: per-frame self-contained; decrypts one payload in place
// (payload = frame bytes after the u16 length header).
class S2cTableCipher {
public:
    static constexpr std::size_t kKeySize = 0x1000;

    explicit S2cTableCipher(const std::array<std::uint8_t, kKeySize>& key);

    void decryptInPlace(std::uint8_t* payload, std::size_t length) const;

private:
    std::array<std::uint8_t, kKeySize> key_;
};

// c2s login cipher: Blowfish variant with a CTS-style overlapping tail for
// lengths that are not a multiple of 8. In-place; degenerate tails
// (remainder but fewer than 8 bytes total) throw std::invalid_argument.
class BlowfishCipher {
public:
    explicit BlowfishCipher(const std::uint8_t* key, std::size_t keyLength);

    void encryptInPlace(std::uint8_t* data, std::size_t length) const;
    void decryptInPlace(std::uint8_t* data, std::size_t length) const;

private:
    void encryptBlock(std::uint8_t* block) const;
    void decryptBlock(std::uint8_t* block) const;
    std::uint32_t f(std::uint32_t x) const;

    std::array<std::uint32_t, 18> p_{};
    std::array<std::uint32_t, 1024> s_{};
};

// c2s session cipher: standard RC4; the permutation evolves across crypt calls
// so keystream continuity spans frames (one session per connection).
class Rc4Session {
public:
    Rc4Session(const std::uint8_t* key, std::size_t keyLength);

    void cryptInPlace(std::uint8_t* data, std::size_t length);

private:
    std::array<std::uint8_t, 256> s_{};
    std::uint8_t i_ = 0;
    std::uint8_t j_ = 0;
};

std::array<std::uint8_t, 16> md5Digest(const std::uint8_t* data, std::size_t length);
std::array<char, 32> md5UpperHex(const std::uint8_t* data, std::size_t length);

// c2s trailer: first 4 characters of the uppercase MD5 hex digest over
// cipherBody[0..min(length, 256)].
std::array<char, 4> c2sTrailer(const std::uint8_t* cipherBody, std::size_t length);
bool c2sTrailerMatches(const std::uint8_t* cipherBody, std::size_t length,
                       const char* trailer);

// c2s RC4 key = 93-byte credential blob followed by the 32 ASCII characters
// of MD5(credentialBlob) in uppercase hex (125 bytes total).
constexpr std::size_t kCredentialBlobSize = 93;
constexpr std::size_t kRc4SessionKeySize = kCredentialBlobSize + 32;

std::array<std::uint8_t, kRc4SessionKeySize> deriveRc4Key(
    const std::array<std::uint8_t, kCredentialBlobSize>& credentialBlob);

// Sequence field (u32le at payload offset 0x0e): increments by 2 per frame;
// the high bits carry connection state flags, so comparisons use the low mask.
constexpr std::uint32_t kSequenceIncrement = 2;
constexpr std::uint32_t kSequenceCompareMask = 0x01ffffffU;

// Session-state flag ORed into the raw credential-derived seed before
// anything is written or advanced: session seed = raw | 0x04000000. All four
// evidenced logins match (2026-11-23 x2, 2026-11-25, plus the fourth sample
// raw 0x0a4079a3 -> session 0x0e4079a3); the first three raw seeds already
// carried the bit, which is how it stayed hidden. Bit 26 sits with the other
// connection state flags above kSequenceCompareMask, so masked comparisons
// are unaffected.
constexpr std::uint32_t kSequenceStateFlag = 0x04000000U;

std::uint32_t readSequenceField(const std::uint8_t* payload);
std::uint32_t nextSequence(std::uint32_t sequenceField);
bool sequenceMatches(std::uint32_t observed, std::uint32_t expected);

// Credential-derived sequence seed (FUN_007a2e10, static pseudocode in
// .rea/sequence_seed_current.json). The 93-byte credential blob is hashed
// BEFORE the MAC is written: the MAC slot [0x4b, 0x5c] is zeroed in an
// internal copy only (the caller's blob keeps the real MAC for the wire and
// the RC4 key). The copy is hashed as 23 little-endian u32 words for 197
// rounds: u starts at 0x69a5274b, p at 0; each round x = d[p] ^ u,
// u += rotl32(x, 19), d[p] = u, p = (p + 1) % 23. The seed is the unaligned
// u32le at byte +0x47 of the mutated buffer, forced odd (| 1).
std::uint32_t deriveSequenceSeed(
    const std::array<std::uint8_t, kCredentialBlobSize>& credentialBlob);

// 0x4004 login-frame value written at +0x0e: the session seed (raw seed |
// kSequenceStateFlag) with byte 2 forced to 0x08. The 0x08 override is
// evidenced by exactly four real logins (2026-11-23 x2, 2026-11-25 and the
// fourth sample: login == (sessionSeed & 0xff00ffff) | 0x00080000); the
// static FUN_007a2a00 flow does NOT show it, so treat it as an empirical
// rule rather than a decompiled one.
std::uint32_t loginSequenceValue(std::uint32_t sequenceSeed);

} // namespace dhsh
