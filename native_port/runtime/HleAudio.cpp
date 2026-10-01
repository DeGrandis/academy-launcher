#include "Hle.h"

#include "Log.h"

#include <windows.h>
#include <mmreg.h>

#include <chrono>
#include <deque>
#include <mutex>

namespace cw::hle {

namespace {

using Clock = std::chrono::steady_clock;

constexpr HRESULT kDsOk = 0;
constexpr DWORD kStatusPlaying = 0x1;
constexpr DWORD kStatusLooping = 0x4;
constexpr DWORD kPlayLooping = 0x1;
constexpr DWORD kPacketSuccess = 0;
constexpr DWORD kPacketFlushed = 0x8000000B;

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

struct PendingPacket {
    XMediaPacket packet;
    Clock::time_point due;
};

struct SilentStream {
    const void* const* vtable;
    LONG references = 1;
    DWORD bytesPerSecond = 44100 * 4;
    StreamDesc desc{};
    std::deque<PendingPacket> pending;
    Clock::time_point nextDue{};
};

struct SilentBuffer {
    LONG references = 1;
    DWORD bufferBytes = 0;
    DWORD bytesPerSecond = 44100 * 4;
    bool playing = false;
    bool looping = false;
    Clock::time_point stopTime{};
};

std::recursive_mutex g_audioLock;
std::deque<SilentStream*> g_streams;
std::uint8_t g_directSound[0x100] = {};
std::uint8_t g_effectsImageDescription[0x1000] = {};

DWORD bytesPerSecondOf(const WAVEFORMATEX* format) {
    if (format == nullptr || format->nAvgBytesPerSec == 0) {
        return 44100 * 4;
    }
    return format->nAvgBytesPerSec;
}

void completePacket(SilentStream* stream, XMediaPacket& packet, DWORD status) {
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

// ---------------------------------------------------------------- XMediaObject vtable for streams

ULONG __stdcall streamAddRef(SilentStream* stream) {
    return static_cast<ULONG>(InterlockedIncrement(&stream->references));
}

ULONG __stdcall streamRelease(SilentStream* stream) {
    const LONG remaining = InterlockedDecrement(&stream->references);
    if (remaining == 0) {
        std::lock_guard lock(g_audioLock);
        std::erase(g_streams, stream);
        delete stream;
    }
    return static_cast<ULONG>(std::max<LONG>(remaining, 0));
}

HRESULT __stdcall streamGetInfo(SilentStream* stream, DWORD* info) {
    info[0] = 0;
    info[1] = 0;
    info[2] = 0;
    return kDsOk;
}

HRESULT __stdcall streamGetStatus(SilentStream* stream, DWORD* status) {
    std::lock_guard lock(g_audioLock);
    // XMO_STATUSF_ACCEPT_INPUT_DATA
    *status = stream->pending.size() < std::max<DWORD>(1, stream->desc.maxAttachedPackets) ? 1 : 0;
    return kDsOk;
}

HRESULT __stdcall streamProcess(SilentStream* stream, const XMediaPacket* input, const XMediaPacket* output) {
    if (input == nullptr) {
        return kDsOk;
    }
    std::lock_guard lock(g_audioLock);
    if (input->status != nullptr) {
        *input->status = 0x8000000A;
    }
    const auto now = Clock::now();
    if (stream->nextDue < now) {
        stream->nextDue = now;
    }
    stream->nextDue += std::chrono::microseconds(static_cast<long long>(input->maxSize) * 1000000 / stream->bytesPerSecond);
    stream->pending.push_back({*input, stream->nextDue});
    return kDsOk;
}

HRESULT __stdcall streamDiscontinuity(SilentStream* stream) {
    return kDsOk;
}

HRESULT __stdcall streamFlush(SilentStream* stream) {
    std::lock_guard lock(g_audioLock);
    while (!stream->pending.empty()) {
        PendingPacket packet = stream->pending.front();
        stream->pending.pop_front();
        completePacket(stream, packet.packet, kPacketFlushed);
    }
    stream->nextDue = {};
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

HRESULT __stdcall xDirectSoundCreate(void* guid, void** directSound, void* outer) {
    *directSound = g_directSound;
    logf("audio: DirectSoundCreate -> silent audio device");
    return kDsOk;
}

HRESULT __stdcall xDirectSoundCreateStream(StreamDesc* desc, SilentStream** stream) {
    auto* created = new SilentStream{kStreamVtable};
    created->desc = *desc;
    created->bytesPerSecond = bytesPerSecondOf(desc->format);
    {
        std::lock_guard lock(g_audioLock);
        g_streams.push_back(created);
    }
    *stream = created;
    return kDsOk;
}

void __stdcall xDirectSoundDoWork() {
    std::lock_guard lock(g_audioLock);
    const auto now = Clock::now();
    for (SilentStream* stream : g_streams) {
        while (!stream->pending.empty() && stream->pending.front().due <= now) {
            PendingPacket packet = stream->pending.front();
            stream->pending.pop_front();
            completePacket(stream, packet.packet, kPacketSuccess);
        }
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

HRESULT __stdcall xDirectSoundOk1(void*) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk2(void*, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk3(void*, DWORD, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk4(void*, DWORD, DWORD, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xDirectSoundOk5(void*, DWORD, DWORD, DWORD, DWORD) {
    return kDsOk;
}

HRESULT __stdcall xCreateSoundBuffer(void* directSound, const BufferDesc* desc, SilentBuffer** buffer, void* outer) {
    auto* created = new SilentBuffer;
    created->bufferBytes = desc != nullptr ? desc->bufferBytes : 0;
    created->bytesPerSecond = bytesPerSecondOf(desc != nullptr ? desc->format : nullptr);
    *buffer = created;
    return kDsOk;
}

ULONG __stdcall xBufferRelease(SilentBuffer* buffer) {
    const LONG remaining = InterlockedDecrement(&buffer->references);
    if (remaining == 0) {
        delete buffer;
    }
    return static_cast<ULONG>(std::max<LONG>(remaining, 0));
}

HRESULT __stdcall xBufferSetBufferData(SilentBuffer* buffer, void* data, DWORD bytes) {
    buffer->bufferBytes = bytes;
    return kDsOk;
}

HRESULT __stdcall xBufferSetFormat(SilentBuffer* buffer, const WAVEFORMATEX* format) {
    buffer->bytesPerSecond = bytesPerSecondOf(format);
    return kDsOk;
}

HRESULT __stdcall xBufferPlay(SilentBuffer* buffer, DWORD reserved1, DWORD reserved2, DWORD flags) {
    std::lock_guard lock(g_audioLock);
    buffer->playing = true;
    buffer->looping = (flags & kPlayLooping) != 0;
    buffer->stopTime = Clock::now() + std::chrono::microseconds(static_cast<long long>(buffer->bufferBytes) * 1000000 / buffer->bytesPerSecond);
    return kDsOk;
}

HRESULT __stdcall xBufferStopEx(SilentBuffer* buffer, LONGLONG timestamp, DWORD flags) {
    std::lock_guard lock(g_audioLock);
    buffer->playing = false;
    return kDsOk;
}

HRESULT __stdcall xBufferGetStatus(SilentBuffer* buffer, DWORD* status) {
    std::lock_guard lock(g_audioLock);
    if (buffer->playing && !buffer->looping && Clock::now() >= buffer->stopTime) {
        buffer->playing = false;
    }
    *status = buffer->playing ? (kStatusPlaying | (buffer->looping ? kStatusLooping : 0)) : 0;
    return kDsOk;
}

HRESULT __stdcall xStreamFlushEx(SilentStream* stream, LONGLONG timestamp, DWORD flags) {
    return streamFlush(stream);
}

HRESULT __stdcall xPosition(void*, float, float, float, DWORD) {
    return kDsOk;
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
    CW_HOOK(0x002F67F6, xDirectSoundOk3);      // IDirectSound_SetAllParameters
    CW_HOOK(0x002F6816, xDirectSoundOk3);      // IDirectSound_SetI3DL2Listener
    CW_HOOK(0x002F6B51, xCreateSoundBuffer);
    CW_HOOK(0x002F5538, xBufferRelease);
    CW_HOOK(0x002F6A95, xBufferSetBufferData);
    CW_HOOK(0x002F608D, xBufferSetFormat);
    CW_HOOK(0x002F5DD9, xBufferPlay);
    CW_HOOK(0x002F5896, xBufferStopEx);
    CW_HOOK(0x002F5DDE, xBufferGetStatus);
    CW_HOOK(0x002F5806, xDirectSoundOk2);      // IDirectSoundBuffer_SetVolume
    CW_HOOK(0x002F5822, xDirectSoundOk2);      // IDirectSoundBuffer_SetHeadroom
    CW_HOOK(0x002F583E, xDirectSoundOk2);      // IDirectSoundBuffer_SetMixBins
    CW_HOOK(0x002F58BA, xDirectSoundOk3);      // IDirectSoundBuffer_SetLoopRegion
    CW_HOOK(0x002F60A9, xDirectSoundOk2);      // IDirectSoundBuffer_SetFrequency
    CW_HOOK(0x002F612F, xDirectSoundOk3);      // IDirectSoundBuffer_SetPlayRegion
    CW_HOOK(0x002F60C5, xPosition);            // IDirectSoundBuffer_SetPosition
    CW_HOOK(0x002F60FA, xPosition);            // IDirectSoundBuffer_SetVelocity
    CW_HOOK(0x002F58F6, xDirectSoundOk2);      // IDirectSoundStream_SetVolume
    CW_HOOK(0x002F58FB, xDirectSoundOk2);      // IDirectSoundStream_SetHeadroom
    CW_HOOK(0x002F5900, xDirectSoundOk2);      // IDirectSoundStream_SetMixBins
    CW_HOOK(0x002F5905, xDirectSoundOk2);      // IDirectSoundStream_SetMixBinVolumes_8
    CW_HOOK(0x002F590A, xDirectSoundOk2);      // IDirectSoundStream_Pause
    CW_HOOK(0x002F590F, xStreamFlushEx);
    CW_HOOK(0x002F614F, xDirectSoundOk2);      // IDirectSoundStream_SetFrequency
    CW_HOOK(0x002F6154, xPosition);            // IDirectSoundStream_SetPosition
    CW_HOOK(0x002F617D, xPosition);            // IDirectSoundStream_SetVelocity
#undef CW_HOOK
}

} // namespace cw::hle
