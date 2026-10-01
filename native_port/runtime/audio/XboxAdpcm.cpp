#include "XboxAdpcm.h"

#include <algorithm>

namespace cw::audio {

namespace {

constexpr int kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
constexpr int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Decoder {
    int predictor = 0;
    int index = 0;

    std::int16_t decode(unsigned nibble) {
        const int step = kStepTable[index];
        int difference = step >> 3;
        if (nibble & 1) difference += step >> 2;
        if (nibble & 2) difference += step >> 1;
        if (nibble & 4) difference += step;
        if (nibble & 8) difference = -difference;
        predictor = std::clamp(predictor + difference, -32768, 32767);
        index = std::clamp(index + kIndexTable[nibble & 15], 0, 88);
        return static_cast<std::int16_t>(predictor);
    }
};

} // namespace

std::size_t xboxAdpcmFrames(std::size_t bytes, unsigned channels) {
    const std::size_t blockBytes = kAdpcmBlockBytesPerChannel * std::max(1u, channels);
    return bytes / blockBytes * kAdpcmSamplesPerBlock;
}

std::vector<std::int16_t> decodeXboxAdpcm(const std::uint8_t* data, std::size_t bytes, unsigned channels) {
    channels = std::clamp(channels, 1u, 8u);
    const std::size_t blockBytes = kAdpcmBlockBytesPerChannel * channels;
    const std::size_t blocks = bytes / blockBytes;
    std::vector<std::int16_t> out(blocks * kAdpcmSamplesPerBlock * channels);
    for (std::size_t block = 0; block < blocks; ++block) {
        const std::uint8_t* in = data + block * blockBytes;
        Decoder decoders[8];
        for (unsigned channel = 0; channel < channels; ++channel) {
            const std::uint8_t* header = in + channel * 4;
            decoders[channel].predictor = static_cast<std::int16_t>(header[0] | (header[1] << 8));
            decoders[channel].index = std::clamp<int>(header[2], 0, 88);
        }
        const std::uint8_t* codes = in + channels * 4;
        std::int16_t* frames = out.data() + block * kAdpcmSamplesPerBlock * channels;
        // Each channel contributes 4 bytes (8 samples) at a time.
        for (std::size_t group = 0; group < kAdpcmSamplesPerBlock / 8; ++group) {
            for (unsigned channel = 0; channel < channels; ++channel) {
                const std::uint8_t* bytesOfGroup = codes + (group * channels + channel) * 4;
                for (unsigned byte = 0; byte < 4; ++byte) {
                    const std::size_t sample = group * 8 + byte * 2;
                    frames[sample * channels + channel] = decoders[channel].decode(bytesOfGroup[byte] & 0xF);
                    frames[(sample + 1) * channels + channel] = decoders[channel].decode(bytesOfGroup[byte] >> 4);
                }
            }
        }
    }
    return out;
}

} // namespace cw::audio
