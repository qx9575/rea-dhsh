#pragma once

#include "formats/MapFormats.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct IMFMediaBuffer;

namespace dhsh {

struct MapMusicSelection {
    std::optional<std::string> path;
    std::string diagnostic;
};

MapMusicSelection select_map_music(const MapIni& ini);

void validate_audio_reader_flags(std::uint32_t flags);
void consume_locked_audio_buffer(IMFMediaBuffer& buffer,
    const std::function<void(const std::byte*, std::size_t)>& consumer);

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual void play(const std::filesystem::path& file, bool loop) = 0;
    virtual void stop() = 0;
};

class AudioPlayer {
public:
    AudioPlayer();
    explicit AudioPlayer(std::unique_ptr<AudioBackend> backend);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool play(const MapMusicSelection& selection,
        const std::filesystem::path& shdata_dir) noexcept;
    void stop() noexcept;

    bool playing() const noexcept {
        return playing_;
    }

    const std::string& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    bool stop_backend() noexcept;

    std::unique_ptr<AudioBackend> backend_;
    bool playing_ = false;
    std::string diagnostic_;
};

} // namespace dhsh
