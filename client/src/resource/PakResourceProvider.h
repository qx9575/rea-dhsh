#pragma once

#include "resource/PakArchive.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dhsh {

// One archive entry that owns a logical path. Duplicate logical paths keep
// every source in mount order so nothing is silently overwritten.
struct ResourceSource {
    std::string pak;
    std::string path;
};

class PakResourceProvider final : public ResourceProvider {
public:
    // Opens the requested archives in the given order. The first mounted entry
    // for a logical path wins, matching tools/kimi_viewer/source.py.
    PakResourceProvider(std::filesystem::path shdata_dir, std::vector<std::string> pak_names);

    std::vector<std::byte> bytes(std::string_view logical_path) override;
    bool exists(std::string_view logical_path) const override;

    // All sources for a logical path, in mount/entry order. Empty when absent.
    std::vector<ResourceSource> sources(std::string_view logical_path) const;

    // Lowercases ASCII and folds backslashes to forward slashes; leading,
    // trailing, and repeated separators are removed.
    static std::string normalize_logical_path(std::string_view logical_path);

private:
    struct IndexedSource {
        std::size_t archive_index;
        std::size_t entry_index;
        std::string pak;
        std::string path;
    };

    std::vector<std::string> pak_names_;
    std::vector<std::unique_ptr<PakArchive>> archives_;
    std::unordered_map<std::string, std::vector<IndexedSource>> index_;
};

} // namespace dhsh
