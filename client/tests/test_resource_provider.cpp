#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
void expect_resource_error(
    Callback&& callback,
    const std::string& message,
    const std::string& expected_fragment = {}) {
    try {
        std::forward<Callback>(callback)();
    } catch (const dhsh::ResourceError& error) {
        if (!expected_fragment.empty()
            && std::string(error.what()).find(expected_fragment) == std::string::npos) {
            throw std::runtime_error(
                message + ": error does not contain original path: " + error.what());
        }
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(message + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(message + ": no exception");
}

Bytes text(std::string_view value) {
    Bytes bytes;
    bytes.reserve(value.size());
    for (const char character : value) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

std::string as_text(const Bytes& bytes) {
    std::string value;
    value.reserve(bytes.size());
    for (const std::byte byte : bytes) {
        value.push_back(static_cast<char>(byte));
    }
    return value;
}

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_header(Bytes& bytes, std::uint32_t count) {
    const std::string magic = "PAKF3001";
    for (const char value : magic) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    append_u32(bytes, 0x12345678U);
    append_u32(bytes, count);
}

void append_record(
    Bytes& bytes,
    const std::string& name,
    std::uint32_t offset,
    std::uint32_t size) {
    check(name.size() <= 32, "test record name is too long");
    for (std::size_t index = 0; index < 32; ++index) {
        const char value = index < name.size() ? name[index] : '\0';
        bytes.push_back(static_cast<std::byte>(value));
    }
    append_u32(bytes, offset);
    append_u32(bytes, size);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
}

struct EntrySpec {
    std::string name;
    Bytes payload;
};

Bytes build_pak(const std::vector<EntrySpec>& specs) {
    Bytes bytes;
    append_header(bytes, static_cast<std::uint32_t>(specs.size()));
    std::uint32_t offset = 16U + 48U * static_cast<std::uint32_t>(specs.size());
    for (const auto& spec : specs) {
        check(spec.payload.size() <= 0xffffffffU, "test payload is too large");
        const auto size = static_cast<std::uint32_t>(spec.payload.size());
        append_record(bytes, spec.name, offset, size);
        offset += size;
    }
    for (const auto& spec : specs) {
        bytes.insert(bytes.end(), spec.payload.begin(), spec.payload.end());
    }
    return bytes;
}

class TempDir {
public:
    TempDir() {
        static unsigned sequence = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("dhsh_test_resource_provider_" + std::to_string(stamp) + "_"
                + std::to_string(++sequence));
        std::filesystem::create_directories(path_);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    void write(const std::string& name, const Bytes& bytes) const {
        std::ofstream output(path_ / name, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create test archive: " + name);
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("cannot write test archive: " + name);
        }
    }

private:
    std::filesystem::path path_;
};

void test_case_and_separator_normalization() {
    TempDir dir;
    dir.write(
        "ui.pak",
        build_pak({
            {"MAP/YingTianFu/YingTianFu.map", text("map-bytes")},
            {"Text/Readme.TXT", text("hello")},
        }));

    dhsh::PakResourceProvider provider(dir.path(), {"ui.pak"});

    check(provider.exists("map\\YingTianFu\\YingTianFu.map"), "backslash lookup is missing");
    check(provider.exists("MAP/YingTianFu/YingTianFu.map"), "mixed-case lookup is missing");
    check(provider.exists("/map/YingTianFu/YingTianFu.map"), "leading-slash lookup is missing");
    check(provider.exists("map//YingTianFu///YingTianFu.map"), "repeated-separator lookup is missing");

    check(
        as_text(provider.bytes("map\\YingTianFu\\YingTianFu.map")) == "map-bytes",
        "backslash lookup returned wrong bytes");
    check(
        as_text(provider.bytes("MAP/YingTianFu/YingTianFu.map")) == "map-bytes",
        "mixed-case lookup returned wrong bytes");
    check(as_text(provider.bytes("text\\readme.txt")) == "hello", "case-folded lookup failed");
    check(as_text(provider.bytes("TEXT/README.TXT")) == "hello", "uppercase lookup failed");
}

void test_returns_stored_bytes_without_decoding() {
    TempDir dir;
    const Bytes stored = {
        std::byte{'N'},
        std::byte{'Z'},
        std::byte{0x0b},
        std::byte{0x00},
        std::byte{0x03},
        std::byte{0x00},
        std::byte{0x02},
        std::byte{0x00},
        std::byte{0x00},
    };
    dir.write("magic.pak", build_pak({{"fx/raw.bin", stored}}));

    dhsh::PakResourceProvider provider(dir.path(), {"magic.pak"});
    check(provider.bytes("FX/RAW.BIN") == stored, "provider decoded or changed stored bytes");
}

void test_missing_paths_throw_with_original_path() {
    TempDir dir;
    dir.write("ui.pak", build_pak({{"Present.txt", text("here")}}));

    dhsh::PakResourceProvider provider(dir.path(), {"ui.pak"});
    const std::string requested = "MAP\\Missing\\File.map";

    expect_resource_error(
        [&] { static_cast<void>(provider.bytes(requested)); },
        "missing logical path",
        requested);
    check(!provider.exists(requested), "missing logical path reported as existing");
    check(!provider.exists(""), "empty logical path reported as existing");
    expect_resource_error(
        [&] { static_cast<void>(provider.bytes("")); },
        "empty logical path");
}

void test_duplicate_paths_keep_all_sources_and_first_mount_wins() {
    TempDir dir;
    dir.write("a.pak", build_pak({{"Shared/Data.bin", text("first")}}));
    dir.write("b.pak", build_pak({{"SHARED/DATA.BIN", text("second")}}));

    dhsh::PakResourceProvider first_wins(dir.path(), {"a.pak", "b.pak"});
    check(as_text(first_wins.bytes("shared\\data.bin")) == "first", "first mount did not win");

    const auto sources = first_wins.sources("Shared/Data.bin");
    check(sources.size() == 2, "duplicate sources were overwritten");
    check(sources[0].pak == "a.pak", "first duplicate source has wrong pak");
    check(sources[0].path == "Shared/Data.bin", "first duplicate source lost its path");
    check(sources[1].pak == "b.pak", "second duplicate source has wrong pak");
    check(sources[1].path == "SHARED/DATA.BIN", "second duplicate source lost its path");

    dhsh::PakResourceProvider reversed(dir.path(), {"b.pak", "a.pak"});
    check(as_text(reversed.bytes("shared/data.bin")) == "second", "mount order did not control priority");
}

void test_duplicate_paths_within_one_archive_keep_entry_order() {
    TempDir dir;
    dir.write(
        "dup.pak",
        build_pak({
            {"Same.bin", text("one")},
            {"SAME.BIN", text("two")},
        }));

    dhsh::PakResourceProvider provider(dir.path(), {"dup.pak"});
    check(as_text(provider.bytes("same.bin")) == "one", "first entry in archive did not win");
    const auto sources = provider.sources("SAME.bin");
    check(sources.size() == 2, "in-archive duplicates were overwritten");
    check(sources[0].path == "Same.bin", "in-archive duplicate order changed");
    check(sources[1].path == "SAME.BIN", "in-archive duplicate order changed");
}

void test_lookup_is_deterministic_across_instances() {
    TempDir dir;
    dir.write("a.pak", build_pak({{"Stable/Path.txt", text("stable")}}));
    dir.write("b.pak", build_pak({{"STABLE/PATH.TXT", text("shadowed")}}));

    dhsh::PakResourceProvider first(dir.path(), {"a.pak", "b.pak"});
    dhsh::PakResourceProvider second(dir.path(), {"a.pak", "b.pak"});

    check(first.bytes("stable/path.txt") == second.bytes("STABLE\\PATH.TXT"),
        "two providers returned different bytes");
    const auto first_sources = first.sources("stable/path.txt");
    const auto second_sources = second.sources("stable/path.txt");
    check(first_sources.size() == second_sources.size(), "two providers indexed different source counts");
    for (std::size_t index = 0; index < first_sources.size(); ++index) {
        check(first_sources[index].pak == second_sources[index].pak, "source pak order is not deterministic");
        check(first_sources[index].path == second_sources[index].path, "source path order is not deterministic");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"case and separator normalization", test_case_and_separator_normalization},
        {"stored bytes are returned undecoded", test_returns_stored_bytes_without_decoding},
        {"missing paths throw with original path", test_missing_paths_throw_with_original_path},
        {"duplicate sources keep mount order", test_duplicate_paths_keep_all_sources_and_first_mount_wins},
        {"in-archive duplicates keep entry order", test_duplicate_paths_within_one_archive_keep_entry_order},
        {"lookup is deterministic", test_lookup_is_deterministic_across_instances},
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
