#include "Kernel.h"

#include "Log.h"

#include <cstring>

namespace cw::kernel {

namespace {

// Lives inside the caller's Xbox A_SHA_CTX buffer (116 bytes).
struct Sha1Context {
    std::uint32_t state[5];
    std::uint64_t length;
    std::uint8_t buffer[64];
    std::uint32_t used;
};
static_assert(sizeof(Sha1Context) <= 116);

std::uint32_t rotateLeft(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void sha1Block(Sha1Context& context, const std::uint8_t* block) {
    std::uint32_t w[80];
    for (int index = 0; index < 16; ++index) {
        w[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) | (block[index * 4 + 1] << 16) | (block[index * 4 + 2] << 8) | block[index * 4 + 3];
    }
    for (int index = 16; index < 80; ++index) {
        w[index] = rotateLeft(w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16], 1);
    }
    std::uint32_t a = context.state[0], b = context.state[1], c = context.state[2], d = context.state[3], e = context.state[4];
    for (int index = 0; index < 80; ++index) {
        std::uint32_t f;
        std::uint32_t k;
        if (index < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (index < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (index < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t temp = rotateLeft(a, 5) + f + e + k + w[index];
        e = d;
        d = c;
        c = rotateLeft(b, 30);
        b = a;
        a = temp;
    }
    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
}

void __stdcall xXcSHAInit(Sha1Context* context) {
    *context = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {}, 0};
}

void __stdcall xXcSHAUpdate(Sha1Context* context, const std::uint8_t* input, ULONG length) {
    context->length += length;
    while (length > 0) {
        const std::uint32_t take = std::min<std::uint32_t>(64 - context->used, length);
        std::memcpy(context->buffer + context->used, input, take);
        context->used += take;
        input += take;
        length -= take;
        if (context->used == 64) {
            sha1Block(*context, context->buffer);
            context->used = 0;
        }
    }
}

void __stdcall xXcSHAFinal(Sha1Context* context, std::uint8_t* digest) {
    const std::uint64_t bitLength = context->length * 8;
    const std::uint8_t pad = 0x80;
    xXcSHAUpdate(context, &pad, 1);
    const std::uint8_t zero = 0;
    while (context->used != 56) {
        xXcSHAUpdate(context, &zero, 1);
    }
    std::uint8_t lengthBytes[8];
    for (int index = 0; index < 8; ++index) {
        lengthBytes[index] = static_cast<std::uint8_t>(bitLength >> (56 - index * 8));
    }
    xXcSHAUpdate(context, lengthBytes, 8);
    for (int index = 0; index < 20; ++index) {
        digest[index] = static_cast<std::uint8_t>(context->state[index / 4] >> (24 - (index % 4) * 8));
    }
}

void __stdcall xXcHMAC(const std::uint8_t* key, ULONG keyLength, const std::uint8_t* data1, ULONG data1Length,
    const std::uint8_t* data2, ULONG data2Length, std::uint8_t* digest) {
    std::uint8_t keyBlock[64] = {};
    Sha1Context context;
    if (keyLength > 64) {
        xXcSHAInit(&context);
        xXcSHAUpdate(&context, key, keyLength);
        xXcSHAFinal(&context, keyBlock);
    } else {
        std::memcpy(keyBlock, key, keyLength);
    }

    std::uint8_t pad[64];
    for (int index = 0; index < 64; ++index) {
        pad[index] = keyBlock[index] ^ 0x36;
    }
    std::uint8_t inner[20];
    xXcSHAInit(&context);
    xXcSHAUpdate(&context, pad, 64);
    if (data1 != nullptr) {
        xXcSHAUpdate(&context, data1, data1Length);
    }
    if (data2 != nullptr) {
        xXcSHAUpdate(&context, data2, data2Length);
    }
    xXcSHAFinal(&context, inner);

    for (int index = 0; index < 64; ++index) {
        pad[index] = keyBlock[index] ^ 0x5C;
    }
    xXcSHAInit(&context);
    xXcSHAUpdate(&context, pad, 64);
    xXcSHAUpdate(&context, inner, 20);
    xXcSHAFinal(&context, digest);
}

struct Rc4State {
    std::uint8_t s[256];
    std::uint8_t i;
    std::uint8_t j;
};

void __stdcall xXcRC4Key(Rc4State* state, ULONG keyLength, const std::uint8_t* key) {
    for (int index = 0; index < 256; ++index) {
        state->s[index] = static_cast<std::uint8_t>(index);
    }
    std::uint8_t j = 0;
    for (int index = 0; index < 256; ++index) {
        j = static_cast<std::uint8_t>(j + state->s[index] + key[index % keyLength]);
        std::swap(state->s[index], state->s[j]);
    }
    state->i = 0;
    state->j = 0;
}

void __stdcall xXcRC4Crypt(Rc4State* state, ULONG length, std::uint8_t* data) {
    for (ULONG index = 0; index < length; ++index) {
        state->i = static_cast<std::uint8_t>(state->i + 1);
        state->j = static_cast<std::uint8_t>(state->j + state->s[state->i]);
        std::swap(state->s[state->i], state->s[state->j]);
        data[index] ^= state->s[static_cast<std::uint8_t>(state->s[state->i] + state->s[state->j])];
    }
}

} // namespace

void registerCryptoExports() {
    registerExport(335, reinterpret_cast<void*>(&xXcSHAInit));
    registerExport(336, reinterpret_cast<void*>(&xXcSHAUpdate));
    registerExport(337, reinterpret_cast<void*>(&xXcSHAFinal));
    registerExport(338, reinterpret_cast<void*>(&xXcRC4Key));
    registerExport(339, reinterpret_cast<void*>(&xXcRC4Crypt));
    registerExport(340, reinterpret_cast<void*>(&xXcHMAC));
}

} // namespace cw::kernel
