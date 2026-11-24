#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhsh {

// Failure categories for the 0x2005 graph-key image decode. Every message the
// decoder returns is a fixed, sanitized string: payloads and pixel bytes are
// never reflected into errors or logs.
enum class GraphKeyDecodeError {
    none,
    malformed_payload,     // fewer bytes than the 0x16-byte image header
    unexpected_command,    // command field is not 0x2005
    invalid_dimensions,    // zero width/height or above the supported cap
    pixel_size_mismatch,   // width*height*2 != payload size - 0x16
    weak_background,       // dominant aligned u16 share is below 1/2
};

struct GraphKeyImage {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    // Row-major RGBA, width*height*4 bytes, alpha always 255.
    std::vector<std::uint8_t> rgba;
};

struct GraphKeyDecodeResult {
    GraphKeyDecodeError error = GraphKeyDecodeError::none;
    GraphKeyImage image;

    bool ok() const { return error == GraphKeyDecodeError::none; }
};

// Fixed, sanitized message for an error value; "" for none. Never includes
// payload or pixel content.
const char* graphKeyDecodeMessage(GraphKeyDecodeError error);

// Decodes the 0x2005 graph-key challenge payload into an RGBA image.
//
// Layout (evidence: .rea/graphkey_decode.json): width u16le at +0x12, height
// u16le at +0x14, then width*height little-endian RGB565 pixels at +0x16. The
// pixel byte stream is obfuscated by repeating a two-byte key with phase 0 at
// payload +0x16; the key is recovered as the most frequent aligned ciphertext
// u16 (the black RGB565 background dominates real challenges). Decoding is
// strictly bounded and fails closed: malformed, mis-sized, or non-image
// payloads are rejected instead of being rendered. No dependency on the
// original executable, and no image content is recorded anywhere.
class GraphKeyDecoder {
public:
    GraphKeyDecodeResult decode(const std::vector<std::byte>& payload) const;
};

} // namespace dhsh
