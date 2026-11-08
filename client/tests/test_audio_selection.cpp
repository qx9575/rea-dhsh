#include "audio/AudioPlayer.h"
#include "formats/MapFormats.h"
#include "resource/NzDecoder.h"
#include "resource/PakResourceProvider.h"
#include "resource/ResourceError.h"
#include "resource/ResourceProvider.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

dhsh::MapIni parse(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return dhsh::parse_map_ini(bytes);
}

struct BackendState {
    std::size_t play_calls = 0;
    std::size_t stop_calls = 0;
    bool active = false;
    bool loop = false;
    std::filesystem::path file;
    std::string failure;
};

class FakeAudioBackend final : public dhsh::AudioBackend {
public:
    explicit FakeAudioBackend(BackendState& state)
        : state_(state) {
    }

    void play(const std::filesystem::path& file, bool loop) override {
        ++state_.play_calls;
        if (!state_.failure.empty()) {
            throw std::runtime_error(state_.failure);
        }
        state_.file = file;
        state_.loop = loop;
        state_.active = true;
    }

    void stop() override {
        ++state_.stop_calls;
        state_.active = false;
    }

private:
    BackendState& state_;
};

class FakeMediaBuffer final : public IMFMediaBuffer {
public:
    BYTE* bytes = nullptr;
    DWORD capacity = 0;
    DWORD size = 0;
    std::size_t lock_calls = 0;
    std::size_t unlock_calls = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE Lock(
        BYTE** output, DWORD* max_length, DWORD* current_length) override {
        ++lock_calls;
        *output = bytes;
        *max_length = capacity;
        *current_length = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        ++unlock_calls;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD* length) override {
        *length = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD length) override {
        size = length;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD* length) override {
        *length = capacity;
        return S_OK;
    }
};

void test_media_buffer_unlocks_when_pcm_append_throws() {
    BYTE value = 0x42;
    FakeMediaBuffer buffer;
    buffer.bytes = &value;
    buffer.capacity = 1;
    buffer.size = 1;
    bool allocation_failure = false;
    try {
        dhsh::consume_locked_audio_buffer(buffer,
            [](const std::byte*, std::size_t) {
                throw std::bad_alloc();
            });
    } catch (const std::bad_alloc&) {
        allocation_failure = true;
    }
    check(allocation_failure && buffer.lock_calls == 1 && buffer.unlock_calls == 1,
        "PCM append failure must still unlock IMFMediaBuffer");
}

void test_empty_null_media_buffer_skips_copy_and_unlocks() {
    FakeMediaBuffer buffer;
    bool copied = false;
    dhsh::consume_locked_audio_buffer(buffer,
        [&](const std::byte*, std::size_t) {
            copied = true;
        });
    check(!copied && buffer.lock_calls == 1 && buffer.unlock_calls == 1,
        "empty null PCM buffer must skip pointer arithmetic and still unlock");
}

void test_map_music_selects_only_the_numbered_local_mp3() {
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(
        parse("[base]\nWIDTH=1\n[ext]\nMUSIC=19007\n"));
    check(selection.path == std::optional<std::string>("sound/19007.mp3"),
        "MUSIC must select only sound/<id>.mp3");
    check(selection.diagnostic.empty(), "valid MUSIC produced a diagnostic");

    const dhsh::MapMusicSelection canonical = dhsh::select_map_music(
        parse("[ext]\nMUSIC=0019007\n"));
    check(canonical.path == std::optional<std::string>("sound/19007.mp3"),
        "numeric MUSIC IDs must use the same canonical path as sound/%d.mp3");
}

void test_missing_music_is_non_throwing_and_diagnostic() {
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(
        parse("[base]\nWIDTH=1\n"));
    check(!selection.path.has_value(), "missing MUSIC must not select a file");
    check(contains(selection.diagnostic, "MUSIC"),
        "missing MUSIC must explain why no music was selected");
}

void test_invalid_music_ids_cannot_escape_the_sound_directory() {
    const std::vector<std::string> invalid = {
        "", "abc", "19007.mp3", "19007x", "-1", "+1", "0x19007",
        "../19007", "19007/19008", "19007\\19008", "http://example/19007",
        "//server/share/19007", "4294967296",
    };
    for (const std::string& id : invalid) {
        const dhsh::MapMusicSelection selection = dhsh::select_map_music(
            parse("[ext]\nMUSIC=" + id + "\n"));
        check(!selection.path.has_value(), "invalid MUSIC ID selected a path: " + id);
        check(contains(selection.diagnostic, "invalid") &&
                contains(selection.diagnostic, "MUSIC"),
            "invalid MUSIC ID lacks a useful diagnostic: " + id);
    }
}

void test_audio_player_passes_a_local_file_and_loop_to_the_backend() {
    BackendState state;
    dhsh::AudioPlayer player(std::make_unique<FakeAudioBackend>(state));
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(
        parse("[ext]\nMUSIC=19007\n"));
    const std::filesystem::path shdata("read-only-shdata");
    check(player.play(selection, shdata), "valid local music did not start");
    check(player.playing(), "AudioPlayer did not retain its playing state");
    check(state.play_calls == 1 && state.active && state.loop,
        "backend did not receive exactly one looping playback request");
    check(state.file == shdata / "sound/19007.mp3",
        "backend received a path outside the local shdata sound directory");
    check(player.diagnostic().empty(), "successful playback produced a diagnostic");
}

void test_file_decode_and_initialization_failures_are_diagnostic_and_non_throwing() {
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(
        parse("[ext]\nMUSIC=19007\n"));
    const std::filesystem::path shdata("read-only-shdata");
    for (const std::string failure : {
             "local audio file not found",
             "Media Foundation MP3 decode failed",
             "XAudio2 initialization failed",
         }) {
        BackendState state;
        state.failure = failure;
        dhsh::AudioPlayer player(std::make_unique<FakeAudioBackend>(state));
        check(!player.play(selection, shdata),
            "backend failure incorrectly reported active playback: " + failure);
        check(!player.playing() && !state.active,
            "backend failure left playback active: " + failure);
        check(contains(player.diagnostic(), "19007.mp3") &&
                contains(player.diagnostic(), failure),
            "backend failure lost its path or cause: " + player.diagnostic());
    }

    dhsh::AudioPlayer unavailable(nullptr);
    check(!unavailable.play(selection, shdata),
        "an unavailable audio backend must remain silent");
    check(contains(unavailable.diagnostic(), "unavailable"),
        "an unavailable audio backend lacks a diagnostic");
}

void test_replacement_stop_and_destruction_leave_no_residual_playback() {
    BackendState state;
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(
        parse("[ext]\nMUSIC=19007\n"));
    const std::filesystem::path shdata("read-only-shdata");
    {
        dhsh::AudioPlayer player(std::make_unique<FakeAudioBackend>(state));
        check(player.play(selection, shdata), "first playback failed");
        check(player.play(selection, shdata), "replacement playback failed");
        check(state.stop_calls == 1 && state.play_calls == 2 && state.active,
            "scene reload did not stop the previous voice before replacement");
        player.stop();
        check(!player.playing() && !state.active && state.stop_calls == 2,
            "explicit stop left a playing voice");
        check(player.play(selection, shdata), "restart after stop failed");
    }
    check(!state.active && state.stop_calls == 3,
        "AudioPlayer destruction left residual playback");
}

void test_missing_or_invalid_selection_stops_previous_map_music() {
    BackendState state;
    dhsh::AudioPlayer player(std::make_unique<FakeAudioBackend>(state));
    check(player.play(dhsh::select_map_music(parse("[ext]\nMUSIC=19007\n")),
              "read-only-shdata"),
        "initial playback failed");
    check(!player.play(dhsh::select_map_music(parse("[base]\nWIDTH=1\n")),
              "read-only-shdata"),
        "a map without MUSIC must not keep playing the previous map music");
    check(!player.playing() && !state.active && state.stop_calls == 1,
        "switching to a map without MUSIC left residual playback");
    check(contains(player.diagnostic(), "MUSIC"),
        "switching to a map without MUSIC lost its diagnostic");
}

std::optional<std::string> find_real_map(dhsh::ResourceProvider& resources) {
    const std::vector<std::string> candidates = {
        "map/YingTianFu.map",
        "config/map/YingTianFu.map",
        "YingTianFu.map",
    };
    for (const std::string& candidate : candidates) {
        if (resources.exists(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

void verify_real_music_selection_and_playback(const std::filesystem::path& shdata) {
    dhsh::PakResourceProvider resources(shdata, {"config.pak", "map.pak"});
    const std::optional<std::string> map_path = find_real_map(resources);
    check(map_path.has_value(),
        "real YingTianFu MAP was not found in config.pak/map.pak");

    const std::vector<std::byte> stored = resources.bytes(*map_path);
    const dhsh::MapIni ini =
        dhsh::parse_map_ini(dhsh::NzDecoder::decode(stored));
    const dhsh::MapMusicSelection selection = dhsh::select_map_music(ini);
    check(selection.path.has_value(),
        "real MAP '" + *map_path + "' has no usable MUSIC: "
            + selection.diagnostic);
    // The YingTianFu fixture is the one real map with a confirmed local
    // music binding; any other MUSIC ID means the fixture or parser changed.
    check(*selection.path == "sound/19007.mp3",
        "real MAP MUSIC must select sound/19007.mp3, got '" + *selection.path + "'");

    const std::filesystem::path music = shdata / *selection.path;
    std::error_code error;
    const auto size = std::filesystem::file_size(music, error);
    check(!error && size > 0,
        "real music file is missing or empty: " + music.string());

    std::cout << "Audio real map=" << *map_path
              << " music=" << *selection.path << " bytes=" << size << '\n';

    dhsh::AudioPlayer player;
    const bool playing = player.play(selection, shdata);
    std::cout << "Audio real playback playing=" << (playing ? "true" : "false");
    if (!player.diagnostic().empty()) {
        std::cout << " diagnostic=" << player.diagnostic();
    }
    std::cout << '\n';

    check(playing && player.playing(),
        "real MP3 playback must start when the fixture exists; diagnostic: "
            + player.diagnostic());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(player.playing(),
        "real playback stopped before the test requested a stop");
    player.stop();
    check(!player.playing(), "real stop left the player active");
    check(player.diagnostic().empty(),
        "real stop produced a diagnostic: " + player.diagnostic());
    std::cout << "Audio real playback playing=false stopped=true\n";
    std::cout << "Audio real result=played (the local MP3 was started and"
                 " stopped; audibility is not verified)\n";
}

int real_main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: dhsh_test_audio_selection --real <shdata>\n";
        return 2;
    }
    try {
        verify_real_music_selection_and_playback(argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: real audio selection: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--real") {
        return real_main(argc, argv);
    }
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"numbered local MP3 selection", test_map_music_selects_only_the_numbered_local_mp3},
        {"missing MUSIC diagnostics", test_missing_music_is_non_throwing_and_diagnostic},
        {"invalid MUSIC IDs and no network paths", test_invalid_music_ids_cannot_escape_the_sound_directory},
        {"local looping backend request", test_audio_player_passes_a_local_file_and_loop_to_the_backend},
        {"non-throwing backend failures", test_file_decode_and_initialization_failures_are_diagnostic_and_non_throwing},
        {"replacement and destruction stop", test_replacement_stop_and_destruction_leave_no_residual_playback},
        {"selection changes stop old music", test_missing_or_invalid_selection_stops_previous_map_music},
        {"PCM append failure unlocks buffer", test_media_buffer_unlocks_when_pcm_append_throws},
        {"empty null PCM buffer skips copy", test_empty_null_media_buffer_skips_copy_and_unlocks},
        {"source reader rejects PCM media type changes", [] {
             dhsh::validate_audio_reader_flags(0);
             bool rejected = false;
             try {
                 dhsh::validate_audio_reader_flags(
                     MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED);
             } catch (const dhsh::FormatError& error) {
                 rejected = contains(error.what(), "media type changed");
             }
             check(rejected,
                 "CURRENTMEDIATYPECHANGED must produce an explicit FormatError");
         }},
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
