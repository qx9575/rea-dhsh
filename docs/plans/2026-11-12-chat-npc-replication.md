# Chat, NPC and Right-side UI Replication Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a replay-first protocol foundation, then use it to restore dynamic NPCs, the original detached chat window, and the right-side buttons with their real SCP/Lua panels before enabling live networking.

**Architecture:** A single decoded-message path feeds a world-object store, chat model, and panel manager from either replay or live sessions. Original PAK/SCP/Lua resources remain authoritative; reverse engineering is limited to missing field layouts, rendering anchors, control behavior, and message bindings. Replay uses decoded, length-bounded binary records so the C++ client does not need a JSON dependency or credentials during offline tests.

**Tech Stack:** C++20, CMake, Win32/D3D11, existing `dhsh_*` libraries, Python 3 with the existing `tools/kimi_msg_*` and `tools/dhsh` parsers, CTest, Ghidra/REA read-only analysis.

---

## Working rules

- Read `docs/plans/2026-11-12-chat-npc-replication-design.md` and the updated handoff before starting. Preserve all current uncommitted work.
- `D:/software/dhsh` is read-only. Do not launch or terminate the reference game. Raw captures, logs, screenshots, and credentials stay out of Git; use `.rea/` or `client/build/`.
- Do not commit or push unless the user explicitly asks. Each task below ends with a verification checkpoint instead of a commit.
- Use `@superpowers:test-driven-development` for every behavior change and `@superpowers:systematic-debugging` for failures. Use `@reverse-engineer-anything` for native/protocol investigations.
- Prefer existing parsers. Do not add a JSON, networking, GUI, or cryptographic package. Reuse the repository's byte readers, Lua VM, SCP parser, and Python crypto reference.
- If an existing unrelated test is red, record it and do not rewrite that unrelated subsystem. Never replace a failing assertion with a weaker one.

## Standard build/test commands

Run from the repository root unless a task says otherwise:

```bash
cmake --build client/build --config Debug --target dhsh_client
ctest --test-dir client/build -C Debug --output-on-failure
cmake --build client/build --config Release --target dhsh_client
ctest --test-dir client/build -C Release --output-on-failure
```

For the real-resource suites, configure with the documented environment if it is not already present:

```bash
DHSH_SHDATA='D:/software/dhsh/shdata' DHSH_MAP_CACHE="$PWD/client/build/map-cache" \
  cmake -S client -B client/build -G 'Visual Studio 17 2022' -A Win32 \
  -DPython3_EXECUTABLE='D:/software/anaconda3/python.exe'
```

`m1_acceptance` and `m1_app_acceptance` must report `Passed`, not `Skipped`, in final verification.

### Task 1: Freeze the baseline and correct the evidence record

**Files:**
- Modify: `docs/plans/2026-11-12-chat-npc-replication-handoff.md`
- Modify: `docs/VERIFY-2026-11-12-npc-hud-runtime.md`
- Create only in ignored storage: `.rea/chat-npc-baseline-*`

**Step 1: Run the existing tests before editing code**

Run:

```bash
cmake --build client/build --config Debug --target dhsh_test_entities dhsh_test_scp_ui dhsh_client
ctest --test-dir client/build -C Debug -R '^(entities|scp_ui|m1_)$' --output-on-failure
```

Expected: existing tests pass. Save the command output under `.rea/`; if a failure predates this work, record its exact test and error rather than claiming a clean baseline.

**Step 2: Capture the two current visual baselines**

Run the new client, not the reference game, with the documented `--shdata`, `--cache`, and `--log` options. Capture the main window and detached chat window into `.rea/`. Confirm the current defects: additive NPC anchor behavior, absent chat messages/input, and incomplete right-side interactions.

**Step 3: Record the corrected anchor fact**

Keep the handoff correction that `client/src/formats/Apg.cpp:188-194` already decodes signed anchors, `client/src/animation/AnimationClip.cpp:29-34` propagates them, and `client/src/scene/Entity.cpp:133-142` currently adds them to position. Update any older statement in `docs/VERIFY-2026-11-12-npc-hud-runtime.md` that says anchors are unassigned.

**Step 4: Checkpoint**

Expected: baseline commands and current defects are reproducible; no production code changed.

### Task 2: Define and test the sanitized decoded-replay format

**Files:**
- Create: `tools/export_protocol_replay.py`
- Create: `tools/test_protocol_replay.py`
- Create: `client/src/network/DecodedMessage.h`
- Create: `client/src/network/ProtocolReplay.cpp`
- Create: `client/src/network/ProtocolReplay.h`
- Create: `client/tests/test_protocol_replay.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`

**Step 1: Specify the format in the exporter**

Use a binary format with no third-party serializer:

```text
header:  char magic[4] = "DHR1"
record:  u32le elapsed_ms
         u16le command
         u8    direction       # 0 = s2c, 1 = c2s
         u8    flags           # reserved, must be zero
         u32le payload_size
         byte payload[payload_size]  # plaintext, command excluded
```

Reject payloads larger than 8000 bytes, trailing partial records, nonzero reserved flags, and non-monotonic timestamps. The exporter must omit login credentials and reject `0x4004` records rather than sanitizing them incompletely.

**Step 2: Write failing Python tests**

In `tools/test_protocol_replay.py`, cover a two-record round trip, a rejected login frame, excessive payload, and truncated record. Core assertion:

```python
self.assertEqual(
    decode_replay(encode_replay([
        ReplayRecord(0, 0x303F, Direction.S2C, b"chat"),
        ReplayRecord(20, 0x301E, Direction.S2C, b"object"),
    ])),
    [ReplayRecord(0, 0x303F, Direction.S2C, b"chat"),
     ReplayRecord(20, 0x301E, Direction.S2C, b"object")],
)
```

Run `PYTHONIOENCODING=utf-8 python -m unittest tools.test_protocol_replay -v`; expect failure because the exporter does not exist.

**Step 3: Implement the minimal exporter**

Reuse `load_key()` and `decrypt_payload()` from `tools/kimi_msg_crypto.py`; do not duplicate table crypto. Add CLI arguments `--stream`, `--out`, `--commands`, and `--max-records`. Default to a small explicit command set and require `--commands` so an accidental full capture is not exported.

Run the Python tests; expect all to pass.

**Step 4: Write the failing C++ replay-reader tests**

Test the same two records, bad magic, oversized length, truncation, and non-monotonic time. Public types:

```cpp
enum class MessageDirection : std::uint8_t { server_to_client, client_to_server };
struct DecodedMessage {
    std::uint32_t elapsed_ms = 0;
    std::uint16_t command = 0;
    MessageDirection direction = MessageDirection::server_to_client;
    std::vector<std::byte> payload;
};
class ProtocolReplay {
public:
    static ProtocolReplay load(const std::filesystem::path& path);
    const std::vector<DecodedMessage>& messages() const noexcept;
};
```

Build `dhsh_test_protocol_replay`; expect compilation failure until the library and test registration are added.

**Step 5: Implement bounded C++ loading and run tests**

Use the existing little-endian byte-reading style. Add `dhsh_protocol` to `client/src/CMakeLists.txt` and link the test. Expected: `ctest -R '^protocol_replay$'` passes and malformed input throws a diagnostic containing the record index.

**Step 6: Checkpoint**

Run both Python and C++ replay tests. Do not add a real capture to Git. Generate real replay files only under `.rea/` after Tasks 3 and 7 identify the required packets.

### Task 3: Establish NPC packet semantics and a keyed world-object store

**Files:**
- Create: `client/src/network/NpcSyncDecoder.cpp`
- Create: `client/src/network/NpcSyncDecoder.h`
- Create: `client/src/scene/WorldObjectStore.cpp`
- Create: `client/src/scene/WorldObjectStore.h`
- Create: `client/tests/test_world_objects.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`
- Modify: `docs/formats/net-protocol.md`
- Create only in ignored storage: `.rea/npc-object-correlation.*`

**Step 1: Correlate read-only evidence before choosing offsets**

With the user's already-running reference client, use `tools/capture_npc_objects.py` and the existing passive message tools. Ask the user to perform only small movements/open-close actions needed for correlation. Match object IDs and changed fields against the six known objects. Do not attach a debugger, write process memory, or terminate the process.

Update `docs/formats/net-protocol.md` with separate observed facts and unresolved candidates. Do not label `0x301e/0x301f/0x30a0/0x300d` as NPC creation/update until the bytes prove their roles.

**Step 2: Write failing object-store tests**

Use object ID, not name, as identity:

```cpp
store.upsert(WorldObject{42, WorldObjectKind::npc, 1004, 10040,
                         "衙役", {6920, 2088}, Direction::left_down});
store.upsert(WorldObject{42, WorldObjectKind::npc, 1004, 10040,
                         "应天府衙役", {6920, 2088}, Direction::left_down});
check(store.size() == 1, "same object ID created a duplicate");
check(store.find(42)->name == "应天府衙役", "update did not replace name");
store.remove(42);
check(store.find(42) == nullptr, "removed NPC remained visible");
check(store.find(43) == nullptr, "removal affected another object");
```

Also test that an incomplete object without shape cannot spawn an NPC.

**Step 3: Implement `WorldObjectStore` minimally**

Store `object_id`, kind, optional static NPCID, shape, UTF-8 display name, pixel position, direction, and revision. Upsert returns whether visual state changed; remove only affects the exact ID.

**Step 4: Write failing packet-decoder golden tests**

Use sanitized packets captured in Step 1. Assertions must cover every consumed field, command direction, exact payload length, insufficient-length rejection, and unknown-command rejection. Include two different objects with the same display name to prevent name-keyed regression.

**Step 5: Implement only evidenced commands**

Return `ObjectUpsert`, `ObjectRemove`, or `std::monostate` for a valid unrelated message. Unknown or partial state must not produce a fallback shape.

**Step 6: Checkpoint**

Run `ctest -R '^(protocol_replay|world_objects)$'`. Expected: all pass; docs name the exact commands/offsets now proven and those still unknown.

### Task 4: Reverse and test the NPC draw anchor, then remove the test NPC path

**Files:**
- Modify: `client/src/scene/Entity.cpp`
- Modify only if evidence requires: `client/src/scene/MapScene.cpp`
- Modify: `client/src/scene/LocalEntityFactory.cpp`
- Modify: `client/src/scene/LocalEntityFactory.h`
- Modify: `client/tests/test_entities.cpp`
- Modify: `client/tests/test_animation.cpp` only if decoder behavior actually changes
- Modify: `docs/VERIFY-2026-11-12-npc-hud-runtime.md`

**Step 1: Reverse the NPC draw method**

Starting at NPC vtable `0x984554` on the pinned matching executable, identify the draw virtual and its use of object `+0x2a/+0x2e` and frame `x/y`. Save pseudocode/assembly evidence in `.rea/`. Determine whether the formula is `position + anchor`, `position - anchor`, or includes another constant. Do not infer the sign from the screenshot alone.

**Step 2: Write the failing anchor test from the reversed formula**

Change `test_frame_origin_combines_logical_position_and_current_anchor` to assert the evidenced formula, and add a second NPC with a different anchor to prove this is not a universal screenshot constant. If the formula is subtraction, the essential assertion is:

```cpp
check(npc.frame_origin() == EntityPosition{303, 313},
      "NPC feet position must subtract the frame anchor");
```

Run `dhsh_test_entities`; expect only the new anchor assertion to fail.

**Step 3: Apply the minimal anchor fix**

Change the single shared origin calculation. Do not touch `ApgDecoder` if its signed x/y bytes are already correct, and do not add per-NPC offsets.

**Step 4: Remove production test-NPC behavior through tests**

Replace the real-resource test that explicitly requires `10009` at `(7000,2152)` with a test that `make_network_npc` uses the supplied shape and never substitutes `10009`. Remove `make_local_npc`, both `add_local_npc_if_available` overloads, and `npc_resource_path` from production. Keep no compatibility wrapper that can accidentally recreate the test NPC.

**Step 5: Run tests and compare real rendering**

Run `ctest -R '^(animation|entities|entities_real_resources)$'`. Then run the real client and compare the same six NPCs against `.rea/live_game_now.png`; verify the correction is uniform and no `10009` test model appears.

**Step 6: Checkpoint**

Expected: anchor tests and real-resource tests pass; documentation records the reversed formula and screenshot result.

### Task 5: Drive NPCs from replay instead of roster hints

**Files:**
- Modify: `client/src/scene/NpcRoster.cpp`
- Modify: `client/src/scene/NpcRoster.h`
- Modify: `client/src/scene/LocalEntityFactory.cpp`
- Modify: `client/src/scene/LocalEntityFactory.h`
- Modify: `client/src/scene/MapScene.cpp`
- Modify: `client/src/scene/MapScene.h`
- Modify: `client/src/app/App.cpp`
- Modify: `client/tests/test_entities.cpp`
- Modify: `client/tests/test_m1_acceptance.cpp`
- Modify: `client/tests/CMakeLists.txt` if a replay acceptance target is added

**Step 1: Write failing scene lifecycle tests**

Feed the replay decoder's upsert/remove events into the scene. Assert create, same-ID update, direction change, removal, and no shape fallback. Verify lazy APG loading still respects the viewport budget and failed resources leave other NPCs intact.

**Step 2: Implement direct-shape entity creation**

Create `make_network_npc(const WorldObject&, ResourceProvider&)` using `<shape>/10/<direction>.apg`. Keep direction fallback only when the original resource bundle lacks the requested direction and log the actual chosen path; never change shape.

**Step 3: Replace hint-driven production spawning**

Remove `kNpcModelHints`, `find_hint`, `npc_has_live_hint`, `npc_hint_direction`, candidate shape arithmetic, and `10009` fallback from the production path. Retain `load_npc_roster` only for static NPCID/map metadata and map/search features.

Add keyed scene methods such as `upsert_network_npc` and `remove_network_npc`; preserve existing local character behavior.

**Step 4: Add `--replay <DHR1>` and deterministic replay stepping**

Parse the option in `client/src/app/App.cpp:169-180` and its parser/usage text. In replay mode, do not call roster spawning. Pump messages by `elapsed_ms`, route NPC events to the object store/scene, and stop cleanly at end-of-stream. Add a bounded `--replay-speed` only if needed for tests; default to recorded timing.

**Step 5: Verify end to end**

Run:

```bash
client/build/Debug/dhsh_client.exe --shdata 'D:/software/dhsh/shdata' \
  --cache client/build/map-cache --replay .rea/npc-six.dhr1 \
  --log client/build/npc-replay.log
```

Expected: six networked NPCs appear/update/remove exactly as recorded, with no static 132-row spawn and no test NPC. Existing non-replay M1 behavior remains usable, with a clear diagnostic that live NPCs require replay/online input rather than fabricated models.

**Step 6: Checkpoint**

Run entity, replay, and both M1 acceptance suites. Expected: pass; logs contain actual object IDs/shapes and no credential fields.

### Task 6: Build the shared chat model and `0x303f` decoder

**Files:**
- Create: `client/src/chat/ChatModel.cpp`
- Create: `client/src/chat/ChatModel.h`
- Create: `client/src/network/ChatMessageDecoder.cpp`
- Create: `client/src/network/ChatMessageDecoder.h`
- Create: `client/tests/test_chat.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`
- Modify: `docs/formats/net-protocol.md`

**Step 1: Prove chat field boundaries**

Correlate several `0x303f` packets with the reference chat screenshot. Record channel, sender, rich-text markers, item links, line breaks, and text encoding. Use world/current/system samples and at least one item link. Do not treat a fixed 40-byte hex preview as a complete parser specification.

**Step 2: Write failing decoder and model tests**

Cover channel filtering, GBK text, long-line wrapping input, auto-scroll paused/unpaused, custom channels, and an item link. Core model API:

```cpp
struct ChatMessage {
    std::uint32_t channel = 0;
    std::string sender;
    std::vector<ChatSpan> spans;
};
class ChatModel {
public:
    void append(ChatMessage message);
    void set_channel_filter(std::optional<std::uint32_t> channel);
    void set_auto_scroll(bool enabled);
    void scroll(std::int32_t delta_lines);
    const std::vector<ChatMessage>& visible_messages() const;
};
```

Verify that changing filter does not delete history and paused scrolling does not jump on append.

**Step 3: Implement bounded decoding and model behavior**

Cap message count and text size using the original limits once confirmed; until then use defensive limits documented as client safeguards, not original semantics. Unknown rich-text tags remain visible as plain diagnostic-safe text rather than being executed.

**Step 4: Connect replay messages to the model**

`0x303f` appends to the shared model; unrelated commands do nothing. Replay mode must display recorded messages without any network connection or fake local echo.

**Step 5: Checkpoint**

Run `ctest -R '^(chat|protocol_replay)$'`. Expected: pass with real sanitized golden packets and documented offsets.

### Task 7: Implement common SCP list, edit, combo, and scroll behavior

**Files:**
- Create: `client/src/ui/ListViewEx.cpp`
- Create: `client/src/ui/ListViewEx.h`
- Create: `client/src/ui/RichEdit.cpp`
- Create: `client/src/ui/RichEdit.h`
- Create: `client/src/ui/ComboBox.cpp`
- Create: `client/src/ui/ComboBox.h`
- Modify: `client/src/ui/Widget.cpp`
- Modify: `client/src/ui/Widget.h`
- Modify: `client/src/ui/ScpWindow.cpp`
- Modify: `client/src/ui/ScpWindow.h`
- Modify: `client/src/platform/Win32Window.cpp`
- Modify: `client/src/platform/Win32Window.h`
- Modify: `client/tests/test_scp_ui.cpp`
- Modify: `client/src/CMakeLists.txt`

**Step 1: Write failing control tests from real SCP declarations**

Use `chatmain_txtlst_msg`, `chatmain_input_mes`, and `chatmain_txtlstchanel` properties from the exported real `chatmain.json`. Test `RECT` versus `CLIENTRECT`, `DRAWTXTTYPE=2`, hidden parents, scroll ranges, and message names. Unknown controls must still produce diagnostics rather than silently rendering.

**Step 2: Implement list layout and rich-text drawing**

`ListViewEx` owns visual lines, wrapping, channel colors, item-link spans, scroll position, and clipping. It reads `ChatModel` but does not own network history. Reuse existing font/GDI conversion and software-renderer clipping; do not scale the control tree.

**Step 3: Add text input without breaking map keyboard input**

Extend `WindowEvent` with UTF-8 text input and composition boundaries as needed. Map `WM_CHAR` and IME messages explicitly; only a focused edit consumes them. Write unit tests for focus, insertion, deletion, cursor movement, max length, and composition commit/cancel. Do not treat every `WM_KEYDOWN` as text.

**Step 4: Implement `RichEdit` and `ComboBox` state**

Support editing, selection/channel choices, show/hide, enable/disable, and `FSM_KEY_DOWN`/`FSM_COMBOBOX_SELECTED` dispatch. Sending is a separate command boundary; editing must not fabricate a successful send.

**Step 5: Implement common scrolling and mouse routing**

Test wheel, page buttons, drag capture, right-click declarations, and input isolation. The detached window must forward coordinates without the current global source/destination scaling.

**Step 6: Checkpoint**

Run `ctest -R '^(scp_ui|lua_host|chat)$'`. Expected: pass; real `chatmain.scp` no longer logs the four target control families as unsupported.

### Task 8: Rebuild `FSChatMain` with original geometry and skin behavior

**Files:**
- Modify: `client/src/platform/ScpChatWindow.cpp`
- Modify: `client/src/platform/ScpChatWindow.h`
- Modify: `client/src/app/App.cpp`
- Modify: `client/tests/test_scp_ui.cpp`
- Create if window automation belongs there: `tools/client_chat_acceptance.py`
- Modify: `docs/VERIFY-2026-11-12-npc-hud-runtime.md`

**Step 1: Reverse construction and layout before rewriting**

At the pinned matching executable, inspect `FSChatMain` construction around `0x4017d0`, RTTI/vtable, and accesses to `chatmain_*` controls. Confirm the actual SCP mount, client-size calculation, `skin0/skin2` selection, tile origin, bar visibility, and button behavior. Ask the user to open/close chat only for the required dynamic observations.

**Step 2: Write failing geometry tests**

Assert the confirmed client size 549×590, source rectangles, no global coordinate scaling, tile alignment, top/bottom row positions, and input-bar placement. Include a test that a source `x=3` control remains at logical `x=3` rather than being rescaled.

**Step 3: Replace approximate rendering**

Remove `source_width_`, `source_height_`, `h + 20`, and per-pixel source scaling. Render the confirmed SCP controls at native coordinates over the original skin. Connect `ChatModel`, `ListViewEx`, `RichEdit`, channel filters, scroll buttons, stop/resume state, and open/close behavior.

**Step 4: Wire switch behavior and sending boundary**

The main `chat_switch` opens the detached window and the detached `chatmain_switch` returns to embedded mode; repeated toggles must not leak Win32 windows. Replay displays history but does not claim to send. Leave a single `ChatSendSink` callback for the live `0x4050` implementation.

**Step 5: Run same-size visual and interaction acceptance**

Capture original and replica at identical client sizes. Compare skin, rows, message wrapping, input bar, and buttons; dynamic text regions may be compared structurally rather than by a fragile whole-screen hash. Exercise open -> filter -> scroll -> pause -> edit -> close -> reopen.

**Step 6: Checkpoint**

Run SCP/chat tests, real App acceptance, and the window smoke test. Expected: pass and no unsupported target chat controls remain.

### Task 9: Create the right-button evidence map and generic panel manager

**Files:**
- Create: `client/src/ui/PanelManager.cpp`
- Create: `client/src/ui/PanelManager.h`
- Create: `manifests/relations/hud-button-panels.csv`
- Create: `client/tests/test_panel_manager.cpp`
- Modify: `client/src/app/App.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`

**Step 1: Build the evidence map before coding callbacks**

For every visible right-side button in `pic/panel1.png` and `.rea/live_game_now.png`, record control name, message code, original Lua/C++ handler, target SCP/panel, display condition, and evidence address/path. Start with the eight `fucnbtn_shortcut` entries and include task tracking separately. Ask the user to click a button only when static Lua/SCP does not identify the result.

CSV columns:

```text
control,message_code,handler,target_kind,target_path,evidence,status
```

`status` is `confirmed` or `unresolved`; no guessed target may be marked confirmed.

**Step 2: Write failing generic panel tests**

Use a fake panel loader. Test open, idempotent reopen, close, exclusive/non-exclusive policy, unknown-message diagnostics, empty-data state, and destruction during callback dispatch. Do not require business data in these tests.

**Step 3: Implement `PanelManager`**

Route original `UiMessage` values through registered handlers. Panel loaders return real `ScpWindow` instances or a code-owned custom panel. Unknown buttons produce a visible/logged unsupported state; they never pretend an action succeeded.

**Step 4: Fix shortcut visibility and layout from original scripts**

Execute the relevant original Lua display/position logic through `FsLuaHost`, adding only evidenced host APIs. Do not leave all eight buttons at `RECT 0,0,32,24`, and do not hardcode screenshot pixels when the script computes them.

**Step 5: Checkpoint**

Run `ctest -R '^(panel_manager|scp_ui|lua_host)$'`. Expected: pass; every visible right button has a confirmed mapping or an explicit unresolved entry.

### Task 10: Connect each confirmed right button to its real panel

**Files:**
- Modify: `client/src/app/App.cpp`
- Modify: `client/src/ui/PanelManager.cpp`
- Modify only for missing evidenced host APIs: `client/src/script/FsLuaHost.cpp`
- Modify: `client/tests/test_panel_manager.cpp`
- Modify: `client/tests/test_scp_ui.cpp`
- Modify: `manifests/relations/hud-button-panels.csv`
- Modify: `client/README.md`

**Step 1: Implement one vertical panel first**

Choose a panel whose static mapping and SCP resources are complete. Add failing tests for its button message, real SCP load, close, and empty-data rendering; then implement and run its real-resource acceptance. This proves the registry path before expanding.

**Step 2: Repeat for all eight shortcut entries**

For each row in the evidence CSV, follow the same small cycle: failing message/panel test, original resource load, generic interactions, real empty-data acceptance. Add protocol requests only where a panel already has a sanitized replay response; otherwise retain a real empty state and mark the protocol as a later live task.

**Step 3: Restore task tracking and other visible code panels**

Implement their original SCP frame/custom drawing and scrolling. Do not invent quest names or completion states. Replay task data only after its message layout has been proven.

**Step 4: Add unsupported-control diagnostics per panel**

A panel may open with an explicit empty data area, but unsupported required controls must be listed by file/control/type. Do not hide a broken panel by returning a blank window without diagnostics.

**Step 5: Run manual full-button acceptance**

Launch the actual client and click every visible right-side button. Verify open, close, switching, scrolling, repeat clicks, and coexistence with detached chat. Record results in the CSV and `client/README.md`; do not claim unimplemented business operations work.

**Step 6: Checkpoint**

Run all UI, Lua, panel, and M1 tests. Expected: pass; every visible button has an observed result and no placeholder business data.

### Task 11: Port crypto/transport only after replay acceptance is green

**Files:**
- Create: `client/src/network/GameCrypto.cpp`
- Create: `client/src/network/GameCrypto.h`
- Create: `client/src/network/GameSession.cpp`
- Create: `client/src/network/GameSession.h`
- Create: `client/src/network/Win32TcpStream.cpp`
- Create: `client/src/network/Win32TcpStream.h`
- Create: `client/tests/test_game_crypto.cpp`
- Create: `client/tests/test_game_session.cpp`
- Modify: `client/src/CMakeLists.txt`
- Modify: `client/tests/CMakeLists.txt`
- Modify: `client/src/app/App.cpp`
- Modify: `client/README.md`
- Modify: `docs/formats/net-protocol.md`

**Step 1: Port and verify the cryptographic primitives independently**

Use `tools/kimi_msg_crypto.py` as the reference for s2c table crypto, Blowfish login, RC4 session state, sequence handling, and MD5 trailers. Golden tests must come from existing sanitized vectors and compare the complete plaintext/ciphertext, including multi-frame RC4 continuity. Never log key material.

Run `ctest -R '^game_crypto$'`; expect exact byte equality before any socket code.

**Step 2: Test framed transport without external network**

Use a loopback fixture server owned by the test. Cover partial reads, multiple frames per read, malformed lengths, reconnect state reset, orderly close, and timeout. No test may contact the production servers.

**Step 3: Implement login/session state with runtime credential entry**

Add an explicit `--online` mode and a credential-entry UI; do not accept passwords on the command line or read credential files. Implement gateway authentication, redirection, game login, initialization, and session teardown from `docs/formats/net-protocol.md`. Unknown transition or authentication failure must stop without falling back to replay.

**Step 4: Route live messages through the existing consumers**

Live object messages use the same `WorldObjectStore`; `0x303f` uses `ChatModel`; panel responses use `PanelManager`. Implement `0x4050` behind `ChatSendSink`, including sequence/trailer handling and no optimistic success before the session accepts the frame.

**Step 5: Perform user-authorized live acceptance**

Only after replay and loopback suites pass, ask the user to perform live login and verification. Check world entry, six NPC identities/positions/directions, movement refresh, chat receive/send, and panel request/response for any newly implemented protocol. Do not print credentials or raw session values.

**Step 6: Checkpoint**

Expected: replay remains green; live results are recorded separately and never used to weaken offline assertions.

### Task 12: Final regression, documentation, and deliverable cleanup

**Files:**
- Modify: `client/README.md`
- Modify: `docs/VERIFY-2026-11-12-npc-hud-runtime.md`
- Modify: `docs/plans/2026-11-12-chat-npc-replication-handoff.md`
- Create only in ignored storage: final screenshots/logs under `.rea/` or `client/build/`

**Step 1: Run all standard Debug and Release tests**

Run the full commands at the top of this plan. Confirm real `m1_acceptance` and `m1_app_acceptance` are `Passed`, not `Skipped`.

**Step 2: Run the actual client in all supported modes**

Exercise local exploration, NPC replay, chat replay, every confirmed right-side panel, and authorized online mode if completed. Verify opening/closing detached chat and all panels does not leak or crash.

**Step 3: Run the existing window smoke test**

```bash
'D:/software/anaconda3/python.exe' tools/client_window_smoke.py \
  --client "$PWD/client/build/Release/dhsh_client.exe" \
  --shdata 'D:/software/dhsh/shdata' \
  --cache "$PWD/client/build/map-cache" --sample-seconds 25
```

Expected: existing PASS criteria remain satisfied. Record performance observations without inventing a new performance claim.

**Step 4: Update current limitations honestly**

Document replay format usage, online prerequisites, supported NPC/chat/panel messages, unresolved controls/protocols, and which panels intentionally show empty data. Remove obsolete claims about `10009`, roster hints, unassigned anchors, and approximate chat scaling.

**Step 5: Check the final worktree**

Run `git status --short`. Remove only temporary files created by this task that are not needed; leave pre-existing untracked/modified files alone. Do not add raw captures, credentials, huge extracted resources, or build logs to Git.

**Step 6: Final checkpoint**

Report tests actually run, original-versus-replica visual evidence, supported button/panel coverage, live-network status, and any remaining limitations. Do not mark unfinished live or panel work as complete.

## Execution handoff

The plan is designed for task-by-task execution with review after each checkpoint. A dedicated worktree is not required for this continuation because the target implementation exists only as uncommitted work in the current workspace; do not move or discard that work. Commit checkpoints remain disabled until the user explicitly requests commits.
