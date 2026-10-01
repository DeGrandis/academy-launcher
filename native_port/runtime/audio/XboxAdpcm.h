#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cw::audio {

constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatXboxAdpcm = 0x0069;

// Xbox ADPCM (IMA ADPCM, 36-byte blocks per channel): each block holds a 4-byte header (initial sample, step
// index) and 32 bytes of 4-bit codes, giving 64 samples per channel. Stereo blocks interleave the channels in
// 4-byte groups after both headers.
constexpr std::size_t kAdpcmBlockBytesPerChannel = 36;
constexpr std::size_t kAdpcmSamplesPerBlock = 64;

// Decodes whole blocks of `bytes` into interleaved 16-bit PCM.
std::vector<std::int16_t> decodeXboxAdpcm(const std::uint8_t* data, std::size_t bytes, unsigned channels);

// Number of sample frames that `bytes` of Xbox ADPCM decode to.
std::size_t xboxAdpcmFrames(std::size_t bytes, unsigned channels);

} // namespace cw::audio
