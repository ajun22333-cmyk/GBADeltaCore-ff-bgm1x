// 게임의 BGM 플레이어(MusicPlayerInfo) 상태를 외부 재생기에 반영하고, 원본 BGM 을 무음 처리한다.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "mp2k_engine.h"
#include "rom.h"

constexpr uint32_t kMp2kIdent = 0x68736D53;  // 'Smsh'
constexpr uint32_t kSoundInfoPtr = 0x03007FF0;

// 에뮬레이터 메모리 접근 (버스 주소)
struct GbaBus {
    virtual ~GbaBus() = default;
    virtual uint8_t r8(uint32_t a) = 0;
    virtual uint16_t r16(uint32_t a) { return (uint16_t) (r8(a) | (r8(a + 1) << 8)); }
    virtual uint32_t r32(uint32_t a) { return r16(a) | ((uint32_t) r16(a + 2) << 16); }
    virtual void w8(uint32_t a, uint8_t v) = 0;
};

struct BgmSnapshot {
    uint32_t header = 0;
    uint32_t status = 0;
    uint32_t clock = 0;
    uint16_t fadeOI = 0;
};

struct SoundInfoSnapshot {
    bool valid = false;
    int reverb = 0, maxChans = 0, masterVolume = 0, dmaPeriod = 0, samplesPerVBlank = 0;
};

// gMPlayTable 후보 중 실행 중 RAM 과 일치하는 첫 항목(0번 = BGM)의 MusicPlayerInfo 주소. 없으면 0.
uint32_t findBgmPlayerInfo(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands);
bool isPlayerInfoValid(GbaBus& bus, uint32_t info);

BgmSnapshot readBgm(GbaBus& bus, uint32_t info);
SoundInfoSnapshot readSoundInfo(GbaBus& bus);

// 원본 BGM 트랙의 vol/volX 를 0 으로 만들고 볼륨 재계산 플래그를 세운다.
void muteBgmTracks(GbaBus& bus, uint32_t info);
// 게임이 재생 중인 곡을 처음부터 다시 시작하게 한다 (무음 처리를 풀 때: 트랙 볼륨을 곡이 다시 정한다)
void restartGameSong(GbaBus& bus, uint32_t info);

// 곡 데이터를 읽어, 앞으로 되돌아가는 GOTO(반복 구간)가 있으면 true. 배경음악은 반복하고 효과음은 반복하지 않는다.
bool isLoopingSong(const Rom& rom, uint32_t header);

// 음악용 플레이어 고르기. 게임마다 BGM 플레이어 번호가 다르므로(포켓몬 0번, 젤다 4개의 검 1번 등)
// 트랙 수가 큰 플레이어들을 음악 후보로 보고, 가장 최근에 곡을 시작해 재생 중인 플레이어를 따라간다.
class BgmPlayerSelector {
public:
    // 실행 중 RAM 과 맞는 테이블을 찾으면 true. 후보가 정해진다.
    bool locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands);
    bool located() const { return !m_players.empty(); }
    bool stillValid(GbaBus& bus) const;
    void reset();

    // 프레임마다 호출. 따라갈 플레이어(없으면 0)와 그 상태를 돌려준다. 플레이어가 바뀌면 changed=true.
    // 다른 플레이어로 옮겨 가는 것은 그 플레이어가 반복하는 곡(배경음악)을 새로 시작했을 때뿐이다.
    uint32_t update(GbaBus& bus, const Rom& rom, BgmSnapshot& snap, bool& changed);
    const std::vector<uint32_t>& players() const { return m_players; }

private:
    struct State {
        uint32_t header = 0xFFFFFFFF;
        uint32_t clock = 0;
        uint64_t startedAt = 0;
    };
    std::vector<uint32_t> m_players;
    std::vector<State> m_state;
    uint32_t m_active = 0;
    uint64_t m_frame = 0;
    std::unordered_map<uint32_t, bool> m_loopCache;
};

// 프레임마다 받은 상태로 재생기를 시작/일시정지/재개한다.
class BgmMirror {
public:
    explicit BgmMirror(Mp2kEngine& eng) : eng_(eng) {}
    void update(const BgmSnapshot& s);
    void reset();

private:
    Mp2kEngine& eng_;
    bool have_ = false;
    BgmSnapshot last_;
};

// 곡의 대략적인 길이(초). 반복 곡은 첫 반복 지점까지. 트랙 중 가장 긴 것 기준
double songSeconds(const Rom& rom, uint32_t header);

// 곡이 음악인지: 반복 구간이 있거나(배경음악) 트랙이 4개 이상(팡파르 등)이면 음악으로 본다.
// 효과음은 대부분 트랙 1~3개에 반복이 없다 (파이어레드 곡 목록 기준).
bool isMusicSong(const Rom& rom, uint32_t header);

// 트랙이 많은 플레이어마다 대체 재생기를 하나씩 둔다. 각 플레이어가 음악을 재생하는 동안만
// 그 플레이어를 무음 처리하고 대체 재생하며, 효과음은 원본 그대로 둔다.
class BgmMultiMirror {
public:
    explicit BgmMultiMirror(const Rom* rom) : rom_(rom) {}

    bool locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands);  // 에뮬레이션 스레드
    bool located() const;
    bool stillValid(GbaBus& bus) const;
    void reset();                                                            // 재생기 모두 제거
    void stopAll();
    // 에뮬레이션 스레드: 대체 재생을 멈추고, 무음 처리하던 게임 곡을 처음부터 다시 시작시킨다
    void releaseMute(GbaBus& bus);

    void configure(const EngineConfig& cfg);  // 출력 레이트 등 (곡은 다시 시작)
    void setRuntimeParams(const SoundInfoSnapshot& si);

    // 에뮬레이션 스레드: 프레임마다. mute=false 면 무음 처리 없이 상태만 따라간다 (녹화용 두 번째 인스턴스)
    void onFrame(GbaBus& bus, bool mute);
    // 출력 스레드: 모든 재생기를 섞어 out 에 쓴다
    void render(float* out, int frames);

    // 연출 곡(반복 없는 긴 곡, 대체하지 않음)을 게임이 재생 중인지
    bool cutsceneActive() const { return cutscene_; }

    // "효과음 1배속": 트랙이 적은 플레이어(효과음용)가 내는 반복 없는 소리도 1배속으로 끝까지 낸다
    // (다음 소리부터 적용). 곡 머리가 롬에 없는 소리(포켓몬 울음소리 등)는 대상이 아니다
    void setSfx1x(bool on) { sfx1x_ = on; }
    // 1배속으로 내는 효과음의 볼륨 (0~1)
    void setSfxVolume(float gain) { sfxGain_ = gain; }

    // 곡 종류
    enum Kind { Kind_Sound = 0, Kind_Music, Kind_Jingle, Kind_Cutscene };

private:
    struct Player {
        uint32_t info = 0;
        std::unique_ptr<Mp2kEngine> eng;
        std::unique_ptr<BgmMirror> mirror;
        uint32_t header = 0xFFFFFFFF;
        bool music = false;
        int kind = Kind_Sound;
        int engKind = Kind_Sound;  // 재생기가 지금 내고 있는 곡의 종류
        bool gameDone = false;     // 게임에서는 팡파레가 이미 끝남
        bool orphan = false;       // 게임은 이 플레이어로 다른 소리를 내지만, 재생기는 팡파레를 끝까지 냄
        bool se = false;           // 효과음용 플레이어 (트랙이 적음)
        bool seOn = false;         // 지금 게임이 내는 효과음을 대체 중
        uint32_t seHeader = 0;     // 효과음: 지난 프레임에 본 곡 머리와 clock (새로 냈는지 가린다)
        uint32_t seClock = 0;
    };
    int classify(uint32_t header);
    bool isSfxSong(uint32_t header);
    void onSeFrame(GbaBus& bus, Player& p, bool mute);
    void seStep(GbaBus& bus, Player& p, const BgmSnapshot& s, bool mute);
    bool jingleLagging(const Player& p) const;
    bool anyJingleLagging(const Player* except) const;
    void cutLaggingJingles(const Player* except);
    const Rom* rom_;
    EngineConfig cfg_;
    mutable std::mutex mtx_;
    std::vector<Player> players_;
    std::atomic<bool> sfx1x_{true};
    std::atomic<float> sfxGain_{1.0f};
    std::vector<float> tmp_;
    std::unordered_map<uint32_t, bool> musicCache_;
    std::unordered_map<uint32_t, int> kindCache_;
    std::atomic<bool> cutscene_{false};
};
