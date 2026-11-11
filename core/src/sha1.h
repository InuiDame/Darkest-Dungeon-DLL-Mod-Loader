#pragma once
// SHA-1，只用于识别 exe 版本（日志和 game_info），不承担安全用途。

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ddc {

class Sha1
{
public:
    void Update(const std::uint8_t* p, std::size_t n)
    {
        total_ += n;
        while (n) {
            if (fill_ == 0 && n >= 64) {
                Block(p);
                p += 64;
                n -= 64;
                continue;
            }
            const std::size_t take = (std::min)(n, 64 - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == 64) {
                Block(buf_);
                fill_ = 0;
            }
        }
    }

    std::string Final()
    {
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t pad80 = 0x80;
        const std::uint8_t zero = 0;
        Update(&pad80, 1);
        while (fill_ != 56) {
            Update(&zero, 1);
        }
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        }
        Update(len, 8);
        char out[41] = {};
        for (int i = 0; i < 20; ++i) {
            std::snprintf(out + i * 2, 3, "%02x", static_cast<unsigned>(h_[i / 4] >> (24 - (i % 4) * 8)) & 0xFF);
        }
        return out;
    }

private:
    static std::uint32_t Rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const std::uint8_t* p)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::uint32_t h_[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::uint64_t total_ = 0;
    std::uint8_t buf_[64] = {};
    std::size_t fill_ = 0;
};

// 计算文件的 SHA1，失败返回空串
inline std::string FileSha1(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<std::uint8_t> buffer(1u << 20);
    Sha1 sha;
    DWORD got = 0;
    while (ReadFile(f, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got) {
        sha.Update(buffer.data(), got);
    }
    CloseHandle(f);
    return sha.Final();
}

}  // namespace ddc
