// MP2K(Sappy) 시퀀스를 ROM 에서 직접 읽어 1배속으로 재생하는 소프트웨어 재생기.
// 동작은 pret/pokeemerald 의 m4a.c / m4a_1.s 디컴파일 소스를 기준으로 옮겼다.
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "rom.h"

struct EngineConfig {
    int outRate = 48000;
    int samplesPerVBlank = 224;  // SoundInfo.pcmSamplesPerVBlank (13379Hz 기준 224)
    int dmaPeriod = 7;           // SoundInfo.pcmDmaPeriod
    int maxChans = 12;           // SoundInfo.maxChans
    int masterVolume = 15;       // SoundInfo.masterVolume
    float gain = 1.0f;           // 최종 출력 배율
};

class Mp2kEngine {
public:
    explicit Mp2kEngine(const Rom* rom);

    void configure(const EngineConfig& cfg);
    // 실행 중인 게임의 SoundInfo 값으로 갱신 (스레드 안전)
    void setRuntimeParams(int maxChans, int masterVolume, int reverb, int samplesPerVBlank, int dmaPeriod);
    void setGain(float g);

    bool start(uint32_t songHeader);  // MPlayStart 와 같은 초기화
    void stop();                      // 재생 중지 (곡 정보도 해제)
    void setPaused(bool paused);      // m4aMPlayStop / MPlayContinue 와 같은 효과
    bool paused() const;
    uint32_t currentSong() const;
    bool finished() const;            // 모든 트랙이 FINE 으로 끝남

    // outRate 기준 스테레오 인터리브 float 출력
    void render(float* out, int frames);

private:
    struct Tone {
        uint8_t type = 0, key = 0, length = 0, panSweep = 0;
        uint32_t wav = 0;
        uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
        uint32_t word8() const { return attack | (decay << 8) | (sustain << 16) | ((uint32_t) release << 24); }
    };

    struct Track {
        uint8_t flags = 0;
        uint8_t wait = 0, patternLevel = 0, repN = 0, gateTime = 0, key = 0, velocity = 0, runningStatus = 0;
        int8_t keyM = 0;
        uint8_t pitM = 0;
        int8_t keyShift = 0, keyShiftX = 0, tune = 0;
        uint8_t pitX = 0;
        int8_t bend = 0;
        uint8_t bendRange = 0, volMR = 0, volML = 0, vol = 0, volX = 0;
        int8_t pan = 0, panX = 0, modM = 0;
        uint8_t mod = 0, modT = 0, lfoSpeed = 0, lfoSpeedC = 0, lfoDelay = 0, lfoDelayC = 0, priority = 0;
        uint8_t echoVol = 0, echoLen = 0;
        Tone tone;
        uint16_t timer = 0;
        uint32_t unk3C = 0;
        uint32_t cmdPtr = 0;
        uint32_t patternStack[3] = {0, 0, 0};
    };

    struct DsChan {
        uint8_t status = 0, type = 0, rightVol = 0, leftVol = 0;
        uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
        uint8_t key = 0, env = 0, envR = 0, envL = 0, echoVol = 0, echoLen = 0;
        uint8_t gateTime = 0, midiKey = 0, velocity = 0, priority = 0;
        int8_t rhythmPan = 0;
        uint32_t count = 0;
        uint32_t frequency = 0;
        uint32_t wav = 0;
        int track = -1;
        uint64_t linkSeq = 0;
        // 재생 위치
        const std::vector<int8_t>* samples = nullptr;
        double pos = 0;
        uint32_t loopStart = 0;
        bool reverse = false;
    };

    struct CgbChan {
        uint8_t status = 0, type = 0, rightVol = 0, leftVol = 0;
        uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
        uint8_t key = 0, envVol = 0, envGoal = 0, envCounter = 0, echoVol = 0, echoLen = 0;
        uint8_t gateTime = 0, midiKey = 0, velocity = 0, priority = 0;
        int8_t rhythmPan = 0;
        uint8_t sustainGoal = 0, n4 = 0, pan = 0, panMask = 0, modify = 0, length = 0, sweep = 0;
        uint32_t frequency = 0;
        uint32_t wavePointer = 0;
        uint32_t currentPointer = 0;
        int track = -1;
        uint64_t linkSeq = 0;
        // 하드웨어 흉내
        bool hwOn = false;
        uint8_t hwVol = 0;          // 0~15 (채널3은 출력 배율로 환산)
        float hwCh3Scale = 0;
        uint8_t hwPan = 0;          // NR51 비트 (하위 4비트=오른쪽, 상위=왼쪽)
        uint32_t hwFreq = 0;
        uint8_t hwNoiseReg = 0;
        double phase = 0;
        double lengthLeft = -1;     // 초. 음수면 길이 제한 없음
        double sweepTimer = 0;
        uint16_t lfsr = 0x7FFF;
        double noiseAcc = 0;
        uint8_t wave[32] = {0};
    };

    void vblank();
    void mplayMain();
    void trackTick(int ti);
    void execCommand(int ti, uint8_t cmd);
    void plyNote(int ti, uint8_t n);
    void plyFine(int ti);
    void plyGoto(Track& t);
    void plyEndTie(int ti);
    void plyMemAcc(int ti);
    void plyXcmd(int ti);
    void trackStop(int ti);
    void trkVolPitSet(Track& t);
    void clearModM(Track& t);
    void chnVolSet(uint8_t& right, uint8_t& left, uint8_t vel, int8_t rpan, const Track& t);
    uint32_t midiKeyToFreq(uint32_t wav, uint8_t key, uint8_t fine) const;
    uint32_t midiKeyToCgbFreq(uint8_t chanNum, uint8_t key, uint8_t fine) const;
    void cgbSound();
    void cgbModVol(CgbChan& c);
    void cgbOscOff(int ch);
    void cgbTrigger(int ch);
    void mixDirectSound();
    const std::vector<int8_t>* sampleData(uint32_t wav, bool reverse);
    void readTone(uint32_t addr, Tone& t) const;
    void stopAllChannels();

    const Rom* rom_;
    EngineConfig cfg_;
    mutable std::mutex mtx_;

    uint32_t song_ = 0;
    bool paused_ = false;
    bool finished_ = true;
    uint32_t tone_ = 0;       // 음색 그룹 주소
    uint8_t priority_ = 0;
    uint16_t tempoD_ = 150, tempoU_ = 0x100, tempoI_ = 150, tempoC_ = 0;
    uint8_t memAcc_[16] = {0};
    int trackCount_ = 0;
    std::vector<Track> tracks_;
    std::vector<DsChan> ds_;
    CgbChan cgb_[4];
    uint64_t linkCounter_ = 0;
    uint8_t c15_ = 14;
    int reverb_ = 0;

    // DirectSound 링 버퍼 (리버브용) — 원본 pcmBuffer 와 같은 구조
    std::vector<float> ringR_, ringL_;
    int ringSlot_ = 0;
    std::vector<float> chunkR_, chunkL_;
    float lastDsR_ = 0, lastDsL_ = 0;

    // 출력 타이밍
    double outPerFrame_ = 0;
    double outAcc_ = 0;
    int frameOutTotal_ = 0;
    int frameOutPos_ = 0;
    float hpL_ = 0, hpR_ = 0, hpInL_ = 0, hpInR_ = 0;

    std::unordered_map<uint64_t, std::vector<int8_t>> sampleCache_;
};
