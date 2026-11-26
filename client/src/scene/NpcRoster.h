#pragma once

#include "scene/Entity.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dhsh {

class ResourceProvider;

// One row of config.pak:misc/npc.csv selected for a map. Coordinates are
// map cells; the client converts them with pixel = cell * 16 + 8, matching
// the live NPC object fields observed in Game.exe 1.50.29.
struct NpcSpawn {
    std::int32_t id = 0;
    std::string name; // UTF-8 for diagnostics; the archive stores GBK
    std::int32_t cell_x = 0;
    std::int32_t cell_y = 0;
    std::int32_t category = 0;
    std::uint32_t model_shape = 0;
    Direction direction = Direction::right_down;
};

// Parses the plain GBK npc.csv text and keeps rows whose map-file column
// equals map_file. Malformed rows are skipped; a header or comment is ignored.
std::vector<NpcSpawn> load_npc_roster(
    std::string_view csv_text, std::string_view map_file);

// Resolves the npc.pak model directory for one NPC. Live-object hints captured
// from Game.exe take precedence (name, then cell); otherwise the candidates
// 10000+npcid and 10000+(npcid%1000) are accepted when the archive contains
// npc/<shape>/10/3.apg. Falls back to the known generic 10009 model.
std::uint32_t resolve_npc_model(const NpcSpawn& spawn,
    ResourceProvider& npc_resources, bool* hint_matched = nullptr);

// Current direction observed for a live NPC hint; right_down when no hint
// matches (the direction affects which npc.pak APG is used for idle).
Direction npc_hint_direction(const NpcSpawn& spawn);

// True when the spawned NPC was present in the latest runtime object capture.
// Used to avoid placing every row of the search table when the server only
// spawned a nearby subset.
bool npc_has_live_hint(const NpcSpawn& spawn);

// Converts the legacy GBK byte string used by config CSVs to UTF-8. Invalid
// sequences are replaced rather than rejecting the whole table.
std::string gbk_to_utf8(std::string_view gbk);

} // namespace dhsh
