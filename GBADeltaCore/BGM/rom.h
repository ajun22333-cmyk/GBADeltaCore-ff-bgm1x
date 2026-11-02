// ROM 이미지 접근과 MP2K 플레이어 테이블(gMPlayTable) 후보 탐색.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Rom {
    std::vector<uint8_t> data;

    bool load(const std::string& path);
    bool loadFromMemory(const uint8_t* p, size_t n);

    // GBA 버스 주소(0x08000000~) -> ROM 오프셋. 범위 밖이면 false.
    bool valid(uint32_t addr, uint32_t len = 1) const;
    uint8_t u8(uint32_t addr) const;
    uint16_t u16(uint32_t addr) const;
    uint32_t u32(uint32_t addr) const;
    const int8_t* ptr(uint32_t addr) const;

    std::string gameCode() const;   // 헤더 0xAC, 4바이트
    std::string gameTitle() const;  // 헤더 0xA0, 12바이트
};

// gMPlayTable 의 한 항목 { MusicPlayerInfo*, MusicPlayerTrack*, u8 numTracks, u8 pad, u16 unk }
struct MPlayEntry {
    uint32_t info;
    uint32_t tracks;
    uint8_t numTracks;
};

struct MPlayTableCandidate {
    uint32_t romAddr;               // 테이블 시작 주소 (0x08xxxxxx)
    std::vector<MPlayEntry> entries;
};

// ROM 전체를 훑어 gMPlayTable 구조와 일치하는 연속 항목들을 찾는다.
// 실제 사용 여부는 실행 중 RAM 의 MusicPlayerInfo.ident 로 확인해야 한다.
std::vector<MPlayTableCandidate> findMPlayTables(const Rom& rom, size_t maxResults = 64);
