#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void expect_format_error(Callback&& callback, const std::string& message) {
    try {
        std::forward<Callback>(callback)();
    } catch (const dhsh::FormatError&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(message + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(message + ": no exception");
}

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    result.reserve(values.size());
    for (const unsigned value : values) {
        check(value <= 0xffU, "test byte is out of range");
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

Bytes text(std::string_view value) {
    Bytes result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(static_cast<std::byte>(character));
    }
    return result;
}

void append_u16(Bytes& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>(value >> 8));
}

void append_u32(Bytes& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

Bytes make_nz(
    std::uint16_t kind,
    std::uint32_t size1,
    const Bytes& payload,
    std::optional<std::uint32_t> size2 = std::nullopt) {
    Bytes result = text("NZ");
    append_u16(result, kind);
    append_u32(result, size1);
    append_u32(result, size2.value_or(static_cast<std::uint32_t>(payload.size())));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes lzw(std::initializer_list<std::uint16_t> codes) {
    Bytes payload;
    for (const std::uint16_t code : codes) {
        append_u16(payload, code);
    }
    return payload;
}

Bytes decode(const Bytes& input) {
    return dhsh::NzDecoder::decode(input);
}

void expect_decode_error(const Bytes& input, const std::string& message) {
    expect_format_error([&] { static_cast<void>(decode(input)); }, message);
}

void test_stored_payload_is_exact() {
    const Bytes payload = text(std::string_view("stored\0bytes", 12));
    check(decode(make_nz(0, static_cast<std::uint32_t>(payload.size()), payload)) == payload,
        "type 0 changed the payload");
    check(decode(make_nz(0, 0, {})).empty(), "empty type 0 payload failed");
}

void test_header_and_payload_extent_are_validated() {
    expect_decode_error(text("NZ"), "short header");
    Bytes bad_magic = make_nz(0, 0, {});
    bad_magic[0] = std::byte{'X'};
    expect_decode_error(bad_magic, "bad magic");
    expect_decode_error(make_nz(0x0c, 0, {}), "unsupported kind");
    expect_decode_error(make_nz(0, 1, text("a"), 2), "truncated declared payload");
    expect_decode_error(make_nz(0, 1, text("ab"), 1), "trailing bytes");
}

void test_stored_sizes_must_match() {
    expect_decode_error(make_nz(0, 2, text("abc")), "bad type 0 size1");
    expect_decode_error(make_nz(0, 3, text("abc"), 2), "bad type 0 size2");
    expect_decode_error(make_nz(0, 3, text("abc"), 4), "oversized type 0 size2");
}

void test_lzw_literals_require_a_single_terminal_end_code() {
    check(decode(make_nz(0x0a, 3, lzw({'A', 'B', 'C', 0x100}))) == text("ABC"),
        "LZW literals decoded incorrectly");
    check(decode(make_nz(0x0a, 0, lzw({0x100}))).empty(), "empty LZW stream failed");
    check(decode(make_nz(0x0a, 0, {})).empty(), "empty LZW payload failed");
    expect_decode_error(make_nz(0x0a, 1, lzw({'A'})), "missing LZW end code");
    expect_decode_error(make_nz(0x0a, 1, lzw({'A', 0x100, 'B'})), "LZW trailing code");
    expect_decode_error(make_nz(0x0a, 0, lzw({0x100, 0})), "LZW trailing zero code");
    expect_decode_error(make_nz(0x0a, 1, bytes({0x41})), "odd LZW payload");
}

void test_lzw_dictionary_backreferences() {
    check(decode(make_nz(0x0a, 7, lzw({'A', 'B', 0x101, 0x103, 0x100})))
            == text("ABABABA"),
        "LZW dictionary or KwKwK expansion is wrong");
    check(decode(make_nz(0x0a, 3, lzw({'A', 0x101, 0x100}))) == text("AAA"),
        "LZW overlapping dictionary expansion is wrong");
    expect_decode_error(make_nz(0x0a, 1, lzw({0x101, 0x100})), "non-literal first LZW code");
    expect_decode_error(make_nz(0x0a, 2, lzw({'A', 0x102, 0x100})), "future LZW code");
}

void test_lzw_output_must_equal_size1() {
    expect_decode_error(make_nz(0x0a, 2, lzw({'A', 0x100})), "short LZW output");
    expect_decode_error(make_nz(0x0a, 2, lzw({'A', 'B', 'C', 0x100})), "LZW output overrun");
    expect_decode_error(make_nz(0x0a, 3, lzw({'A', 'B', 0x101, 0x100})),
        "LZW dictionary output overrun");
    expect_decode_error(make_nz(0x0a, 0xffffffffU, lzw({0x100})),
        "huge incorrect LZW size1");
}

void append_values(Bytes& output, std::initializer_list<unsigned> values) {
    const Bytes suffix = bytes(values);
    output.insert(output.end(), suffix.begin(), suffix.end());
}

Bytes lzo_literal_stream(std::string_view value) {
    check(value.size() >= 4 && value.size() <= 238, "invalid LZO test literal length");
    Bytes payload = bytes({17U + static_cast<unsigned>(value.size())});
    const Bytes literals = text(value);
    payload.insert(payload.end(), literals.begin(), literals.end());
    return payload;
}

void append_lzo_end(Bytes& payload) {
    append_values(payload, {0x11, 0x00, 0x00});
}

void test_lzo_literals_and_end_marker() {
    Bytes payload = lzo_literal_stream("abcd");
    append_lzo_end(payload);
    check(decode(make_nz(0x0b, 4, payload)) == text("abcd"), "LZO literals decoded incorrectly");
    check(decode(make_nz(0x0b, 0, bytes({0x11, 0x00, 0x00}))).empty(),
        "empty LZO stream failed");
    expect_decode_error(make_nz(0x0b, 4, lzo_literal_stream("abcd")), "missing LZO end marker");
}

void test_lzo_overlapping_copies() {
    Bytes repeat = lzo_literal_stream("aaaa");
    append_values(repeat, {0xa0, 0x00});
    append_lzo_end(repeat);
    check(decode(make_nz(0x0b, 10, repeat)) == text("aaaaaaaaaa"),
        "LZO offset-one overlapping copy is wrong");

    Bytes copy = lzo_literal_stream("abcd");
    append_values(copy, {0x4c, 0x00});
    append_lzo_end(copy);
    check(decode(make_nz(0x0b, 7, copy)) == text("abcdabc"),
        "LZO dictionary backreference is wrong");
}

void test_lzo_truncated_input_throws() {
    expect_decode_error(make_nz(0x0b, 0, {}), "empty LZO payload");
    expect_decode_error(make_nz(0x0b, 3, bytes({20, 'a', 'b'})), "truncated initial literals");
    expect_decode_error(make_nz(0x0b, 7, bytes({21, 'a', 'b', 'c', 'd', 0x40})),
        "truncated LZO match");
    expect_decode_error(make_nz(0x0b, 4, bytes({0, 0})), "truncated extended literal count");
    expect_decode_error(make_nz(0x0b, 4, bytes({0x20, 0, 0, 0})),
        "truncated extended match count");
}

void test_lzo_rejects_bad_references_trailing_data_and_wrong_sizes() {
    Bytes trailing = lzo_literal_stream("abcd");
    append_lzo_end(trailing);
    trailing.push_back(std::byte{0});
    expect_decode_error(make_nz(0x0b, 4, trailing), "trailing LZO data");

    Bytes bad_reference = lzo_literal_stream("abcd");
    append_values(bad_reference, {0x4c, 0xff});
    append_lzo_end(bad_reference);
    expect_decode_error(make_nz(0x0b, 7, bad_reference), "LZO reference before output");

    Bytes valid = lzo_literal_stream("abcd");
    append_lzo_end(valid);
    expect_decode_error(make_nz(0x0b, 3, valid), "LZO output overrun");
    expect_decode_error(make_nz(0x0b, 5, valid), "short LZO output");
    expect_decode_error(make_nz(0x0b, 0xffffffffU, bytes({0x11, 0x00, 0x00})),
        "huge incorrect LZO size1");
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open NZ fixture");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine NZ fixture size");
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!input) {
        throw std::runtime_error("cannot read NZ fixture");
    }
    return data;
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create decoded fixture");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write decoded fixture");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        try {
            write_file(argv[2], decode(read_file(argv[1])));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "NZ error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"stored payload is exact", test_stored_payload_is_exact},
        {"header and payload extent are validated", test_header_and_payload_extent_are_validated},
        {"stored sizes must match", test_stored_sizes_must_match},
        {"LZW literals and end code", test_lzw_literals_require_a_single_terminal_end_code},
        {"LZW dictionary backreferences", test_lzw_dictionary_backreferences},
        {"LZW output equals size1", test_lzw_output_must_equal_size1},
        {"LZO literals and end marker", test_lzo_literals_and_end_marker},
        {"LZO overlapping copies", test_lzo_overlapping_copies},
        {"LZO truncated input", test_lzo_truncated_input_throws},
        {"LZO malformed references, trailing data, and sizes", test_lzo_rejects_bad_references_trailing_data_and_wrong_sizes},
    };

    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
