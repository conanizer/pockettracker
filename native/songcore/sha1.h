#ifndef POCKETTRACKER_SONGCORE_SHA1_H
#define POCKETTRACKER_SONGCORE_SHA1_H

// ─── SHA-1 — the trace header's project= id ──────────────────────────────────────────────────────
//
// The conformance trace identifies its project by the SHA-1 of the canonical serialized project JSON
// (event-schema §6): the exact bytes FileController.serializeProject produces, which are the exact
// bytes of a .ptp on disk (S2 proved the C++ round-trip is byte-for-byte). Kotlin's twin is
// EventTrace.projectSha1.
//
// Vendored rather than pulled from a platform crypto API for the usual songcore reason: the same
// bytes must hash the same on the device, on the host tools, and on Linux — and a trace whose header
// disagrees is a trace that can't be compared. It is a project id, not a security primitive.

#include <cstdint>
#include <cstdio>
#include <string>

namespace songcore {

inline std::string sha1_hex(const std::string& msg) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string data = msg;
    uint64_t ml = static_cast<uint64_t>(data.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56) data += static_cast<char>(0x00);
    for (int i = 7; i >= 0; --i) data += static_cast<char>((ml >> (i * 8)) & 0xFF);

    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(data[chunk + i * 4])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(data[chunk + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(data[chunk + i * 4 + 2])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(data[chunk + i * 4 + 3])));
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    char buf[41];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return buf;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SHA1_H
