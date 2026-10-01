// Xbox DirectSound replacement on XAudio2.
//
// The game uses DirectSound buffers (sound effects from the wave bank, SetBufferData on memory it owns) and streams
// (music, ambience and voice, fed with XMEDIAPACKETs). Buffers and streams become XAudio2 source voices; Xbox ADPCM
// (format 0x69) is decoded to 16-bit PCM. Stream packets complete on the game thread in DirectSoundDoWork, as on the
// console. Without an audio device (or with CW_AUDIO=0) the same objects keep the old silent timing behaviour.
//
// Environment: CW_AUDIO=0 disables output; CW_AUDIO_VOLUME=0..2 scales the master volume (default 1);
// CW_AUDIO_MIN_DISTANCE sets the distance where 3D sounds start to fade (default 15 game units);
// CW_AUDIO_STATS=<ms> logs active voices, glitches and output peak/RMS levels periodically (for tests).

#include "Hle.h"

#include "Log.h"
#include "audio/XboxAdpcm.h"

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace cw::hle {

namespace {

using Clock = std::chrono::steady_clock;

constexpr HRESULT kDsOk = 0;
constexpr DWORD kStatusPlaying = 0x1;
constexpr DWORD kStatusLooping = 0x4;
constexpr DWORD kPlayLooping = 0x1;
constexpr DWORD kPacketSuccess = 0;
constexpr DWORD kPacketPending = 0x8000000A;
constexpr DWORD kPacketFlushed = 0x8000000B;
constexpr DWORD kBufferCtrl3d = 0x10;
constexpr DWORD kStreamPause = 0x1;

struct XMediaPacket {
    void* buffer;
    DWORD maxSize;
    DWORD* completedSize;
    DWORD* status;
    void* contextOrEvent;
    LONGLONG* timestamp;
};

struct StreamDesc {
    DWORD flags;
    DWORD maxAttachedPackets;
    WAVEFORMATEX* format;
    void(__stdcall* callback)(void* streamContext, void* packetContext, DWORD status);
    void* context;
    void* mixBins;
};

struct BufferDesc {
    DWORD size;
    DWORD flags;
    DWORD bufferBytes;
    WAVEFORMATEX* format;
    void* mixBins;
    DWORD inputMixBin;
};

struct Vector3 {
    float x, y, z;
};

// ------------------------------------------------------------------------------------------------ format helpers

struct Format {
    WORD tag = kWaveFormatPcmTag;
    WORD channels = 2;
    DWORD rate = 44100;
    WORD blockAlign = 4;
    WORD bits = 16;
    static constexpr WORD kWaveFormatPcmTag = 1;

    static Format from(const WAVEFORMATEX* format) {
        Format result;
        if (format != nullptr) {
            result.tag = format->wFormatTag;
            result.channels = std::max<WORD>(1, format->nChannels);
            result.rate = format->nSamplesPerSec != 0 ? format->nSamplesPerSec : 44100;
            result.blockAlign = std::max<WORD>(1, format->nBlockAlign);
            result.bits = format->wBitsPerSample;
        }
        return result;
    }
    bool adpcm() const { return tag == audio::kWaveFormatXboxAdpcm; }
    DWORD bytesPerSecond() const {
        return adpcm() ? rate * 36 * channels / 64 : rate * blockAlign;
    }
    // Byte offset (in the source data) -> sample frame.
    UINT32 frameOf(DWORD bytes) const {
        return adpcm() ? static_cast<UINT32>(audio::xboxAdpcmFrames(bytes, channels)) : bytes / blockAlign;
    }
    WAVEFORMATEX pcm() const {
        WAVEFORMATEX out{};
        out.wFormatTag = WAVE_FORMAT_PCM;
        out.nChannels = channels;
        out.nSamplesPerSec = rate;
        out.wBitsPerSample = adpcm() ? 16 : (bits == 8 ? 8 : 16);
        out.nBlockAlign = static_cast<WORD>(out.nChannels * out.wBitsPerSample / 8);
        out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
        return out;
    }
};

float volumeToAmplitude(LONG hundredthsOfDb) {
    if (hundredthsOfDb <= -10000) {
        return 0.0f;
    }
    return std::pow(10.0f, static_cast<float>(hundredthsOfDb) / 2000.0f);
}

float environmentFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : static_cast<float>(std::atof(value));
}

// ------------------------------------------------------------------------------------------------ engine

std::recursive_mutex g_audioLock;
IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
float g_masterVolume = 1.0f;
float g_minDistance = 15.0f;

struct Listener {
    Vector3 position{0, 0, 0};
    Vector3 front{0, 0, 1};
    Vector3 top{0, 1, 0};
} g_listener;

// Decoded PCM shared between buffers that play the same wave bank entry.
struct PcmData {
    std::vector<std::int16_t> samples;
};
std::map<std::pair<const void*, DWORD>, std::weak_ptr<PcmData>> g_decodedCache;

std::shared_ptr<PcmData> decodedFor(const void* data, DWORD bytes, const Format& format) {
    auto& slot = g_decodedCache[{data, bytes}];
    if (auto existing = slot.lock()) {
        return existing;
    }
    auto decoded = std::make_shared<PcmData>();
    decoded->samples = audio::decodeXboxAdpcm(static_cast<const std::uint8_t*>(data), bytes, format.channels);
    slot = decoded;
    return decoded;
}

// Stereo pan and distance gain for a 3D source relative to the listener.
void spatialize(IXAudio2SourceVoice* voice, const Format& format, const Vector3& position, bool headRelative, float baseVolume) {
    Vector3 offset = position;
    if (!headRelative) {
        offset = {position.x - g_listener.position.x, position.y - g_listener.position.y, position.z - g_listener.position.z};
    }
    const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    // Right vector = top x front (left-handed DirectSound coordinates).
    const Vector3& f = g_listener.front;
    const Vector3& t = g_listener.top;
    Vector3 right{t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
    const float rightLength = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    float pan = 0.0f;
    if (distance > 0.001f && rightLength > 0.001f) {
        pan = (offset.x * right.x + offset.y * right.y + offset.z * right.z) / (distance * rightLength);
    }
    const float gain = baseVolume * (distance <= g_minDistance ? 1.0f : g_minDistance / distance);
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
    const float left = std::cos(angle) * gain * 1.41421356f;
    const float rightGain = std::sin(angle) * gain * 1.41421356f;
    // Source channels -> 2 output channels.
    float matrix[16] = {};
    if (format.channels == 1) {
        matrix[0] = left;
        matrix[1] = rightGain;
    } else {
        matrix[0] = left;
        matrix[3] = rightGain;
    }
    voice->SetOutputMatrix(nullptr, std::min<UINT32>(format.channels, 2), 2, matrix);
    voice->SetVolume(1.0f);
}

// ------------------------------------------------------------------------------------------------ buffers

struct Buffer {
    LONG references = 1;
    Format format;
    DWORD flags = 0;
    const void* data = nullptr;
    DWORD dataBytes = 0;
    DWORD loopStart = 0;
    DWORD loopLength = 0;
    DWORD playStart = 0;
    DWORD playLength = 0;
    LONG volume = 0;
    DWORD headroom = 600;  // Xbox default for buffers: 6 dB
    DWORD frequency = 0;
    Vector3 position{0, 0, 0};
    bool playing = false;
    bool looping = false;
    Clock::time_point stopTime{};  // silent fallback
    IXAudio2SourceVoice* voice = nullptr;
    Format voiceFormat;
    std::shared_ptr<PcmData> decoded;

    bool is3d() const { return (flags & kBufferCtrl3d) != 0; }
};

void applyBufferMix(Buffer* buffer) {
    if (buffer->voice == nullptr) {
        return;
    }
    const float amplitude = volumeToAmplitude(buffer->volume - static_cast<LONG>(buffer->headroom)) * g_masterVolume;
    if (buffer->is3d()) {
        spatialize(buffer->voice, buffer->format, buffer->position, false, amplitude);
    } else {
        buffer->voice->SetVolume(amplitude);
    }
    const float ratio = buffer->frequency != 0 ? static_cast<float>(buffer->frequency) / static_cast<float>(buffer->format.rate) : 1.0f;
    buffer->voice->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, 4.0f));
}

bool ensureBufferVoice(Buffer* buffer) {
    if (g_xaudio == nullptr) {
        return false;
    }
    if (buffer->voice != nullptr && buffer->voiceFormat.tag == buffer->format.tag && buffer->voiceFormat.channels == buffer->format.channels &&
        buffer->voiceFormat.rate == buffer->format.rate && buffer->voiceFormat.bits == buffer->format.bits) {
        return true;
    }
    if (buffer->voice != nullptr) {
        buffer->voice->DestroyVoice();
        buffer->voice = nullptr;
    }
    const WAVEFORMATEX pcm = buffer->format.pcm();
    if (FAILED(g_xaudio->CreateSourceVoice(&buffer->voice, &pcm, 0, 4.0f))) {
        buffer->voice = nullptr;
        return false;
    }
    buffer->voiceFormat = buffer->format;
    return true;
}

void stopVoice(IXAudio2SourceVoice* voice) {
    if (voice != nullptr) {
        voice->Stop(0);
        voice->FlushSourceBuffers();
    }
}

// ------------------------------------------------------------------------------------------------ streams

struct Stream;

struct QueuedPacket {
    Stream* stream;
    XMediaPacket packet;
    std::vector<std::int16_t> decoded;
    Clock::time_point due{};  // silent fallback
};

struct VoiceCallback : IXAudio2VoiceCallback {
    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void*) override {}
    void __stdcall OnBufferEnd(void* context) override;
    void __stdcall OnLoopEnd(void*) override {}
    void __stdcall OnVoiceError(void*, HRESULT error) override { logf("audio: voice error 0x%08lX", error); }
};
VoiceCallback g_voiceCallback;

struct Stream {
    const void* const* vtable;
    LONG references = 1;
    StreamDesc desc{};
    Format format;
    LONG volume = 0;
    DWORD headroom = 0;  // Xbox default for streams
    DWORD frequency = 0;
    Vector3 position{0, 0, 0};
    bool paused = false;
    IXAudio2SourceVoice* voice = nullptr;
    std::deque<std::unique_ptr<QueuedPacket>> pending;
    Clock::time_point nextDue{};
};

std::deque<Stream*> g_streams;
std::mutex g_finishedLock;
std::vector<QueuedPacket*> g_finished;  // filled by the XAudio2 thread, completed in DirectSoundDoWork

void __stdcall VoiceCallback::OnBufferEnd(void* context) {
    std::lock_guard lock(g_finishedLock);
    g_finished.push_back(static_cast<QueuedPacket*>(context));
}

void completePacket(Stream* stream, XMediaPacket& packet, DWORD status) {
    if (packet.completedSize != nullptr) {
        *packet.completedSize = status == kPacketSuccess ? packet.maxSize : 0;
    }
    if (packet.status != nullptr) {
        *packet.status = status;
    }
    if (stream->desc.callback != nullptr) {
        stream->desc.callback(stream->desc.context, packet.contextOrEvent, status);
    } else if (packet.contextOrEvent != nullptr) {
        SetEvent(static_cast<HANDLE>(packet.contextOrEvent));
    }
}

void applyStreamMix(Stream* stream) {
    if (stream->voice == nullptr) {
        return;
    }
    stream->voice->SetVolume(volumeToAmplitude(stream->volume - static_cast<LONG>(stream->headroom)) * g_masterVolume);
    const float ratio = stream->frequency != 0 ? static_cast<float>(stream->frequency) / static_cast<float>(stream->format.rate) : 1.0f;
    stream->voice->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, 4.0f));
}

// ---------------------------------------------------------------- XMediaObject vtable for streams

ULONG __stdcall streamAddRef(Stream* stream) {
    return static_cast<ULONG>(InterlockedIncrement(&stream->references));
}

ULONG __stdcall streamRelease(Stream* stream) {
    const LONG remaining = InterlockedDecrement(&stream->references);
    if (remaining == 0) {
        std::lock_guard lock(g_audioLock);
        if (stream->voice != nullptr) {
            stream->voice->DestroyVoice();  // synchronous: no callbacks run after this
        }
        {
            std::lock_guard finished(g_finishedLock);
            std::erase_if(g_finished, [stream](QueuedPacket* packet) { return packet->stream == stream; });
        }
        std::erase(g_streams, stream);
        delete stream;
    }
    return static_cast<ULONG>(std::max<LONG>(remaining, 0));
}

HRESULT __stdcall streamGetInfo(Stream* stream, DWORD* info) {
    info[0] = 0;
    info[1] = 0;
    info[2] = 0;
    return kDsOk;
}

HRESULT __stdcall streamGetStatus(Stream* stream, DWORD* status) {
    std::lock_guard lock(g_audioLock);
    // XMO_STATUSF_ACCEPT_INPUT_DATA
    *status = stream->pending.size() < std::max<DWORD>(1, stream->desc.maxAttachedPackets) ? 1 : 0;
    return kDsOk;
}

HRESULT __stdcall streamProcess(Stream* stream, const XMediaPacket* input, const XMediaPacket* output) {
    if (input == nullptr) {
        return kDsOk;
    }
    std::lock_guard lock(g_audioLock);
    if (input->status != nullptr) {
        *input->status = kPacketPending;
    }
    auto queued = std::make_unique<QueuedPacket>();
    queued->stream = stream;
    queued->packet = *input;
    if (stream->voice != nullptr) {
        XAUDIO2_BUFFER submit{};
        if (stream->format.adpcm()) {
            queued->decoded = audio::decodeXboxAdpcm(static_cast<const std::uint8_t*>(input->buffer), input->maxSize, stream->format.channels);
            submit.AudioBytes = static_cast<UINT32>(queued->decoded.size() * sizeof(std::int16_t));
            submit.pAudioData = reinterpret_cast<const BYTE*>(queued->decoded.data());
        } else {
            submit.AudioBytes = input->maxSize;
            submit.pAudioData = static_cast<const BYTE*>(input->buffer);
        }
        submit.pContext = queued.get();
        if (submit.AudioBytes == 0 || FAILED(stream->voice->SubmitSourceBuffer(&submit))) {
            // Nothing to play: complete it on the next DoWork.
            std::lock_guard finished(g_finishedLock);
            g_finished.push_back(queued.get());
        } else if (!stream->paused) {
            stream->voice->Start(0);
        }
    } else {
        const auto now = Clock::now();
        if (stream->nextDue < now) {
            stream->nextDue = now;
        }
        stream->nextDue += std::chrono::microseconds(static_cast<long long>(input->maxSize) * 1000000 / std::max<DWORD>(1, stream->format.bytesPerSecond()));
        queued->due = stream->nextDue;
    }
    stream->pending.push_back(std::move(queued));
    return kDsOk;
}

HRESULT __stdcall streamDiscontinuity(Stream* stream) {
    return kDsOk;
}

HRESULT __stdcall streamFlush(Stream* stream) {
    std::lock_guard lock(g_audioLock);
    // Completion callbacks may release the stream.
    streamAddRef(stream);
    struct Releaser {
        Stream* stream;
        ~Releaser() { streamRelease(stream); }
    } releaser{stream};
    if (stream->voice != nullptr) {
        stream->voice->Stop(0);
        stream->voice->FlushSourceBuffers();
    }
    {
        std::lock_guard finished(g_finishedLock);
        std::erase_if(g_finished, [stream](QueuedPacket* packet) { return packet->stream == stream; });
    }
    while (!stream->pending.empty()) {
        std::unique_ptr<QueuedPacket> packet = std::move(stream->pending.front());
        stream->pending.pop_front();
        completePacket(stream, packet->packet, kPacketFlushed);
    }
    stream->nextDue = {};
    // FlushSourceBuffers reports the flushed buffers later; they no longer exist, so wait for the voice to drain.
    if (stream->voice != nullptr) {
        XAUDIO2_VOICE_STATE state{};
        for (int spin = 0; spin < 200; ++spin) {
            stream->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                break;
            }
            Sleep(1);
        }
        std::lock_guard finished(g_finishedLock);
        std::erase_if(g_finished, [stream](QueuedPacket* packet) { return packet->stream == stream; });
    }
    return kDsOk;
}

const void* const kStreamVtable[] = {
    reinterpret_cast<const void*>(&streamAddRef),
    reinterpret_cast<const void*>(&streamRelease),
    reinterpret_cast<const void*>(&streamGetInfo),
    reinterpret_cast<const void*>(&streamGetStatus),
    reinterpret_cast<const void*>(&streamProcess),
    reinterpret_cast<const void*>(&streamDiscontinuity),
    reinterpret_cast<const void*>(&streamFlush),
};

// ---------------------------------------------------------------- DirectSound C API

std::uint8_t g_directSound[0x100] = {};
std::uint8_t g_effectsImageDescription[0x1000] = {};

// Output level meter on the mastering voice, sampled by a logging thread.
void startStatistics(DWORD periodMs) {
    IUnknown* meter = nullptr;
    if (FAILED(XAudio2CreateVolumeMeter(&meter, 0))) {
        return;
    }
    XAUDIO2_EFFECT_DESCRIPTOR descriptor{meter, TRUE, 2};
    XAUDIO2_EFFECT_CHAIN chain{1, &descriptor};
    const bool attached = SUCCEEDED(g_master->SetEffectChain(&chain));
    meter->Release();
    if (!attached) {
        return;
    }
    CreateThread(nullptr, 0, [](LPVOID parameter) -> DWORD {
        const DWORD period = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(parameter));
        for (;;) {
            Sleep(period);
            float peak[2] = {};
            float rms[2] = {};
            XAUDIO2FX_VOLUMEMETER_LEVELS levels{peak, rms, 2};
            g_master->GetEffectParameters(0, &levels, sizeof(levels));
            XAUDIO2_PERFORMANCE_DATA performance{};
            g_xaudio->GetPerformanceData(&performance);
            logf("audio stats: voices %u/%u glitches %u peak %.3f rms %.3f", performance.ActiveSourceVoiceCount,
                performance.TotalSourceVoiceCount, performance.GlitchesSinceEngineStarted, std::max(peak[0], peak[1]), std::max(rms[0], rms[1]));
        }
    }, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(std::max<DWORD>(100, periodMs))), 0, nullptr);
}

void startEngine() {
    const char* setting = std::getenv("CW_AUDIO");
    if (setting != nullptr && std::strcmp(setting, "0") == 0) {
        logf("audio: disabled by CW_AUDIO=0 (silent)");
        return;
    }
    g_masterVolume = std::clamp(environmentFloat("CW_AUDIO_VOLUME", 1.0f), 0.0f, 2.0f);
    g_minDistance = std::max(0.1f, environmentFloat("CW_AUDIO_MIN_DISTANCE", 15.0f));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        g_xaudio = nullptr;
        logf("audio: XAudio2 unavailable (silent)");
        return;
    }
    if (FAILED(g_xaudio->CreateMasteringVoice(&g_master, 2, 48000))) {
        g_xaudio->Release();
        g_xaudio = nullptr;
        logf("audio: no output device (silent)");
        return;
    }
    logf("audio: XAudio2 output, master volume %.2f", g_masterVolume);
    if (const char* stats = std::getenv("CW_AUDIO_STATS")) {
        startStatistics(static_cast<DWORD>(std::strtoul(stats, nullptr, 10)));
    }
}

HRESULT __stdcall xDirectSoundCreate(void* guid, void** directSound, void* outer) {
    std::lock_guard lock(g_audioLock);
    if (g_xaudio == nullptr) {
        startEngine();
    }
    *directSound = g_directSound;
    return kDsOk;
}

HRESULT __stdcall xDirectSoundCreateStream(StreamDesc* desc, Stream** stream) {
    std::lock_guard lock(g_audioLock);
    auto* created = new Stream{kStreamVtable};
    created->desc = *desc;
    created->format = Format::from(desc->format);
    created->desc.format = nullptr;  // the caller's format struct may not outlive this call
    if (g_xaudio != nullptr) {
        const WAVEFORMATEX pcm = created->format.pcm();
        if (FAILED(g_xaudio->CreateSourceVoice(&created->voice, &pcm, 0, 4.0f, &g_voiceCallback))) {
            created->voice = nullptr;
        } else {
            applyStreamMix(created);
        }
    }
    g_streams.push_back(created);
    *stream = created;
    return kDsOk;
}

void __stdcall xDirectSoundDoWork() {
    std::lock_guard lock(g_audioLock);
    // Packet callbacks may release streams; keep every stream alive until this pass is done.
    std::vector<Stream*> streams(g_streams.begin(), g_streams.end());
    for (Stream* stream : streams) {
        streamAddRef(stream);
    }
    std::vector<QueuedPacket*> finished;
    {
        std::lock_guard guard(g_finishedLock);
        finished.swap(g_finished);
    }
    for (QueuedPacket* done : finished) {
        Stream* stream = done->stream;
        if (std::find(streams.begin(), streams.end(), stream) == streams.end()) {
            continue;
        }
        auto entry = std::find_if(stream->pending.begin(), stream->pending.end(), [done](const auto& packet) { return packet.get() == done; });
        if (entry == stream->pending.end()) {
            continue;
        }
        std::unique_ptr<QueuedPacket> packet = std::move(*entry);
        stream->pending.erase(entry);
        completePacket(stream, packet->packet, kPacketSuccess);
    }
    // Silent fallback: complete packets on a timer.
    const auto now = Clock::now();
    for (Stream* stream : streams) {
        if (stream->voice != nullptr) {
            continue;
        }
        while (!stream->pending.empty() && stream->pending.front()->due <= now) {
            std::unique_ptr<QueuedPacket> packet = std::move(stream->pending.front());
            stream->pending.pop_front();
            completePacket(stream, packet->packet, kPacketSuccess);
        }
    }
    for (Stream* stream : streams) {
        streamRelease(stream);
    }
}

void __stdcall xDirectSoundUseLightHRTF() {
}

void __stdcall xDirectSoundOverrideSpeakerConfig(DWORD config) {
}

ULONG __stdcall xDirectSoundRelease(void* directSound) {
    return 1;
}

HRESULT __stdcall xGetSpeakerConfig(void* directSound, DWORD* config) {
    *config = 0x00000002;
    return kDsOk;
}

HRESULT __stdcall xDownloadEffectsImage(void* directSound, const void* image, DWORD size, void* location, void** description) {
    if (description != nullptr) {
        reinterpret_cast<DWORD*>(g_effectsImageDescription)[0] = 0x40;
        *description = g_effectsImageDescription;
    }
    return kDsOk;
}

// IDirectSound_SetAllParameters(this, const DS3DLISTENER*, apply): position, velocity, front, top, factors.
HRESULT __stdcall xSetListenerParameters(void* directSound, const float* parameters, DWORD apply) {
    if (parameters == nullptr) {
        return kDsOk;
    }
    std::lock_guard lock(g_audioLock);
    static bool logged = false;
    // The Xbox struct starts with dwSize (64) like the PC one; tolerate a missing size field.
    const float* values = reinterpret_cast<const DWORD*>(parameters)[0] == 64 ? parameters + 1 : parameters;
    g_listener.position = {values[0], values[1], values[2]};
    g_listener.front = {values[6], values[7], values[8]};
    g_listener.top = {values[9], values[10], values[11]};
    if (!logged) {
        logged = true;
        logf("audio: listener at (%.1f, %.1f, %.1f) front (%.2f, %.2f, %.2f) top (%.2f, %.2f, %.2f)", values[0], values[1], values[2],
            values[6], values[7], values[8], values[9], values[10], values[11]);
    }
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk1(void*) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk2(void*, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk3(void*, DWORD, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xCreateSoundBuffer(void* directSound, const BufferDesc* desc, Buffer** buffer, void* outer) {
    auto* created = new Buffer;
    if (desc != nullptr) {
        created->format = Format::from(desc->format);
        created->flags = desc->flags;
        created->dataBytes = desc->bufferBytes;
    }
    *buffer = created;
    return kDsOk;
}

ULONG __stdcall xBufferRelease(Buffer* buffer) {
    const LONG remaining = InterlockedDecrement(&buffer->references);
    if (remaining == 0) {
        std::lock_guard lock(g_audioLock);
        if (buffer->voice != nullptr) {
            buffer->voice->DestroyVoice();
        }
        delete buffer;
    }
    return static_cast<ULONG>(std::max<LONG>(remaining, 0));
}

HRESULT __stdcall xBufferSetBufferData(Buffer* buffer, void* data, DWORD bytes) {
    std::lock_guard lock(g_audioLock);
    stopVoice(buffer->voice);
    buffer->playing = false;
    buffer->data = data;
    buffer->dataBytes = bytes;
    buffer->decoded.reset();
    buffer->loopStart = buffer->loopLength = buffer->playStart = buffer->playLength = 0;
    return kDsOk;
}

HRESULT __stdcall xBufferSetFormat(Buffer* buffer, const WAVEFORMATEX* format) {
    std::lock_guard lock(g_audioLock);
    buffer->format = Format::from(format);
    buffer->decoded.reset();
    return kDsOk;
}

HRESULT __stdcall xBufferPlay(Buffer* buffer, DWORD reserved1, DWORD reserved2, DWORD flags) {
    std::lock_guard lock(g_audioLock);
    const bool looping = (flags & kPlayLooping) != 0;
    if (buffer->playing && buffer->voice != nullptr) {
        XAUDIO2_VOICE_STATE state{};
        buffer->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued > 0) {
            buffer->looping = looping;  // already playing: Play only updates the looping flag
            return kDsOk;
        }
    }
    buffer->playing = true;
    buffer->looping = looping;
    buffer->stopTime = Clock::now() + std::chrono::microseconds(static_cast<long long>(buffer->dataBytes) * 1000000 / std::max<DWORD>(1, buffer->format.bytesPerSecond()));
    if (buffer->data == nullptr || buffer->dataBytes == 0 || !ensureBufferVoice(buffer)) {
        return kDsOk;
    }
    XAUDIO2_BUFFER submit{};
    UINT32 totalFrames;
    if (buffer->format.adpcm()) {
        if (!buffer->decoded) {
            buffer->decoded = decodedFor(buffer->data, buffer->dataBytes, buffer->format);
        }
        submit.AudioBytes = static_cast<UINT32>(buffer->decoded->samples.size() * sizeof(std::int16_t));
        submit.pAudioData = reinterpret_cast<const BYTE*>(buffer->decoded->samples.data());
        totalFrames = static_cast<UINT32>(buffer->decoded->samples.size() / buffer->format.channels);
    } else {
        submit.AudioBytes = buffer->dataBytes - buffer->dataBytes % buffer->format.blockAlign;
        submit.pAudioData = static_cast<const BYTE*>(buffer->data);
        totalFrames = submit.AudioBytes / buffer->format.blockAlign;
    }
    if (totalFrames == 0) {
        return kDsOk;
    }
    if (buffer->playLength != 0) {
        submit.PlayBegin = std::min(buffer->format.frameOf(buffer->playStart), totalFrames - 1);
        submit.PlayLength = std::min(buffer->format.frameOf(buffer->playLength), totalFrames - submit.PlayBegin);
    }
    if (looping) {
        submit.LoopCount = XAUDIO2_LOOP_INFINITE;
        if (buffer->loopLength != 0) {
            const UINT32 playEnd = submit.PlayLength != 0 ? submit.PlayBegin + submit.PlayLength : totalFrames;
            submit.LoopBegin = std::min(buffer->format.frameOf(buffer->loopStart), playEnd - 1);
            submit.LoopLength = std::min(buffer->format.frameOf(buffer->loopLength), playEnd - submit.LoopBegin);
        } else {
            submit.LoopBegin = submit.PlayBegin;
            submit.LoopLength = submit.PlayLength;
        }
    }
    submit.Flags = XAUDIO2_END_OF_STREAM;
    stopVoice(buffer->voice);
    if (SUCCEEDED(buffer->voice->SubmitSourceBuffer(&submit))) {
        applyBufferMix(buffer);
        buffer->voice->Start(0);
    }
    return kDsOk;
}

HRESULT __stdcall xBufferStopEx(Buffer* buffer, LONGLONG timestamp, DWORD flags) {
    std::lock_guard lock(g_audioLock);
    buffer->playing = false;
    stopVoice(buffer->voice);
    return kDsOk;
}

HRESULT __stdcall xBufferGetStatus(Buffer* buffer, DWORD* status) {
    std::lock_guard lock(g_audioLock);
    if (buffer->playing) {
        if (buffer->voice != nullptr) {
            XAUDIO2_VOICE_STATE state{};
            buffer->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                buffer->playing = false;
            }
        } else if (!buffer->looping && Clock::now() >= buffer->stopTime) {
            buffer->playing = false;
        }
    }
    *status = buffer->playing ? (kStatusPlaying | (buffer->looping ? kStatusLooping : 0)) : 0;
    return kDsOk;
}

HRESULT __stdcall xBufferSetVolume(Buffer* buffer, LONG volume) {
    std::lock_guard lock(g_audioLock);
    buffer->volume = volume;
    applyBufferMix(buffer);
    return kDsOk;
}

HRESULT __stdcall xBufferSetHeadroom(Buffer* buffer, DWORD headroom) {
    std::lock_guard lock(g_audioLock);
    buffer->headroom = std::min<DWORD>(headroom, 10000);
    applyBufferMix(buffer);
    return kDsOk;
}

HRESULT __stdcall xStreamSetHeadroom(Stream* stream, DWORD headroom) {
    std::lock_guard lock(g_audioLock);
    stream->headroom = std::min<DWORD>(headroom, 10000);
    applyStreamMix(stream);
    return kDsOk;
}

HRESULT __stdcall xBufferSetFrequency(Buffer* buffer, DWORD frequency) {
    std::lock_guard lock(g_audioLock);
    buffer->frequency = frequency;
    applyBufferMix(buffer);
    return kDsOk;
}

HRESULT __stdcall xBufferSetLoopRegion(Buffer* buffer, DWORD start, DWORD length) {
    std::lock_guard lock(g_audioLock);
    buffer->loopStart = start;
    buffer->loopLength = length;
    return kDsOk;
}

HRESULT __stdcall xBufferSetPlayRegion(Buffer* buffer, DWORD start, DWORD length) {
    std::lock_guard lock(g_audioLock);
    buffer->playStart = start;
    buffer->playLength = length;
    return kDsOk;
}

HRESULT __stdcall xBufferSetPosition(Buffer* buffer, float x, float y, float z, DWORD apply) {
    std::lock_guard lock(g_audioLock);
    buffer->position = {x, y, z};
    applyBufferMix(buffer);
    return kDsOk;
}

HRESULT __stdcall xPosition(void*, float, float, float, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xStreamSetVolume(Stream* stream, LONG volume) {
    std::lock_guard lock(g_audioLock);
    stream->volume = volume;
    applyStreamMix(stream);
    return kDsOk;
}

HRESULT __stdcall xStreamSetFrequency(Stream* stream, DWORD frequency) {
    std::lock_guard lock(g_audioLock);
    stream->frequency = frequency;
    applyStreamMix(stream);
    return kDsOk;
}

HRESULT __stdcall xStreamPause(Stream* stream, DWORD pause) {
    std::lock_guard lock(g_audioLock);
    stream->paused = (pause & kStreamPause) != 0;
    if (stream->voice != nullptr) {
        if (stream->paused) {
            stream->voice->Stop(0);
        } else {
            stream->voice->Start(0);
        }
    }
    return kDsOk;
}

HRESULT __stdcall xStreamFlushEx(Stream* stream, LONGLONG timestamp, DWORD flags) {
    return streamFlush(stream);
}

} // namespace

void installAudioHooks() {
    auto hook = [](std::uint32_t address, const void* function, const char* name) { hookFunction(address, function, name); };
#define CW_HOOK(address, function) hook(address, reinterpret_cast<const void*>(&function), #function)
    CW_HOOK(0x002F6D1B, xDirectSoundCreate);
    CW_HOOK(0x002F6D62, xDirectSoundCreateStream);
    CW_HOOK(0x002F5927, xDirectSoundDoWork);
    CW_HOOK(0x002F49E1, xDirectSoundUseLightHRTF);
    CW_HOOK(0x002F4A09, xDirectSoundOverrideSpeakerConfig);
    CW_HOOK(0x002F49B5, xDirectSoundRelease);
    CW_HOOK(0x002F578B, xGetSpeakerConfig);
    CW_HOOK(0x002F57A7, xDownloadEffectsImage);
    CW_HOOK(0x002F57CE, xDirectSoundOk3);      // IDirectSound_SetMixBinHeadroom
    CW_HOOK(0x002F57EE, xDirectSoundOk1);      // IDirectSound_SynchPlayback
    CW_HOOK(0x002F6075, xDirectSoundOk1);      // IDirectSound_CommitDeferredSettings
    CW_HOOK(0x002F67F6, xSetListenerParameters); // IDirectSound_SetAllParameters
    CW_HOOK(0x002F6816, xDirectSoundOk3);      // IDirectSound_SetI3DL2Listener
    CW_HOOK(0x002F6B51, xCreateSoundBuffer);
    CW_HOOK(0x002F5538, xBufferRelease);
    CW_HOOK(0x002F6A95, xBufferSetBufferData);
    CW_HOOK(0x002F608D, xBufferSetFormat);
    CW_HOOK(0x002F5DD9, xBufferPlay);
    CW_HOOK(0x002F5896, xBufferStopEx);
    CW_HOOK(0x002F5DDE, xBufferGetStatus);
    CW_HOOK(0x002F5806, xBufferSetVolume);     // IDirectSoundBuffer_SetVolume
    CW_HOOK(0x002F5822, xBufferSetHeadroom);   // IDirectSoundBuffer_SetHeadroom
    CW_HOOK(0x002F583E, xDirectSoundOk2);      // IDirectSoundBuffer_SetMixBins
    CW_HOOK(0x002F58BA, xBufferSetLoopRegion);
    CW_HOOK(0x002F60A9, xBufferSetFrequency);
    CW_HOOK(0x002F612F, xBufferSetPlayRegion);
    CW_HOOK(0x002F60C5, xBufferSetPosition);   // IDirectSoundBuffer_SetPosition
    CW_HOOK(0x002F60FA, xPosition);            // IDirectSoundBuffer_SetVelocity
    CW_HOOK(0x002F58F6, xStreamSetVolume);     // IDirectSoundStream_SetVolume
    CW_HOOK(0x002F58FB, xStreamSetHeadroom);   // IDirectSoundStream_SetHeadroom
    CW_HOOK(0x002F5900, xDirectSoundOk2);      // IDirectSoundStream_SetMixBins
    CW_HOOK(0x002F5905, xDirectSoundOk2);      // IDirectSoundStream_SetMixBinVolumes_8
    CW_HOOK(0x002F590A, xStreamPause);         // IDirectSoundStream_Pause
    CW_HOOK(0x002F590F, xStreamFlushEx);
    CW_HOOK(0x002F614F, xStreamSetFrequency);  // IDirectSoundStream_SetFrequency
    CW_HOOK(0x002F6154, xPosition);            // IDirectSoundStream_SetPosition
    CW_HOOK(0x002F617D, xPosition);            // IDirectSoundStream_SetVelocity
#undef CW_HOOK
}

} // namespace cw::hle
