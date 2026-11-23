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

std::uint32_t readSequenceField(const std::uint8_t* payload);
std::uint32_t nextSequence(std::uint32_t sequenceField);
bool sequenceMatches(std::uint32_t observed, std::uint32_t expected);

} // namespace dhsh
