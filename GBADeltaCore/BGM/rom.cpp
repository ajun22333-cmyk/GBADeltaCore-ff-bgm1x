#include "rom.h"

#include <cstdio>
#include <cstring>

#include "winutil.h"

bool Rom::load(const std::string& path) {
    FILE* f = fopenUtf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024 * 1024) {
        std::fclose(f);
        return false;
    }
    data.resize((size_t) n);
    size_t got = std::fread(data.data(), 1, (size_t) n, f);
    std::fclose(f);
    return got == (size_t) n;
}

bool Rom::loadFromMemory(const uint8_t* p, size_t n) {
    if (!p || n == 0 || n > 32 * 1024 * 1024) return false;
    data.assign(p, p + n);
    return true;
}

bool Rom::valid(uint32_t addr, uint32_t len) const {
    if (addr < 0x08000000u || addr >= 0x0A000000u) return false;
    uint64_t off = addr - 0x08000000u;
    return off + len <= data.size();
}

uint8_t Rom::u8(uint32_t addr) const {
    return valid(addr) ? data[addr - 0x08000000u] : 0;
}

uint16_t Rom::u16(uint32_t addr) const {
    if (!valid(addr, 2)) return 0;
    const uint8_t* p = &data[addr - 0x08000000u];
    return (uint16_t) (p[0] | (p[1] << 8));
}

uint32_t Rom::u32(uint32_t addr) const {
    if (!valid(addr, 4)) return 0;
    const uint8_t* p = &data[addr - 0x08000000u];
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

const int8_t* Rom::ptr(uint32_t addr) const {
    return valid(addr) ? reinterpret_cast<const int8_t*>(&data[addr - 0x08000000u]) : nullptr;
}

std::string Rom::gameCode() const {
    if (data.size() < 0xB0) return {};
    return std::string(reinterpret_cast<const char*>(&data[0xAC]), 4);
}

std::string Rom::gameTitle() const {
    if (data.size() < 0xB0) return {};
    std::string s(reinterpret_cast<const char*>(&data[0xA0]), 12);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

static bool isRamPtr(uint32_t p) {
    if (p & 3) return false;
    if (p >= 0x02000000u && p < 0x02040000u) return true;  // EWRAM
    if (p >= 0x03000000u && p < 0x03008000u) return true;  // IWRAM
    return false;
}

static bool readEntry(const Rom& rom, uint32_t addr, MPlayEntry& e) {
    if (!rom.valid(addr, 12)) return false;
    e.info = rom.u32(addr);
    e.tracks = rom.u32(addr + 4);
    e.numTracks = rom.u8(addr + 8);
    if (!isRamPtr(e.info) || !isRamPtr(e.tracks)) return false;
    if (e.info == e.tracks) return false;
    if (e.numTracks < 1 || e.numTracks > 16) return false;  // MAX_MUSICPLAYER_TRACKS
    if (rom.u8(addr + 9) != 0) return false;                 // 구조체 패딩
    return true;
}

std::vector<MPlayTableCandidate> findMPlayTables(const Rom& rom, size_t maxResults) {
    std::vector<MPlayTableCandidate> out;
    if (rom.data.size() < 12) return out;
    uint32_t end = 0x08000000u + (uint32_t) rom.data.size() - 12;
    for (uint32_t a = 0x08000000u; a <= end; a += 4) {
        MPlayEntry e;
        if (!readEntry(rom, a, e)) continue;
        // 바로 앞이 같은 구조면 이미 이전 시작점에서 처리됨
        MPlayEntry prev;
        if (a >= 0x0800000Cu && readEntry(rom, a - 12, prev)) continue;

        MPlayTableCandidate c;
        c.romAddr = a;
        uint32_t p = a;
        while (readEntry(rom, p, e) && c.entries.size() < 32) {
            bool dup = false;
            for (auto& x : c.entries) {
                if (x.info == e.info || x.tracks == e.tracks) dup = true;
            }
            if (dup) break;
            c.entries.push_back(e);
            p += 12;
        }
        if (c.entries.size() >= 2) {
            out.push_back(std::move(c));
            if (out.size() >= maxResults) break;
        }
    }
    return out;
}
