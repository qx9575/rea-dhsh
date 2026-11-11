#include "resource/PakResourceProvider.h"

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace dhsh {
namespace {

char ascii_lower(char value) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

} // namespace

std::string PakResourceProvider::normalize_logical_path(std::string_view logical_path) {
    std::string normalized;
    std::size_t start = 0;
    while (start <= logical_path.size()) {
        const std::size_t end = logical_path.find_first_of("/\\", start);
        const std::size_t length =
            end == std::string_view::npos ? logical_path.size() - start : end - start;
        if (length > 0) {
            if (!normalized.empty()) {
                normalized.push_back('/');
            }
            for (std::size_t index = 0; index < length; ++index) {
                normalized.push_back(ascii_lower(logical_path[start + index]));
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return normalized;
}

PakResourceProvider::PakResourceProvider(
    std::filesystem::path shdata_dir,
    std::vector<std::string> pak_names,
    std::vector<PakMountDiagnostics>* diagnostics)
    : pak_names_(std::move(pak_names)) {
    const auto elapsed = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    };
    if (diagnostics != nullptr) {
        diagnostics->clear();
        diagnostics->resize(pak_names_.size());
    }
    archives_.reserve(pak_names_.size());

    for (std::size_t index = 0; index < pak_names_.size(); ++index) {
        const std::string& pak_name = pak_names_[index];
        if (diagnostics != nullptr) {
            diagnostics->at(index).pak = pak_name;
        }
        const auto open_start = std::chrono::steady_clock::now();
        try {
            archives_.push_back(std::make_unique<PakArchive>(shdata_dir / pak_name));
        } catch (const std::exception& error) {
            throw FormatError("cannot index PAK " + pak_name + ": " + error.what());
        }
        if (diagnostics != nullptr) {
            diagnostics->at(index).open_ms = elapsed(open_start);
            diagnostics->at(index).entries = archives_.back()->entries().size();
        }
    }

    // Mount order is the explicit PAK order; within one archive, entry order is
    // the index order. Every occurrence is retained so duplicates are visible
    // through sources() instead of being silently overwritten.
    for (std::size_t archive_index = 0; archive_index < archives_.size(); ++archive_index) {
        const auto index_start = std::chrono::steady_clock::now();
        const std::vector<PakEntry>& entries = archives_[archive_index]->entries();
        for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
            const PakEntry& entry = entries[entry_index];
            const std::string key = normalize_logical_path(entry.path);
            if (key.empty()) {
                continue;
            }
            index_[key].push_back(IndexedSource{
                archive_index,
                entry_index,
                pak_names_[archive_index],
                entry.path,
            });
        }
        if (diagnostics != nullptr) {
            diagnostics->at(archive_index).index_ms = elapsed(index_start);
        }
    }
}

std::vector<std::byte> PakResourceProvider::bytes(std::string_view logical_path) {
    const std::string key = normalize_logical_path(logical_path);
    if (key.empty()) {
        throw FormatError("empty resource path");
    }

    const auto found = index_.find(key);
    if (found == index_.end() || found->second.empty()) {
        throw FormatError("resource not found: " + std::string(logical_path));
    }

    const IndexedSource& source = found->second.front();
    const std::vector<PakEntry>& entries = archives_[source.archive_index]->entries();
    return archives_[source.archive_index]->read(entries[source.entry_index]);
}

bool PakResourceProvider::exists(std::string_view logical_path) const {
    const std::string key = normalize_logical_path(logical_path);
    if (key.empty()) {
        return false;
    }
    const auto found = index_.find(key);
    return found != index_.end() && !found->second.empty();
}

std::vector<ResourceSource> PakResourceProvider::sources(std::string_view logical_path) const {
    const std::string key = normalize_logical_path(logical_path);
    if (key.empty()) {
        return {};
    }

    const auto found = index_.find(key);
    if (found == index_.end()) {
        return {};
    }

    std::vector<ResourceSource> result;
    result.reserve(found->second.size());
    for (const IndexedSource& source : found->second) {
        result.push_back(ResourceSource{source.pak, source.path});
    }
    return result;
}

} // namespace dhsh
