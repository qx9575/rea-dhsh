#include "resource/PakArchive.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
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

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_header(Bytes& bytes, std::uint32_t count, std::uint32_t checksum = 0x12345678U) {
    const std::string magic = "PAKF3001";
    for (const char value : magic) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    append_u32(bytes, checksum);
    append_u32(bytes, count);
}

void append_record(
    Bytes& bytes,
    const std::string& name,
    std::uint32_t offset,
    std::uint32_t size,
    std::uint32_t kind) {
    check(name.size() <= 32, "test record name is too long");
    for (std::size_t index = 0; index < 32; ++index) {
        const char value = index < name.size() ? name[index] : '\0';
        bytes.push_back(static_cast<std::byte>(value));
    }
    append_u32(bytes, offset);
    append_u32(bytes, size);
    append_u32(bytes, kind);
    append_u32(bytes, 0);
}

void append_payload(Bytes& bytes, const std::string& payload) {
    for (const char value : payload) {
        bytes.push_back(static_cast<std::byte>(value));
    }
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test archive");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write test archive");
    }
}

class TempArchive {
public:
    TempArchive(const Bytes& bytes) {
        static unsigned sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / ("dhsh_test_pak_" + std::to_string(++sequence) + ".pak");
        write_file(path_, bytes);
    }

    TempArchive(const TempArchive&) = delete;
    TempArchive& operator=(const TempArchive&) = delete;

    ~TempArchive() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    void replace(const Bytes& bytes) const {
        write_file(path_, bytes);
    }

private:
    std::filesystem::path path_;
};

Bytes leaf_archive() {
    Bytes bytes;
    append_header(bytes, 1);
    append_record(bytes, "leaf.txt", 64, 3, 0);
    append_payload(bytes, "abc");
    return bytes;
}

Bytes nested_archive(unsigned nesting) {
    Bytes bytes;
    append_header(bytes, 1);
    append_record(bytes, "leaf.bin", 64, 0, 0);

    for (unsigned level = 0; level < nesting; ++level) {
        Bytes parent;
        append_header(parent, 1);
        append_record(parent, "nested", 64, static_cast<std::uint32_t>(bytes.size()), 1);
        parent.insert(parent.end(), bytes.begin(), bytes.end());
        bytes = std::move(parent);
    }
    return bytes;
}

void test_leaf_path_absolute_offset_and_read() {
    TempArchive file(leaf_archive());
    const dhsh::PakArchive archive(file.path());
    check(archive.entries().size() == 1, "expected one leaf");

    const auto& entry = archive.entries().front();
    check(entry.name == "leaf.txt", "incorrect leaf name");
    check(entry.path == "leaf.txt", "incorrect leaf path");
    check(entry.offset == 64, "leaf offset is not absolute");
    check(entry.size == 3, "incorrect leaf size");
    check(entry.kind == 0, "incorrect leaf kind");
    check(entry.depth == 0, "incorrect leaf depth");

    const auto payload = archive.read(entry);
    check(payload.size() == 3, "incorrect payload size");
    check(payload[0] == std::byte{'a'} && payload[1] == std::byte{'b'} && payload[2] == std::byte{'c'},
        "incorrect payload bytes");
}

void test_nested_offsets_are_relative_to_current_base() {
    Bytes bytes;
    append_header(bytes, 1);
    append_record(bytes, "nested", 64, 52, 1);
    append_header(bytes, 1);
    append_record(bytes, "leaf.bin", 48, 4, 0);
    append_payload(bytes, "data");

    TempArchive file(bytes);
    const dhsh::PakArchive archive(file.path());
    check(archive.entries().size() == 1, "expected nested leaf");
    const auto& entry = archive.entries().front();
    check(entry.path == "nested/leaf.bin", "incorrect nested path");
    check(entry.offset == 112, "nested payload offset is not absolute");
    check(entry.depth == 1, "incorrect nested depth");
    check(entry.size == 4 && entry.kind == 0, "nested leaf metadata changed");
}

void test_zero_size_alias_is_preserved() {
    Bytes bytes;
    append_header(bytes, 2);
    append_record(bytes, "alias", 112, 0, 0);
    append_record(bytes, "payload", 112, 3, 0);
    append_payload(bytes, "abc");

    TempArchive file(bytes);
    const dhsh::PakArchive archive(file.path());
    check(archive.entries().size() == 2, "zero-size alias was discarded");
    check(archive.entries()[0].size == 0, "zero-size alias changed size");
    check(archive.entries()[0].offset == 112, "zero-size alias changed offset");
    check(archive.read(archive.entries()[0]).empty(), "zero-size alias returned data");
    check(archive.read(archive.entries()[1]).size() == 3, "aliased payload is unreadable");
}

void test_bad_magic_throws() {
    Bytes bytes = leaf_archive();
    bytes[0] = std::byte{'X'};
    TempArchive file(bytes);
    expect_format_error([&] { dhsh::PakArchive archive(file.path()); }, "bad magic");
}

void test_excessive_entry_count_throws() {
    Bytes bytes;
    append_header(bytes, 200001);
    TempArchive file(bytes);
    expect_format_error([&] { dhsh::PakArchive archive(file.path()); }, "excessive entry count");
}

void test_truncated_record_throws() {
    Bytes bytes;
    append_header(bytes, 1);
    append_record(bytes, "leaf", 64, 0, 0);
    bytes.pop_back();
    TempArchive file(bytes);
    expect_format_error([&] { dhsh::PakArchive archive(file.path()); }, "truncated record");
}

void test_nesting_deeper_than_eight_throws() {
    TempArchive valid(nested_archive(8));
    const dhsh::PakArchive archive(valid.path());
    check(archive.entries().size() == 1 && archive.entries()[0].depth == 8,
        "eight nested levels should be valid");

    TempArchive invalid(nested_archive(9));
    expect_format_error([&] { dhsh::PakArchive archive(invalid.path()); }, "excessive nesting");
}

void test_unsafe_paths_throw() {
    for (const std::string& name : {"../escape", "safe/../../escape", "..\\escape"}) {
        Bytes bytes;
        append_header(bytes, 1);
        append_record(bytes, name, 64, 0, 0);
        TempArchive file(bytes);
        expect_format_error([&] { dhsh::PakArchive archive(file.path()); }, "unsafe path: " + name);
    }
}

void test_out_of_bounds_and_overflowing_ranges_throw() {
    for (const auto& range : {std::pair<std::uint32_t, std::uint32_t>{63, 2},
             {0xffffffffU, 0xffffffffU}}) {
        Bytes bytes;
        append_header(bytes, 1);
        append_record(bytes, "leaf", range.first, range.second, 0);
        TempArchive file(bytes);
        expect_format_error([&] { dhsh::PakArchive archive(file.path()); }, "invalid payload range");
    }
}

void test_read_rejects_foreign_and_out_of_bounds_entries() {
    TempArchive file(leaf_archive());
    const dhsh::PakArchive archive(file.path());
    const dhsh::PakEntry copy = archive.entries().front();
    expect_format_error([&] { static_cast<void>(archive.read(copy)); }, "copied entry");

    TempArchive other_file(leaf_archive());
    const dhsh::PakArchive other(other_file.path());
    expect_format_error([&] { static_cast<void>(archive.read(other.entries().front())); }, "foreign entry");

    Bytes truncated;
    append_header(truncated, 1);
    append_record(truncated, "leaf.txt", 64, 3, 0);
    file.replace(truncated);
    expect_format_error([&] { static_cast<void>(archive.read(archive.entries().front())); },
        "truncated payload");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        try {
            const dhsh::PakArchive archive(argv[1]);
            std::cout << "PAK entries: " << archive.entries().size() << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "PAK error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        return 2;
    }

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"leaf path, absolute offset, and read", test_leaf_path_absolute_offset_and_read},
        {"nested offsets use current base", test_nested_offsets_are_relative_to_current_base},
        {"zero-size alias is preserved", test_zero_size_alias_is_preserved},
        {"bad magic throws", test_bad_magic_throws},
        {"excessive entry count throws", test_excessive_entry_count_throws},
        {"truncated record throws", test_truncated_record_throws},
        {"nesting deeper than eight throws", test_nesting_deeper_than_eight_throws},
        {"unsafe paths throw", test_unsafe_paths_throw},
        {"invalid payload ranges throw", test_out_of_bounds_and_overflowing_ranges_throw},
        {"read rejects foreign and out-of-bounds entries", test_read_rejects_foreign_and_out_of_bounds_entries},
    };

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
