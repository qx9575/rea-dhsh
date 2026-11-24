#include "network/GraphKeyDecoder.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint8_t expand5(std::uint8_t v) {
    return static_cast<std::uint8_t>(v * 255 / 31);
}

std::uint8_t expand6(std::uint8_t v) {
    return static_cast<std::uint8_t>(v * 255 / 63);
}

// Reference RGB565 -> RGBA conversion, written independently of the decoder.
void append_rgba(std::vector<std::uint8_t>& out, std::uint16_t rgb565) {
    const std::uint8_t r = expand5(static_cast<std::uint8_t>((rgb565 >> 11) & 0x1f));
    const std::uint8_t g = expand6(static_cast<std::uint8_t>((rgb565 >> 5) & 0x3f));
    const std::uint8_t b = expand5(static_cast<std::uint8_t>(rgb565 & 0x1f));
    out.push_back(r);
    out.push_back(g);
    out.push_back(b);
    out.push_back(255);
}

// Builds a synthetic 0x2005 payload: 0x16-byte header (command 0x2005, zero
// padding, width/height) followed by the RGB565 pixel stream XORed with the
// two-byte key, phase 0 at payload +0x16. All image content here is synthetic.
std::vector<std::byte> build_payload(std::uint16_t width, std::uint16_t height,
                                     const std::vector<std::uint16_t>& rgb565_pixels,
                                     std::uint8_t key0, std::uint8_t key1,
                                     std::size_t extra_trailing = 0) {
    std::vector<std::byte> payload(0x16 + rgb565_pixels.size() * 2 + extra_trailing,
                                   std::byte{0});
    payload[0x00] = std::byte{0x05};
    payload[0x01] = std::byte{0x20};
    payload[0x12] = static_cast<std::byte>(width & 0xff);
    payload[0x13] = static_cast<std::byte>((width >> 8) & 0xff);
    payload[0x14] = static_cast<std::byte>(height & 0xff);
    payload[0x15] = static_cast<std::byte>((height >> 8) & 0xff);
    const std::uint8_t key[2] = {key0, key1};
    for (std::size_t i = 0; i < rgb565_pixels.size(); ++i) {
        const std::uint16_t v = rgb565_pixels[i];
        payload[0x16 + i * 2] = static_cast<std::byte>((v & 0xff) ^ key[0]);
        payload[0x16 + i * 2 + 1] = static_cast<std::byte>(((v >> 8) & 0xff) ^ key[1]);
    }
    return payload;
}

// Synthetic 96x32 golden image: black background with a handful of known
// colored pixels. No real captcha content.
std::vector<std::uint16_t> golden_image() {
    std::vector<std::uint16_t> pixels(96 * 32, 0x0000);
    auto set = [&pixels](std::size_t x, std::size_t y, std::uint16_t v) {
        pixels[y * 96 + x] = v;
    };
    set(0, 0, 0xf800);        // red
    set(95, 0, 0x07e0);       // green
    set(0, 31, 0x001f);       // blue
    set(95, 31, 0xffff);      // white
    set(48, 16, 0xf81f);      // magenta
    set(10, 5, 0x07ff);       // cyan
    set(20, 27, 0xffe0);      // yellow
    set(33, 8, 0x8410);       // mid gray
    set(80, 20, 0x2104);      // dark pixel
    return pixels;
}

std::vector<std::uint8_t> golden_rgba(const std::vector<std::uint16_t>& pixels) {
    std::vector<std::uint8_t> expected;
    expected.reserve(pixels.size() * 4);
    for (const std::uint16_t v : pixels) {
        append_rgba(expected, v);
    }
    return expected;
}

void expect_full_rgba(const dhsh::GraphKeyImage& image,
                      const std::vector<std::uint16_t>& expected_pixels) {
    const std::vector<std::uint8_t> expected = golden_rgba(expected_pixels);
    check(image.rgba.size() == expected.size(), "RGBA byte count mismatch");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (image.rgba[i] != expected[i]) {
            throw std::runtime_error("RGBA mismatch at byte offset " + std::to_string(i) +
                ": expected " + std::to_string(expected[i]) +
                ", got " + std::to_string(image.rgba[i]));
        }
    }
}

void test_golden_96x32_roundtrip() {
    const dhsh::GraphKeyDecoder decoder;
    const std::vector<std::uint16_t> pixels = golden_image();
    // Key bytes are synthetic (not the observed production values).
    const auto payload = build_payload(96, 32, pixels, 0xc3, 0x5a);
    const auto result = decoder.decode(payload);
    check(result.ok(), "golden 96x32 payload was rejected");
    check(result.error == dhsh::GraphKeyDecodeError::none, "golden error enum mismatch");
    check(result.image.width == 96, "golden width mismatch");
    check(result.image.height == 32, "golden height mismatch");
    expect_full_rgba(result.image, pixels);
}

void test_small_dimensions_roundtrip() {
    const dhsh::GraphKeyDecoder decoder;
    std::vector<std::uint16_t> pixels(4 * 3, 0x0000);
    pixels[0] = 0xf800;
    pixels[5] = 0x07e0;
    pixels[11] = 0x001f;
    const auto payload = build_payload(4, 3, pixels, 0x11, 0x22);
    const auto result = decoder.decode(payload);
    check(result.ok(), "small-dimension payload was rejected");
    check(result.image.width == 4 && result.image.height == 3, "small-dimension size mismatch");
    expect_full_rgba(result.image, pixels);
}

void test_rgb565_expansion_values() {
    // Independent known answers for the 5/6/5 expansion (r*255/31, g*255/63).
    const dhsh::GraphKeyDecoder decoder;
    std::vector<std::uint16_t> pixels(2 * 8, 0x0000);
    const std::uint16_t values[8] = {
        0xf800, 0x07e0, 0x001f, 0xffff, 0xf81f, 0x07ff, 0xffe0, 0x8410,
    };
    for (std::size_t i = 0; i < 8; ++i) {
        pixels[i] = values[i];
    }
    const auto payload = build_payload(2, 8, pixels, 0x00, 0x00);
    const auto result = decoder.decode(payload);
    check(result.ok(), "expansion vector was rejected");
    check(expand5(31) == 255 && expand6(63) == 255, "expansion identity values wrong");
    check(expand5(16) == 131 && expand6(32) == 129, "expansion mid values wrong");
    expect_full_rgba(result.image, pixels);
}

void test_short_payload_is_malformed() {
    const dhsh::GraphKeyDecoder decoder;
    for (const std::size_t size : {std::size_t{0}, std::size_t{0x10}, std::size_t{0x15}}) {
        const std::vector<std::byte> payload(size, std::byte{0});
        const auto result = decoder.decode(payload);
        check(!result.ok(), "short payload accepted (size " + std::to_string(size) + ")");
        check(result.error == dhsh::GraphKeyDecodeError::malformed_payload,
            "short payload error enum mismatch");
        check(result.image.rgba.empty(), "short payload produced pixels");
    }
}

void test_wrong_command_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    auto payload = build_payload(96, 32, golden_image(), 0xc3, 0x5a);
    payload[0x00] = std::byte{0x04};
    const auto result = decoder.decode(payload);
    check(!result.ok(), "wrong command accepted");
    check(result.error == dhsh::GraphKeyDecodeError::unexpected_command,
        "wrong command error enum mismatch");
}

void test_zero_dimensions_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    {
        // width = 0, height = 0, no pixel bytes: size equality would hold, so
        // the dimension check itself must reject.
        const auto payload = build_payload(0, 0, {}, 0x00, 0x00);
        const auto result = decoder.decode(payload);
        check(!result.ok(), "zero dimensions accepted");
        check(result.error == dhsh::GraphKeyDecodeError::invalid_dimensions,
            "zero dimensions error enum mismatch");
    }
    {
        const auto payload = build_payload(96, 0, {}, 0x00, 0x00);
        const auto result = decoder.decode(payload);
        check(!result.ok(), "zero height accepted");
        check(result.error == dhsh::GraphKeyDecodeError::invalid_dimensions,
            "zero height error enum mismatch");
    }
}

void test_oversized_dimensions_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    // Keep width*height*2 consistent so the rejection proves the dimension cap.
    const std::uint16_t width = 300;   // above the supported cap
    const std::uint16_t height = 8;
    const std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height, 0x0000);
    const auto payload = build_payload(width, height, pixels, 0x00, 0x00);
    const auto result = decoder.decode(payload);
    check(!result.ok(), "oversized dimensions accepted");
    check(result.error == dhsh::GraphKeyDecodeError::invalid_dimensions,
        "oversized dimensions error enum mismatch");
}

void test_truncated_pixels_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    const std::vector<std::uint16_t> pixels = golden_image();
    auto payload = build_payload(96, 32, pixels, 0xc3, 0x5a);
    payload.resize(payload.size() - 1);
    const auto result = decoder.decode(payload);
    check(!result.ok(), "truncated pixel stream accepted");
    check(result.error == dhsh::GraphKeyDecodeError::pixel_size_mismatch,
        "truncation error enum mismatch");
}

void test_trailing_bytes_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    const std::vector<std::uint16_t> pixels = golden_image();
    const auto payload = build_payload(96, 32, pixels, 0xc3, 0x5a, /*extra_trailing=*/2);
    const auto result = decoder.decode(payload);
    check(!result.ok(), "trailing bytes accepted");
    check(result.error == dhsh::GraphKeyDecodeError::pixel_size_mismatch,
        "trailing bytes error enum mismatch");
}

void test_odd_pixel_byte_count_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    // width*height*2 is always even; an odd pixel region can never match.
    auto payload = build_payload(1, 1, {0x0000}, 0x00, 0x00);
    payload.push_back(std::byte{0xab});
    const auto result = decoder.decode(payload);
    check(!result.ok(), "odd pixel byte count accepted");
    check(result.error == dhsh::GraphKeyDecodeError::pixel_size_mismatch,
        "odd pixel byte count error enum mismatch");
}

void test_dimension_size_mismatch_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    // Header claims 97x32 but the pixel stream holds 96x32 bytes.
    const std::vector<std::uint16_t> pixels = golden_image();
    const auto payload = build_payload(97, 32, pixels, 0xc3, 0x5a);
    const auto result = decoder.decode(payload);
    check(!result.ok(), "dimension/size mismatch accepted");
    check(result.error == dhsh::GraphKeyDecodeError::pixel_size_mismatch,
        "dimension/size mismatch error enum mismatch");
}

void test_weak_background_rejected() {
    const dhsh::GraphKeyDecoder decoder;
    // Four evenly spread ciphertext values: dominant share is 0.25.
    std::vector<std::uint16_t> pixels(96 * 32);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint16_t>(0x1000 * (i % 4));
    }
    const auto payload = build_payload(96, 32, pixels, 0x00, 0x00);
    const auto result = decoder.decode(payload);
    check(!result.ok(), "weak background accepted");
    check(result.error == dhsh::GraphKeyDecodeError::weak_background,
        "weak background error enum mismatch");
    check(result.image.rgba.empty(), "weak background produced pixels");
}

void test_half_background_boundary_accepted() {
    const dhsh::GraphKeyDecoder decoder;
    // Dominant share exactly 0.5 meets the >= threshold and must decode.
    std::vector<std::uint16_t> pixels(96 * 32);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (i % 2 == 0) ? 0x0000 : 0x7c00;
    }
    const auto payload = build_payload(96, 32, pixels, 0x3c, 0xa5);
    const auto result = decoder.decode(payload);
    check(result.ok(), "half-background boundary rejected");
    expect_full_rgba(result.image, pixels);
}

void test_error_messages_are_sanitized() {
    check(std::string(dhsh::graphKeyDecodeMessage(dhsh::GraphKeyDecodeError::none)).empty(),
        "none error should have an empty message");
    const dhsh::GraphKeyDecodeError errors[] = {
        dhsh::GraphKeyDecodeError::malformed_payload,
        dhsh::GraphKeyDecodeError::unexpected_command,
        dhsh::GraphKeyDecodeError::invalid_dimensions,
        dhsh::GraphKeyDecodeError::pixel_size_mismatch,
        dhsh::GraphKeyDecodeError::weak_background,
    };
    for (const auto error : errors) {
        const std::string message = dhsh::graphKeyDecodeMessage(error);
        check(!message.empty(), "error message is empty");
    }
}

} // namespace

int main() {
    try {
        test_golden_96x32_roundtrip();
        test_small_dimensions_roundtrip();
        test_rgb565_expansion_values();
        test_short_payload_is_malformed();
        test_wrong_command_rejected();
        test_zero_dimensions_rejected();
        test_oversized_dimensions_rejected();
        test_truncated_pixels_rejected();
        test_trailing_bytes_rejected();
        test_odd_pixel_byte_count_rejected();
        test_dimension_size_mismatch_rejected();
        test_weak_background_rejected();
        test_half_background_boundary_accepted();
        test_error_messages_are_sanitized();
        std::cout << "graphkey tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "graphkey test failure: " << error.what() << '\n';
        return 1;
    }
}
