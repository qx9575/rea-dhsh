#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

// Static search-table metadata only. Networked NPC models, positions, and
// directions must come from replicated objects, never from this CSV row.
struct NpcSpawn {
    std::int32_t id = 0;
    std::string name; // UTF-8 for diagnostics; the archive stores GBK
    std::int32_t cell_x = 0;
    std::int32_t cell_y = 0;
    std::int32_t category = 0;
};

// Parses the plain GBK npc.csv text and keeps rows whose map-file column
// equals map_file. Malformed rows are skipped; a header or comment is ignored.
std::vector<NpcSpawn> load_npc_roster(
    std::string_view csv_text, std::string_view map_file);

// Converts the legacy GBK byte string used by config CSVs to UTF-8. Invalid
// sequences are replaced rather than rejecting the whole table.
std::string gbk_to_utf8(std::string_view gbk);

} // namespace dhsh
