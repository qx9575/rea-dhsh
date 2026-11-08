#include "audio/AudioPlayer.h"

#include "resource/ResourceError.h"

#ifndef _WIN32
#error "AudioPlayer requires Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throw_hresult(std::string_view operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(result) << ')';
    throw std::runtime_error(message.str());
}

void check_hresult(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw_hresult(operation, result);
    }
}

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            return;
        }
        check_hresult(result, "COM initialization");
        initialized_ = true;
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

private:
    bool initialized_ = false;
};

class MediaFoundationSession {
public:
    MediaFoundationSession() {
        check_hresult(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET),
            "Media Foundation initialization");
    }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;

    ~MediaFoundationSession() {
        MFShutdown();
    }
};

class MediaBufferLock {
public:
    explicit MediaBufferLock(IMFMediaBuffer& buffer)
        : buffer_(buffer) {
        check_hresult(buffer_.Lock(&bytes_, &capacity_, &size_),
            "Media Foundation PCM buffer lock");
    }

    MediaBufferLock(const MediaBufferLock&) = delete;
    MediaBufferLock& operator=(const MediaBufferLock&) = delete;

    ~MediaBufferLock() {
        if (locked_) {
            static_cast<void>(buffer_.Unlock());
        }
    }

    const BYTE* bytes() const noexcept {
        return bytes_;
    }

    DWORD capacity() const noexcept {
        return capacity_;
    }

    DWORD size() const noexcept {
        return size_;
    }

    void unlock() {
        const HRESULT result = buffer_.Unlock();
        locked_ = false;
        check_hresult(result, "Media Foundation PCM buffer unlock");
    }

private:
    IMFMediaBuffer& buffer_;
    BYTE* bytes_ = nullptr;
    DWORD capacity_ = 0;
    DWORD size_ = 0;
    bool locked_ = true;
};

struct DecodedAudio {
    WAVEFORMATEXTENSIBLE format{};
    std::vector<BYTE> pcm;
};

DecodedAudio decode_local_audio(const std::filesystem::path& file) {
    ComPtr<IMFByteStream> stream;
    check_hresult(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                      MF_FILEFLAGS_NONE, file.c_str(), &stream),
        "Media Foundation local file open");
    ComPtr<IMFSourceReader> reader;
    check_hresult(MFCreateSourceReaderFromByteStream(stream.Get(), nullptr, &reader),
        "Media Foundation source reader creation");

    ComPtr<IMFMediaType> requested;
    check_hresult(MFCreateMediaType(&requested),
        "Media Foundation output type creation");
    check_hresult(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio),
        "Media Foundation audio major type selection");
    check_hresult(requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM),
        "Media Foundation PCM subtype selection");
    check_hresult(reader->SetCurrentMediaType(
                      MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, requested.Get()),
        "Media Foundation PCM output selection");

    ComPtr<IMFMediaType> output;
    check_hresult(reader->GetCurrentMediaType(
                      MF_SOURCE_READER_FIRST_AUDIO_STREAM, &output),
        "Media Foundation decoded format query");
    WAVEFORMATEX* wave = nullptr;
    UINT32 wave_size = 0;
    check_hresult(MFCreateWaveFormatExFromMFMediaType(output.Get(), &wave, &wave_size),
        "Media Foundation wave format conversion");
    if (wave == nullptr || wave_size < sizeof(WAVEFORMATEX)
        || wave_size > sizeof(WAVEFORMATEXTENSIBLE)) {
        CoTaskMemFree(wave);
        throw std::runtime_error("Media Foundation returned an invalid PCM format");
    }
    DecodedAudio decoded;
    std::memcpy(&decoded.format, wave, wave_size);
    CoTaskMemFree(wave);

    const WAVEFORMATEX& format = decoded.format.Format;
    const bool pcm = format.wFormatTag == WAVE_FORMAT_PCM
        || (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && wave_size >= sizeof(WAVEFORMATEXTENSIBLE)
            && decoded.format.SubFormat == KSDATAFORMAT_SUBTYPE_PCM);
    if (!pcm || format.nChannels == 0 || format.nChannels > XAUDIO2_MAX_AUDIO_CHANNELS
        || format.nSamplesPerSec == 0
        || (format.wBitsPerSample != 8 && format.wBitsPerSample != 16
            && format.wBitsPerSample != 24 && format.wBitsPerSample != 32)
        || format.nBlockAlign != format.nChannels * format.wBitsPerSample / 8) {
        throw std::runtime_error("Media Foundation returned an unsupported PCM layout");
    }

    constexpr std::size_t max_decoded_bytes = 512U * 1024U * 1024U;
    while (true) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        const HRESULT read_result = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        check_hresult(read_result, "Media Foundation MP3 decode");
        validate_audio_reader_flags(flags);
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            throw std::runtime_error("Media Foundation MP3 decode reported a stream error");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (!sample) {
            continue;
        }
        ComPtr<IMFMediaBuffer> buffer;
        check_hresult(sample->ConvertToContiguousBuffer(&buffer),
            "Media Foundation PCM buffer conversion");
        consume_locked_audio_buffer(*buffer.Get(),
            [&](const std::byte* bytes, std::size_t size) {
                if (size > max_decoded_bytes
                    || decoded.pcm.size() > max_decoded_bytes - size
                    || decoded.pcm.size()
                        > std::numeric_limits<UINT32>::max() - size) {
                    throw std::runtime_error(
                        "decoded audio exceeds the playback buffer budget");
                }
                const BYTE* begin = reinterpret_cast<const BYTE*>(bytes);
                decoded.pcm.insert(decoded.pcm.end(), begin, begin + size);
            });
    }
    if (decoded.pcm.empty() || decoded.pcm.size() % format.nBlockAlign != 0) {
        throw std::runtime_error("Media Foundation decoded no complete PCM frames");
    }
    return decoded;
}

class XAudio2Backend final : public AudioBackend {
public:
    XAudio2Backend() {
        check_hresult(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR),
            "XAudio2 initialization");
        check_hresult(xaudio_->CreateMasteringVoice(&mastering_),
            "XAudio2 mastering voice initialization");
    }

    ~XAudio2Backend() override {
        stop();
        if (mastering_ != nullptr) {
            mastering_->DestroyVoice();
        }
        xaudio_.Reset();
    }

    void play(const std::filesystem::path& file, bool loop) override {
        DecodedAudio decoded = decode_local_audio(file);
        check_hresult(xaudio_->CreateSourceVoice(&source_, &decoded.format.Format),
            "XAudio2 source voice creation");
        pcm_ = std::move(decoded.pcm);
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(pcm_.size());
        buffer.pAudioData = pcm_.data();
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
        check_hresult(source_->SubmitSourceBuffer(&buffer),
            "XAudio2 PCM buffer submission");
        check_hresult(source_->Start(0, XAUDIO2_COMMIT_NOW),
            "XAudio2 playback start");
    }

    void stop() override {
        if (source_ != nullptr) {
            source_->Stop(0, XAUDIO2_COMMIT_NOW);
            source_->FlushSourceBuffers();
            source_->DestroyVoice();
            source_ = nullptr;
        }
        pcm_.clear();
    }

private:
    ComApartment com_;
    MediaFoundationSession media_foundation_;
    ComPtr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* mastering_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    std::vector<BYTE> pcm_;
};

} // namespace

void validate_audio_reader_flags(std::uint32_t flags) {
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
        throw FormatError(
            "Media Foundation media type changed during PCM decoding");
    }
}

void consume_locked_audio_buffer(IMFMediaBuffer& buffer,
    const std::function<void(const std::byte*, std::size_t)>& consumer) {
    MediaBufferLock lock(buffer);
    if (lock.size() > lock.capacity()
        || (lock.size() != 0 && lock.bytes() == nullptr)) {
        throw std::runtime_error("Media Foundation returned an invalid PCM buffer");
    }
    if (lock.size() != 0) {
        consumer(reinterpret_cast<const std::byte*>(lock.bytes()), lock.size());
    }
    lock.unlock();
}

MapMusicSelection select_map_music(const MapIni& ini) {
    const auto section = ini.find("ext");
    if (section == ini.end()) {
        return {std::nullopt, "MAP [ext] MUSIC is missing"};
    }
    const auto value = section->second.find("MUSIC");
    if (value == section->second.end()) {
        return {std::nullopt, "MAP [ext] MUSIC is missing"};
    }
    const std::string& text = value->second;
    std::uint32_t id = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), id);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return {std::nullopt, "invalid MAP [ext] MUSIC numeric ID: '" + text + "'"};
    }
    return {"sound/" + std::to_string(id) + ".mp3", {}};
}

AudioPlayer::AudioPlayer() {
    try {
        backend_ = std::make_unique<XAudio2Backend>();
    } catch (const std::exception& error) {
        diagnostic_ = "audio backend unavailable: " + std::string(error.what());
    }
}

AudioPlayer::AudioPlayer(std::unique_ptr<AudioBackend> backend)
    : backend_(std::move(backend)) {
    if (!backend_) {
        diagnostic_ = "audio backend unavailable: no backend was provided";
    }
}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::play(const MapMusicSelection& selection,
    const std::filesystem::path& shdata_dir) noexcept {
    if (playing_ && !stop_backend()) {
        return false;
    }
    if (!selection.path.has_value()) {
        diagnostic_ = selection.diagnostic.empty()
            ? "MAP MUSIC did not select an audio file"
            : selection.diagnostic;
        return false;
    }
    if (!backend_) {
        if (diagnostic_.empty()) {
            diagnostic_ = "audio backend unavailable";
        }
        return false;
    }
    diagnostic_.clear();

    const std::filesystem::path file = shdata_dir / *selection.path;
    try {
        backend_->play(file, true);
        playing_ = true;
        return true;
    } catch (const std::exception& error) {
        std::string diagnostic =
            "cannot play '" + file.generic_string() + "': " + error.what();
        if (!stop_backend()) {
            diagnostic += "; " + diagnostic_;
        }
        diagnostic_ = std::move(diagnostic);
        playing_ = false;
        return false;
    }
}

void AudioPlayer::stop() noexcept {
    static_cast<void>(stop_backend());
}

bool AudioPlayer::stop_backend() noexcept {
    playing_ = false;
    if (!backend_) {
        return true;
    }
    try {
        backend_->stop();
        return true;
    } catch (const std::exception& error) {
        diagnostic_ = "cannot stop audio: " + std::string(error.what());
        return false;
    }
}

} // namespace dhsh
