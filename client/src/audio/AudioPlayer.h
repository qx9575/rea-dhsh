#pragma once

#include "formats/MapFormats.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

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

class AudioStartGate;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual void play(const std::filesystem::path& file, bool loop,
        std::stop_token stop = {}) = 0;
    virtual void stop() = 0;

protected:
    bool start_playback(std::stop_token stop,
        const std::function<void()>& start);

private:
    std::shared_ptr<AudioStartGate> start_gate_;
    friend class AudioPlayer;
};

class AudioPlayer {
public:
    AudioPlayer();
    explicit AudioPlayer(std::unique_ptr<AudioBackend> backend);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool play(const MapMusicSelection& selection,
        const std::filesystem::path& shdata_dir, std::stop_token stop = {}) noexcept;
    void stop() noexcept;

    bool playing() const noexcept {
        return playing_;
    }

    const std::string& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    bool stop_backend() noexcept;
    void set_start_gate(const std::shared_ptr<AudioStartGate>& gate);

    std::unique_ptr<AudioBackend> backend_;
    bool playing_ = false;
    std::string diagnostic_;
    friend class AudioPlaybackWorker;
};

struct AudioPlaybackResult {
    bool playing = false;
    std::string diagnostic;
};

class AudioPlaybackWorker {
public:
    using PlayerFactory = std::function<std::unique_ptr<AudioPlayer>()>;

    AudioPlaybackWorker(MapMusicSelection selection, std::filesystem::path shdata_dir,
        PlayerFactory factory = {});
    ~AudioPlaybackWorker();

    AudioPlaybackWorker(const AudioPlaybackWorker&) = delete;
    AudioPlaybackWorker& operator=(const AudioPlaybackWorker&) = delete;

    void request_stop() noexcept;
    bool try_result(AudioPlaybackResult& result) const;

private:
    struct Shared;

    std::shared_ptr<Shared> shared_;
    std::shared_ptr<AudioStartGate> start_gate_;
    std::stop_source stop_source_;
    std::thread thread_;
};

} // namespace dhsh
