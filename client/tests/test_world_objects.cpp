#include "network/NpcSyncDecoder.h"
#include "scene/WorldObjectStore.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    throw std::runtime_error("invalid golden hex");
}

Bytes bytes_from_hex(const std::string& hex) {
    check(hex.size() % 2 == 0, "odd golden hex size");
    Bytes bytes;
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<std::byte>(
            (hex_digit(hex[index]) << 4) | hex_digit(hex[index + 1])));
    }
    return bytes;
}

Bytes golden_plain() {
    // Sanitized s2c 0x301e: public NPC "黄文炳" from the pinned 1.50.29 client.
    // Source correlation: .rea/npc-object-correlation-refresh.golden.json.
    return bytes_from_hex(
        "1e30000000000000000000000000a2a00000bbc6cec4b1fe0000000000000000"
        "0000000000000000000000000000000000001c3d1400914ec10100008c000000"
        "0103000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00");
}

Bytes golden_payload() {
    const Bytes plain = golden_plain();
    check(plain.size() == 161, "incorrect golden plain size");
    return Bytes(plain.begin() + 2, plain.end());
}

dhsh::DecodedMessage golden_message() {
    return dhsh::DecodedMessage{
        0, 0x301e, dhsh::MessageDirection::server_to_client, golden_payload()};
}

void write_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

dhsh::WorldObject sample_object(std::uint32_t id = 42) {
    return dhsh::WorldObject{
        id, dhsh::WorldObjectKind::npc, 1004, 10040,
        "应天府衙役", {6920, 2088}, dhsh::Direction::left_down};
}

void test_store_keys_by_object_id_and_tracks_visual_revision() {
    dhsh::WorldObjectStore store;
    check(store.upsert(sample_object()), "initial NPC upsert was not visual");
    check(store.size() == 1, "initial NPC was not stored");
    check(store.find(42)->revision == 1, "initial revision is not one");

    dhsh::WorldObject renamed = sample_object();
    renamed.name = "衙役";
    check(store.upsert(renamed), "name change was not visual");
    check(store.size() == 1, "same object ID created a duplicate");
    check(store.find(42)->name == "衙役", "update did not replace name");
    check(store.find(42)->revision == 2, "visual change did not advance revision");

    check(!store.upsert(renamed), "identical upsert reported a visual change");
    check(store.find(42)->revision == 2, "identical upsert advanced revision");

    dhsh::WorldObject moved = renamed;
    moved.position = {6928, 2096};
    moved.direction = dhsh::Direction::right_up;
    moved.shape = 10051;
    check(store.upsert(moved), "shape, position, or direction change was not visual");
    check(store.find(42)->position == dhsh::EntityPosition({6928, 2096}), "position update missing");
    check(store.find(42)->direction == dhsh::Direction::right_up, "direction update missing");
    check(store.find(42)->shape == 10051, "shape update missing");
    check(store.find(42)->static_npc_id == 1004, "static NPC ID was not preserved");
    check(store.find(42)->revision == 3, "second visual change did not advance revision");
}

void test_store_rejects_incomplete_npc_and_removes_only_exact_id() {
    dhsh::WorldObjectStore store;
    dhsh::WorldObject incomplete = sample_object();
    incomplete.shape.reset();
    check(!store.upsert(incomplete), "NPC without shape was accepted");
    check(store.size() == 0, "NPC without shape spawned an object");

    dhsh::WorldObject other = sample_object(43);
    other.static_npc_id = 1005;
    check(store.upsert(sample_object()), "first named NPC rejected");
    check(store.upsert(other), "second named NPC rejected");
    check(store.size() == 2, "same-name NPCs were merged");
    check(store.remove(42), "exact object removal failed");
    check(!store.remove(42), "repeated removal reported success");
    check(store.find(42) == nullptr, "removed NPC remained visible");
    check(store.find(43) != nullptr, "removal affected another object");
    check(store.find(43)->name == "应天府衙役", "remaining same-name NPC changed");
}

void test_short_301e_decodes_every_evidenced_field() {
    const auto event = dhsh::NpcSyncDecoder{}.decode(golden_message());
    const auto* upsert = std::get_if<dhsh::ObjectUpsert>(&event);
    check(upsert != nullptr, "short NPC 0x301e did not produce ObjectUpsert");
    const dhsh::WorldObject& object = upsert->object;
    check(object.object_id == 1326364, "wrong object ID");
    check(object.kind == dhsh::WorldObjectKind::npc, "wrong object kind");
    check(!object.static_npc_id.has_value(), "unproven static NPC ID was invented");
    check(object.shape == 20113, "wrong NPC shape");
    check(object.name == std::string("\xe9\xbb\x84\xe6\x96\x87\xe7\x82\xb3", 9),
        "GBK name was not converted to UTF-8");
    check(object.position == dhsh::EntityPosition({7192, 2248}), "wrong pixel position");
    check(object.direction == dhsh::Direction::left_down, "wrong direction mapping");
    check(object.revision == 0, "decoder invented a store revision");
}

void test_all_eight_wire_directions_are_preserved() {
    for (std::uint8_t wire = 0; wire < 8; ++wire) {
        dhsh::DecodedMessage message = golden_message();
        message.payload[0x3e] = static_cast<std::byte>(wire);
        const auto event = dhsh::NpcSyncDecoder{}.decode(message);
        const auto* upsert = std::get_if<dhsh::ObjectUpsert>(&event);
        check(upsert != nullptr, "valid wire direction rejected");
        check(upsert->object.direction == static_cast<dhsh::Direction>(wire),
            "wire direction changed during decode");
    }
}

void test_unknown_unrelated_and_wrong_direction_messages_are_ignored() {
    const dhsh::NpcSyncDecoder decoder;
    dhsh::DecodedMessage message = golden_message();
    message.direction = dhsh::MessageDirection::client_to_server;
    check(std::holds_alternative<std::monostate>(decoder.decode(message)),
        "c2s copy produced an object event");

    for (const std::uint16_t command : {0x3030, 0x300a, 0x302f, 0x30a0, 0x300d, 0xffff}) {
        message = golden_message();
        message.command = command;
        check(std::holds_alternative<std::monostate>(decoder.decode(message)),
            "unknown or unproven command produced an object event");
    }
}

void test_exact_length_type_shape_and_direction_are_required() {
    const dhsh::NpcSyncDecoder decoder;
    for (const std::size_t size : {std::size_t{0}, std::size_t{158}, std::size_t{160}, std::size_t{258}}) {
        dhsh::DecodedMessage message = golden_message();
        message.payload.resize(size, std::byte{0});
        check(std::holds_alternative<std::monostate>(decoder.decode(message)),
            "non-evidenced payload length produced an object event");
    }

    dhsh::DecodedMessage message = golden_message();
    message.payload[0x3f] = std::byte{4};
    check(std::holds_alternative<std::monostate>(decoder.decode(message)),
        "unproven object type produced an NPC");

    message = golden_message();
    message.payload[0x34] = std::byte{0};
    message.payload[0x35] = std::byte{0};
    check(std::holds_alternative<std::monostate>(decoder.decode(message)),
        "missing shape produced a fallback NPC");

    message = golden_message();
    message.payload[0x3e] = std::byte{8};
    check(std::holds_alternative<std::monostate>(decoder.decode(message)),
        "invalid direction produced an NPC");

    message = golden_message();
    write_u32(message.payload, 0x36, std::numeric_limits<std::uint32_t>::max());
    check(std::holds_alternative<std::monostate>(decoder.decode(message)),
        "overflowing pixel coordinate produced an NPC");
}

void test_decoder_preserves_same_name_objects_as_distinct_ids() {
    const dhsh::NpcSyncDecoder decoder;
    dhsh::WorldObjectStore store;
    dhsh::DecodedMessage first = golden_message();
    dhsh::DecodedMessage second = golden_message();
    write_u32(second.payload, 0x30, 1326365);
    for (const dhsh::DecodedMessage* message : {&first, &second}) {
        const auto event = decoder.decode(*message);
        const auto* upsert = std::get_if<dhsh::ObjectUpsert>(&event);
        check(upsert != nullptr, "same-name golden variant rejected");
        check(store.upsert(upsert->object), "decoded NPC was not stored");
    }
    check(store.size() == 2, "decoder or store merged same-name objects");
    check(store.find(1326364)->name == store.find(1326365)->name,
        "test did not preserve equal display names");
}

} // namespace

int main() {
    try {
        test_store_keys_by_object_id_and_tracks_visual_revision();
        test_store_rejects_incomplete_npc_and_removes_only_exact_id();
        test_short_301e_decodes_every_evidenced_field();
        test_all_eight_wire_directions_are_preserved();
        test_unknown_unrelated_and_wrong_direction_messages_are_ignored();
        test_exact_length_type_shape_and_direction_are_required();
        test_decoder_preserves_same_name_objects_as_distinct_ids();
        std::cout << "world_objects tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "world_objects test failure: " << error.what() << '\n';
        return 1;
    }
}
