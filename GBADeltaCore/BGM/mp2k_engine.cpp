#include "mp2k_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kVBlankRate = 16777216.0 / 280896.0;  // 59.7275 Hz

// SoundChannel.statusFlags
constexpr uint8_t SF_START = 0x80;
constexpr uint8_t SF_STOP = 0x40;
constexpr uint8_t SF_LOOP = 0x10;
constexpr uint8_t SF_IEC = 0x04;
constexpr uint8_t SF_ENV = 0x03;
constexpr uint8_t SF_ENV_ATTACK = 0x03;
constexpr uint8_t SF_ENV_DECAY = 0x02;
constexpr uint8_t SF_ENV_SUSTAIN = 0x01;
constexpr uint8_t SF_ENV_RELEASE = 0x00;
constexpr uint8_t SF_ON = SF_START | SF_STOP | SF_IEC | SF_ENV;

// MusicPlayerTrack.flags
constexpr uint8_t MPT_VOLSET = 0x01;
constexpr uint8_t MPT_VOLCHG = 0x03;
constexpr uint8_t MPT_PITSET = 0x04;
constexpr uint8_t MPT_PITCHG = 0x0C;
constexpr uint8_t MPT_START = 0x40;
constexpr uint8_t MPT_EXIST = 0x80;

// ToneData.type
constexpr uint8_t TT_CGB = 0x07;
constexpr uint8_t TT_FIX = 0x08;
constexpr uint8_t TT_REV = 0x10;
constexpr uint8_t TT_CMP = 0x20;
constexpr uint8_t TT_SPL = 0x40;
constexpr uint8_t TT_RHY = 0x80;

constexpr uint8_t MO_PIT = 0x02;
constexpr uint8_t MO_VOL = 0x01;
constexpr uint8_t ENV_DIR_INC = 0x08;
constexpr uint8_t ENV_DIR_DEC = 0x00;

const uint8_t kClockTable[49] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x1C, 0x1E, 0x20, 0x24, 0x28, 0x2A, 0x2C,
    0x30, 0x34, 0x36, 0x38, 0x3C, 0x40, 0x42, 0x44, 0x48, 0x4C, 0x4E, 0x50, 0x54, 0x58, 0x5A, 0x5C,
    0x60,
};

const uint32_t kFreqTable[12] = {
    2147483648u, 2275179671u, 2410468894u, 2553802834u, 2705659852u, 2866546760u,
    3037000500u, 3217589947u, 3408917802u, 3611622603u, 3826380858u, 4053909305u,
};

const int16_t kCgbFreqTable[12] = {
    -2004, -1891, -1785, -1685, -1591, -1501, -1417, -1337, -1262, -1192, -1125, -1062,
};

const uint8_t kNoiseTable[60] = {
    0xD7, 0xD6, 0xD5, 0xD4, 0xC7, 0xC6, 0xC5, 0xC4, 0xB7, 0xB6, 0xB5, 0xB4,
    0xA7, 0xA6, 0xA5, 0xA4, 0x97, 0x96, 0x95, 0x94, 0x87, 0x86, 0x85, 0x84,
    0x77, 0x76, 0x75, 0x74, 0x67, 0x66, 0x65, 0x64, 0x57, 0x56, 0x55, 0x54,
    0x47, 0x46, 0x45, 0x44, 0x37, 0x36, 0x35, 0x34, 0x27, 0x26, 0x25, 0x24,
    0x17, 0x16, 0x15, 0x14, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
};

const uint8_t kCgb3Vol[16] = {
    0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40, 0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20,
};

const int8_t kDeltaTable[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};

uint8_t scaleTable(int key) {  // gScaleTable
    return (uint8_t) (((14 - key / 12) << 4) | (key % 12));
}

uint8_t cgbScaleTable(int key) {  // gCgbScaleTable
    return (uint8_t) (((key / 12) << 4) | (key % 12));
}

uint32_t umul3232H32(uint32_t a, uint32_t b) {
    return (uint32_t) (((uint64_t) a * b) >> 32);
}

float cgb3Scale(uint8_t reg) {  // NR32 값 -> 출력 배율
    if (reg & 0x80) return 0.75f;
    switch ((reg >> 5) & 3) {
    case 1: return 1.0f;
    case 2: return 0.5f;
    case 3: return 0.25f;
    default: return 0.0f;
    }
}

}  // namespace

Mp2kEngine::Mp2kEngine(const Rom* rom) : rom_(rom) {
    for (int i = 0; i < 4; ++i) {
        cgb_[i].type = (uint8_t) (i + 1);
        cgb_[i].panMask = (uint8_t) (0x11 << i);
    }
    configure(cfg_);
}

void Mp2kEngine::configure(const EngineConfig& cfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    cfg_ = cfg;
    cfg_.samplesPerVBlank = std::clamp(cfg_.samplesPerVBlank, 16, 1024);
    cfg_.dmaPeriod = std::clamp(cfg_.dmaPeriod, 2, 16);
    cfg_.maxChans = std::clamp(cfg_.maxChans, 1, 12);
    ds_.assign(12, DsChan());
    ringR_.assign((size_t) cfg_.samplesPerVBlank * cfg_.dmaPeriod, 0.0f);
    ringL_ = ringR_;
    ringSlot_ = 0;
    chunkR_.assign((size_t) cfg_.samplesPerVBlank, 0.0f);
    chunkL_ = chunkR_;
    outPerFrame_ = cfg_.outRate / kVBlankRate;
    frameOutTotal_ = 0;
    frameOutPos_ = 0;
}

void Mp2kEngine::setRuntimeParams(int maxChans, int masterVolume, int reverb, int samplesPerVBlank, int dmaPeriod) {
    bool reconfig = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (maxChans >= 1 && maxChans <= 12) cfg_.maxChans = maxChans;
        if (masterVolume >= 0 && masterVolume <= 15) cfg_.masterVolume = masterVolume;
        if (reverb >= 0 && reverb <= 127) reverb_ = reverb;
        if (samplesPerVBlank >= 16 && samplesPerVBlank <= 1024 && dmaPeriod >= 2 && dmaPeriod <= 16 &&
            (samplesPerVBlank != cfg_.samplesPerVBlank || dmaPeriod != cfg_.dmaPeriod)) {
            reconfig = true;
        }
    }
    if (reconfig) {
        EngineConfig c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            c = cfg_;
        }
        c.samplesPerVBlank = samplesPerVBlank;
        c.dmaPeriod = dmaPeriod;
        uint32_t song = currentSong();
        configure(c);
        if (song) start(song);
    }
}

void Mp2kEngine::setGain(float g) {
    std::lock_guard<std::mutex> lk(mtx_);
    cfg_.gain = g;
}

bool Mp2kEngine::paused() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return paused_;
}

uint32_t Mp2kEngine::currentSong() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return song_;
}

bool Mp2kEngine::finished() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return finished_;
}

void Mp2kEngine::readTone(uint32_t addr, Tone& t) const {
    t.type = rom_->u8(addr);
    t.key = rom_->u8(addr + 1);
    t.length = rom_->u8(addr + 2);
    t.panSweep = rom_->u8(addr + 3);
    t.wav = rom_->u32(addr + 4);
    t.attack = rom_->u8(addr + 8);
    t.decay = rom_->u8(addr + 9);
    t.sustain = rom_->u8(addr + 10);
    t.release = rom_->u8(addr + 11);
}

void Mp2kEngine::stopAllChannels() {
    for (auto& c : ds_) {
        c.status = 0;
        c.track = -1;
    }
    for (int i = 0; i < 4; ++i) {
        cgbOscOff(i + 1);
        cgb_[i].status = 0;
        cgb_[i].track = -1;
    }
}

bool Mp2kEngine::start(uint32_t header) {
    std::lock_guard<std::mutex> lk(mtx_);
    uint8_t n = rom_->u8(header);
    stopAllChannels();
    if (!rom_->valid(header, 8 + 4u * n) || n == 0 || n > 16) {
        // 트랙이 없는 곡(포켓몬의 0번 '빈 곡' 등)은 게임이 BGM 을 끄는 용도로 쓴다: 무음으로 둔다
        song_ = header;
        tracks_.clear();
        trackCount_ = 0;
        paused_ = false;
        finished_ = true;
        return false;
    }
    song_ = header;
    paused_ = false;
    finished_ = false;
    trackCount_ = n;
    tracks_.assign(n, Track());
    tone_ = rom_->u32(header + 4);
    priority_ = rom_->u8(header + 2);
    uint8_t reverb = rom_->u8(header + 3);
    if (reverb & 0x80) reverb_ = reverb & 0x7F;  // m4aSoundMode(SOUND_MODE_REVERB_SET)
    tempoD_ = 150;
    tempoI_ = 150;
    tempoU_ = 0x100;
    tempoC_ = 0;
    std::memset(memAcc_, 0, sizeof(memAcc_));
    for (int i = 0; i < n; ++i) {
        tracks_[i].flags = MPT_EXIST | MPT_START;
        tracks_[i].cmdPtr = rom_->u32(header + 8 + 4 * i);
    }
    return true;
}

void Mp2kEngine::stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    stopAllChannels();
    song_ = 0;
    tracks_.clear();
    trackCount_ = 0;
    finished_ = true;
}

void Mp2kEngine::setPaused(bool p) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (p == paused_) return;
    paused_ = p;
    if (p) {
        // m4aMPlayStop: 모든 트랙의 채널을 즉시 끊는다
        for (int i = 0; i < trackCount_; ++i) trackStop(i);
    }
}

// ---------------------------------------------------------------- 시퀀서

void Mp2kEngine::trackStop(int ti) {
    Track& t = tracks_[ti];
    if (!(t.flags & MPT_EXIST)) return;
    for (auto& c : ds_) {
        if (c.track == ti) {
            c.status = 0;
            c.track = -1;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (cgb_[i].track == ti) {
            if (cgb_[i].status) cgbOscOff(i + 1);
            cgb_[i].status = 0;
            cgb_[i].track = -1;
        }
    }
}

void Mp2kEngine::clearModM(Track& t) {
    t.modM = 0;
    t.lfoSpeedC = 0;
    t.flags |= (t.modT == 0) ? MPT_PITCHG : MPT_VOLCHG;
}

void Mp2kEngine::trkVolPitSet(Track& t) {
    if (t.flags & MPT_VOLSET) {
        int32_t x = (uint32_t) (t.vol * t.volX) >> 5;
        if (t.modT == 1) x = (uint32_t) (x * (t.modM + 128)) >> 7;
        int32_t y = 2 * t.pan + t.panX;
        if (t.modT == 2) y += t.modM;
        y = std::clamp(y, -128, 127);
        t.volMR = (uint8_t) ((uint32_t) ((y + 128) * x) >> 8);
        t.volML = (uint8_t) ((uint32_t) ((127 - y) * x) >> 8);
    }
    if (t.flags & MPT_PITSET) {
        int32_t bend = t.bend * t.bendRange;
        int32_t x = (t.tune + bend) * 4 + (t.keyShift << 8) + (t.keyShiftX << 8) + t.pitX;
        if (t.modT == 0) x += 16 * t.modM;
        t.keyM = (int8_t) (x >> 8);
        t.pitM = (uint8_t) x;
    }
    t.flags &= (uint8_t) ~(MPT_PITSET | MPT_VOLSET);
}

void Mp2kEngine::chnVolSet(uint8_t& right, uint8_t& left, uint8_t vel, int8_t rpan, const Track& t) {
    int32_t r = (vel * (128 + rpan) * t.volMR) >> 14;
    int32_t l = (vel * (127 - rpan) * t.volML) >> 14;
    right = (uint8_t) std::min(r, 255);
    left = (uint8_t) std::min(l, 255);
}

uint32_t Mp2kEngine::midiKeyToFreq(uint32_t wav, uint8_t key, uint8_t fine) const {
    uint32_t fineShifted = (uint32_t) fine << 24;
    if (key > 178) {
        key = 178;
        fineShifted = 255u << 24;
    }
    uint8_t s1 = scaleTable(key);
    uint32_t v1 = kFreqTable[s1 & 0xF] >> (s1 >> 4);
    uint8_t s2 = scaleTable(key + 1);
    uint32_t v2 = kFreqTable[s2 & 0xF] >> (s2 >> 4);
    return umul3232H32(rom_->u32(wav + 4), v1 + umul3232H32(v2 - v1, fineShifted));
}

uint32_t Mp2kEngine::midiKeyToCgbFreq(uint8_t chanNum, uint8_t key, uint8_t fine) const {
    if (chanNum == 4) {
        if (key <= 20) {
            key = 0;
        } else {
            key -= 21;
            if (key > 59) key = 59;
        }
        return kNoiseTable[key];
    }
    if (key <= 35) {
        fine = 0;
        key = 0;
    } else {
        key -= 36;
        if (key > 130) {
            key = 130;
            fine = 255;
        }
    }
    uint8_t s1 = cgbScaleTable(key);
    int32_t v1 = kCgbFreqTable[s1 & 0xF] >> (s1 >> 4);
    uint8_t s2 = cgbScaleTable(key + 1);
    int32_t v2 = kCgbFreqTable[s2 & 0xF] >> (s2 >> 4);
    return (uint32_t) (v1 + ((fine * (v2 - v1)) >> 8) + 2048);
}

void Mp2kEngine::plyGoto(Track& t) {
    t.cmdPtr = rom_->u32(t.cmdPtr);
}

void Mp2kEngine::plyFine(int ti) {
    Track& t = tracks_[ti];
    for (auto& c : ds_) {
        if (c.track != ti) continue;
        if (c.status & SF_ON) c.status |= SF_STOP;
        c.track = -1;
    }
    for (auto& c : cgb_) {
        if (c.track != ti) continue;
        if (c.status & SF_ON) c.status |= SF_STOP;
        c.track = -1;
    }
    t.flags = 0;
}

void Mp2kEngine::plyEndTie(int ti) {
    Track& t = tracks_[ti];
    uint8_t key;
    uint8_t b = rom_->u8(t.cmdPtr);
    if (b < 0x80) {
        t.key = b;
        t.cmdPtr++;
        key = b;
    } else {
        key = t.key;
    }
    // 연결 리스트 맨 앞(가장 최근에 연결된 채널)부터 찾는다
    uint8_t* best = nullptr;
    uint64_t bestSeq = 0;
    auto check = [&](uint8_t& status, uint8_t midiKey, int track, uint64_t seq) {
        if (track != ti) return;
        if (!(status & (SF_START | SF_ENV)) || (status & SF_STOP)) return;
        if (midiKey != key) return;
        if (!best || seq > bestSeq) {
            best = &status;
            bestSeq = seq;
        }
    };
    for (auto& c : ds_) check(c.status, c.midiKey, c.track, c.linkSeq);
    for (auto& c : cgb_) check(c.status, c.midiKey, c.track, c.linkSeq);
    if (best) *best |= SF_STOP;
}

void Mp2kEngine::plyMemAcc(int ti) {
    Track& t = tracks_[ti];
    uint8_t op = rom_->u8(t.cmdPtr++);
    uint8_t& a = memAcc_[rom_->u8(t.cmdPtr++) & 0xF];
    uint8_t data = rom_->u8(t.cmdPtr++);
    bool cond;
    switch (op) {
    case 0: a = data; return;
    case 1: a += data; return;
    case 2: a -= data; return;
    case 3: a = memAcc_[data & 0xF]; return;
    case 4: a += memAcc_[data & 0xF]; return;
    case 5: a -= memAcc_[data & 0xF]; return;
    case 6: cond = a == data; break;
    case 7: cond = a != data; break;
    case 8: cond = a > data; break;
    case 9: cond = a >= data; break;
    case 10: cond = a <= data; break;
    case 11: cond = a < data; break;
    case 12: cond = a == memAcc_[data & 0xF]; break;
    case 13: cond = a != memAcc_[data & 0xF]; break;
    case 14: cond = a > memAcc_[data & 0xF]; break;
    case 15: cond = a >= memAcc_[data & 0xF]; break;
    case 16: cond = a <= memAcc_[data & 0xF]; break;
    case 17: cond = a < memAcc_[data & 0xF]; break;
    default: return;
    }
    if (cond) {
        plyGoto(t);
    } else {
        t.cmdPtr += 4;
    }
}

void Mp2kEngine::plyXcmd(int ti) {
    Track& t = tracks_[ti];
    uint8_t n = rom_->u8(t.cmdPtr++);
    switch (n) {
    case 1:  // xwave
        t.tone.wav = rom_->u32(t.cmdPtr);
        t.cmdPtr += 4;
        break;
    case 2: t.tone.type = rom_->u8(t.cmdPtr++); break;
    case 4: t.tone.attack = rom_->u8(t.cmdPtr++); break;
    case 5: t.tone.decay = rom_->u8(t.cmdPtr++); break;
    case 6: t.tone.sustain = rom_->u8(t.cmdPtr++); break;
    case 7: t.tone.release = rom_->u8(t.cmdPtr++); break;
    case 8: t.echoVol = rom_->u8(t.cmdPtr++); break;
    case 9: t.echoLen = rom_->u8(t.cmdPtr++); break;
    case 10: t.tone.length = rom_->u8(t.cmdPtr++); break;
    case 11: t.tone.panSweep = rom_->u8(t.cmdPtr++); break;
    case 12: {  // xwait
        uint16_t len = rom_->u16(t.cmdPtr);
        if (t.timer < len) {
            t.timer++;
            t.cmdPtr -= 2;
            t.wait = 1;
        } else {
            t.timer = 0;
            t.cmdPtr += 2;
        }
        break;
    }
    case 13:
        t.unk3C = rom_->u32(t.cmdPtr);
        t.cmdPtr += 4;
        break;
    default:  // ply_xxx -> ply_fine
        plyFine(ti);
        break;
    }
}

void Mp2kEngine::plyNote(int ti, uint8_t n) {
    Track& t = tracks_[ti];
    t.gateTime = kClockTable[n];
    uint8_t b = rom_->u8(t.cmdPtr);
    if (b < 0x80) {
        t.key = b;
        t.cmdPtr++;
        b = rom_->u8(t.cmdPtr);
        if (b < 0x80) {
            t.velocity = b;
            t.cmdPtr++;
            b = rom_->u8(t.cmdPtr);
            if (b < 0x80) {
                t.gateTime += b;
                t.cmdPtr++;
            }
        }
    }

    int8_t rhythmPan = 0;
    Tone tone = t.tone;
    uint8_t key = t.key;
    if (t.tone.type & (TT_RHY | TT_SPL)) {
        uint8_t idx = (t.tone.type & TT_SPL) ? rom_->u8(t.tone.word8() + t.key) : t.key;
        readTone(t.tone.wav + idx * 12u, tone);
        if (tone.type & (TT_SPL | TT_RHY)) return;
        if (t.tone.type & TT_RHY) {
            if (tone.panSweep & 0x80) rhythmPan = (int8_t) ((tone.panSweep - 0xC0) * 2);
            key = tone.key;
        }
    }

    uint32_t prio = std::min<uint32_t>(priority_ + t.priority, 255);
    uint8_t cgbType = tone.type & TT_CGB;
    uint64_t seq = ++linkCounter_;

    if (cgbType) {
        CgbChan& c = cgb_[cgbType - 1];
        if ((c.status & SF_ON) && !(c.status & SF_STOP)) {
            if (c.priority > prio) return;
            if (c.priority == prio && c.track < ti) return;  // 원본: 트랙 포인터 비교 (NULL 이 가장 작음)
        }
        c.track = ti;
        c.linkSeq = seq;
        t.lfoDelayC = t.lfoDelay;
        if (t.lfoDelay != 0) clearModM(t);
        trkVolPitSet(t);
        c.gateTime = t.gateTime;
        c.midiKey = t.key;
        c.velocity = t.velocity;
        c.priority = (uint8_t) prio;
        c.key = key;
        c.rhythmPan = rhythmPan;
        c.type = tone.type;
        c.wavePointer = tone.wav;
        c.attack = tone.attack;
        c.decay = tone.decay;
        c.sustain = tone.sustain;
        c.release = tone.release;
        c.echoVol = t.echoVol;
        c.echoLen = t.echoLen;
        chnVolSet(c.rightVol, c.leftVol, c.velocity, c.rhythmPan, t);
        int k = c.key + t.keyM;
        if (k < 0) k = 0;
        c.length = tone.length;
        uint8_t ps = tone.panSweep;
        c.sweep = ((ps & 0x80) || !(ps & 0x70)) ? 8 : ps;
        c.frequency = midiKeyToCgbFreq(cgbType, (uint8_t) k, t.pitM);
        c.status = SF_START;
        t.flags &= 0xF0;
        return;
    }

    // DirectSound 채널 할당 (ply_note 의 우선순위 규칙)
    int sel = -1;
    uint32_t bestPrio = prio;
    int bestTrack = ti;
    bool foundStopping = false;
    for (int i = 0; i < cfg_.maxChans; ++i) {
        DsChan& c = ds_[i];
        if (!(c.status & SF_ON)) {
            sel = i;
            break;
        }
        bool compare = false;
        if (c.status & SF_STOP) {
            if (!foundStopping) {
                foundStopping = true;
                bestPrio = c.priority;
                bestTrack = c.track;
                sel = i;
                continue;
            }
            compare = true;
        } else {
            compare = !foundStopping;
        }
        if (!compare) continue;
        if (c.priority < bestPrio) {
            bestPrio = c.priority;
            bestTrack = c.track;
            sel = i;
        } else if (c.priority == bestPrio) {
            if (c.track > bestTrack) {
                bestTrack = c.track;
                sel = i;
            } else if (c.track == bestTrack) {
                sel = i;
            }
        }
    }
    if (sel < 0) return;

    DsChan& c = ds_[sel];
    c.track = ti;
    c.linkSeq = seq;
    t.lfoDelayC = t.lfoDelay;
    if (t.lfoDelay != 0) clearModM(t);
    trkVolPitSet(t);
    c.gateTime = t.gateTime;
    c.midiKey = t.key;
    c.velocity = t.velocity;
    c.priority = (uint8_t) prio;
    c.key = key;
    c.rhythmPan = rhythmPan;
    c.type = tone.type;
    c.wav = tone.wav;
    c.attack = tone.attack;
    c.decay = tone.decay;
    c.sustain = tone.sustain;
    c.release = tone.release;
    c.echoVol = t.echoVol;
    c.echoLen = t.echoLen;
    chnVolSet(c.rightVol, c.leftVol, c.velocity, c.rhythmPan, t);
    int k = c.key + t.keyM;
    if (k < 0) k = 0;
    c.count = t.unk3C;
    c.frequency = midiKeyToFreq(c.wav, (uint8_t) k, t.pitM);
    c.status = SF_START;
    t.flags &= 0xF0;
}

void Mp2kEngine::execCommand(int ti, uint8_t cmd) {
    Track& t = tracks_[ti];
    switch (cmd) {
    case 0xB1: plyFine(ti); break;
    case 0xB2: plyGoto(t); break;
    case 0xB3:  // PATT
        if (t.patternLevel < 3) {
            t.patternStack[t.patternLevel] = t.cmdPtr + 4;
            t.patternLevel++;
            plyGoto(t);
        } else {
            plyFine(ti);
        }
        break;
    case 0xB4:  // PEND
        if (t.patternLevel) {
            t.patternLevel--;
            t.cmdPtr = t.patternStack[t.patternLevel];
        }
        break;
    case 0xB5: {  // REPT
        uint32_t start = t.cmdPtr;
        uint8_t count = rom_->u8(t.cmdPtr);
        if (count == 0) {
            t.cmdPtr++;
            plyGoto(t);
            break;
        }
        t.repN++;
        t.cmdPtr++;
        if (t.repN < count) {
            plyGoto(t);
        } else {
            t.repN = 0;
            t.cmdPtr = start + 5;
        }
        break;
    }
    case 0xB9: plyMemAcc(ti); break;
    case 0xBA: t.priority = rom_->u8(t.cmdPtr++); break;
    case 0xBB: {
        tempoD_ = (uint16_t) (rom_->u8(t.cmdPtr++) * 2);
        tempoI_ = (uint16_t) ((tempoD_ * tempoU_) >> 8);
        break;
    }
    case 0xBC:
        t.keyShift = (int8_t) rom_->u8(t.cmdPtr++);
        t.flags |= MPT_PITCHG;
        break;
    case 0xBD: {
        uint8_t idx = rom_->u8(t.cmdPtr++);
        readTone(tone_ + idx * 12u, t.tone);
        break;
    }
    case 0xBE:
        t.vol = rom_->u8(t.cmdPtr++);
        t.flags |= MPT_VOLCHG;
        break;
    case 0xBF:
        t.pan = (int8_t) (rom_->u8(t.cmdPtr++) - 0x40);
        t.flags |= MPT_VOLCHG;
        break;
    case 0xC0:
        t.bend = (int8_t) (rom_->u8(t.cmdPtr++) - 0x40);
        t.flags |= MPT_PITCHG;
        break;
    case 0xC1:
        t.bendRange = rom_->u8(t.cmdPtr++);
        t.flags |= MPT_PITCHG;
        break;
    case 0xC2:
        t.lfoSpeed = rom_->u8(t.cmdPtr++);
        if (t.lfoSpeed == 0) clearModM(t);
        break;
    case 0xC3: t.lfoDelay = rom_->u8(t.cmdPtr++); break;
    case 0xC4:
        t.mod = rom_->u8(t.cmdPtr++);
        if (t.mod == 0) clearModM(t);
        break;
    case 0xC5: {
        uint8_t v = rom_->u8(t.cmdPtr++);
        if (t.modT != v) {
            t.modT = v;
            t.flags |= MPT_VOLCHG | MPT_PITCHG;
        }
        break;
    }
    case 0xC8:
        t.tune = (int8_t) (rom_->u8(t.cmdPtr++) - 0x40);
        t.flags |= MPT_PITCHG;
        break;
    case 0xCC: t.cmdPtr += 2; break;  // PORT: 사운드 레지스터 직접 쓰기, 재현하지 않음
    case 0xCD: plyXcmd(ti); break;
    case 0xCE: plyEndTie(ti); break;
    default: plyFine(ti); break;  // 점프 테이블에서 ply_fine 으로 연결된 명령
    }
}

void Mp2kEngine::trackTick(int ti) {
    Track& t = tracks_[ti];

    // 게이트 타임 감소
    auto gate = [&](uint8_t& status, uint8_t& gateTime, int& track) {
        if (track != ti) return;
        if (status & SF_ON) {
            if (gateTime != 0) {
                if (--gateTime == 0) status |= SF_STOP;
            }
        } else {
            track = -1;  // ClearChain
        }
    };
    for (auto& c : ds_) gate(c.status, c.gateTime, c.track);
    for (auto& c : cgb_) gate(c.status, c.gateTime, c.track);

    if (t.flags & MPT_START) {
        uint32_t cmd = t.cmdPtr;
        uint32_t stack[3] = {t.patternStack[0], t.patternStack[1], t.patternStack[2]};
        t = Track();  // Clear64byte (0x00~0x3F)
        t.cmdPtr = cmd;
        std::memcpy(t.patternStack, stack, sizeof(stack));
        t.flags = MPT_EXIST;
        t.bendRange = 2;
        t.volX = 0x40;
        t.lfoSpeed = 0x16;
        t.tone.type = 1;
    }

    while (t.wait == 0) {
        uint8_t cmd = rom_->u8(t.cmdPtr);
        if (!rom_->valid(t.cmdPtr)) {
            plyFine(ti);
            return;
        }
        if (cmd < 0x80) {
            cmd = t.runningStatus;
        } else {
            t.cmdPtr++;
            if (cmd >= 0xBD) t.runningStatus = cmd;
        }
        if (cmd >= 0xCF) {
            plyNote(ti, (uint8_t) (cmd - 0xCF));
        } else if (cmd > 0xB0) {
            execCommand(ti, cmd);
            if (t.flags == 0) return;
        } else if (cmd >= 0x80) {
            t.wait = kClockTable[cmd - 0x80];
        } else {
            // 러닝 스테이터스가 없는 상태에서 데이터 바이트를 만남: 원본은 jump table 범위 밖
            plyFine(ti);
            return;
        }
    }

    t.wait--;
    if (t.lfoSpeed == 0 || t.mod == 0) return;
    if (t.lfoDelayC != 0) {
        t.lfoDelayC--;
        return;
    }
    t.lfoSpeedC = (uint8_t) (t.lfoSpeedC + t.lfoSpeed);
    uint8_t c = t.lfoSpeedC;
    int32_t r2;
    if (((uint8_t) (c - 0x40)) & 0x80) {
        r2 = (int8_t) c;
    } else {
        r2 = 0x80 - c;
    }
    r2 = (t.mod * r2) >> 6;
    if ((int8_t) (t.modM ^ (int8_t) r2) != 0) {
        t.modM = (int8_t) r2;
        t.flags |= (t.modT == 0) ? MPT_PITCHG : MPT_VOLCHG;
    }
}

void Mp2kEngine::mplayMain() {
    if (!song_ || paused_ || finished_) return;

    uint32_t tempoC = tempoC_ + tempoI_;
    while (tempoC >= 150) {
        bool anyTrack = false;
        for (int i = 0; i < trackCount_; ++i) {
            if (!(tracks_[i].flags & MPT_EXIST)) continue;
            anyTrack = true;
            trackTick(i);
        }
        if (!anyTrack) {
            finished_ = true;
            tempoC_ = (uint16_t) tempoC;
            return;
        }
        tempoC -= 150;
    }
    tempoC_ = (uint16_t) tempoC;

    for (int i = 0; i < trackCount_; ++i) {
        Track& t = tracks_[i];
        if (!(t.flags & MPT_EXIST)) continue;
        if (!(t.flags & (MPT_VOLCHG | MPT_PITCHG))) continue;
        trkVolPitSet(t);
        for (auto& c : ds_) {
            if (c.track != i) continue;
            if (!(c.status & SF_ON)) {
                c.track = -1;
                continue;
            }
            if (t.flags & MPT_VOLCHG) chnVolSet(c.rightVol, c.leftVol, c.velocity, c.rhythmPan, t);
            if (t.flags & MPT_PITCHG) {
                int k = c.key + t.keyM;
                if (k < 0) k = 0;
                c.frequency = midiKeyToFreq(c.wav, (uint8_t) k, t.pitM);
            }
        }
        for (int ch = 0; ch < 4; ++ch) {
            CgbChan& c = cgb_[ch];
            if (c.track != i) continue;
            if (!(c.status & SF_ON)) {
                c.track = -1;
                continue;
            }
            if (t.flags & MPT_VOLCHG) {
                chnVolSet(c.rightVol, c.leftVol, c.velocity, c.rhythmPan, t);
                c.modify |= MO_VOL;
            }
            if (t.flags & MPT_PITCHG) {
                int k = c.key + t.keyM;
                if (k < 0) k = 0;
                c.frequency = midiKeyToCgbFreq((uint8_t) (ch + 1), (uint8_t) k, t.pitM);
                c.modify |= MO_PIT;
            }
        }
        t.flags &= 0xF0;
    }
}

// ---------------------------------------------------------------- PSG (CGB) 채널

void Mp2kEngine::cgbOscOff(int ch) {
    CgbChan& c = cgb_[ch - 1];
    c.hwOn = false;
    c.hwVol = 0;
    c.hwCh3Scale = 0;
}

void Mp2kEngine::cgbTrigger(int ch) {
    CgbChan& c = cgb_[ch - 1];
    c.hwOn = true;
    if (ch == 3) {
        if (c.wavePointer != c.currentPointer) {
            for (int i = 0; i < 16; ++i) {
                uint8_t b = rom_->u8(c.wavePointer + i);
                c.wave[i * 2] = b >> 4;
                c.wave[i * 2 + 1] = b & 0xF;
            }
            c.currentPointer = c.wavePointer;
        }
        c.lengthLeft = c.length ? (256 - c.length) / 256.0 : -1;
    } else {
        c.lengthLeft = c.length ? (64 - (c.length & 63)) / 256.0 : -1;
        if (ch == 4) c.lfsr = 0x7FFF;
        if (ch == 1) c.sweepTimer = 0;
    }
}

void Mp2kEngine::cgbModVol(CgbChan& c) {
    uint32_t r = c.rightVol, l = c.leftVol;
    bool hardPan = false;
    if (r >= l) {
        if (r / 2 >= l) {
            c.pan = 0x0F;
            hardPan = true;
        }
    } else {
        if (l / 2 >= r) {
            c.pan = 0xF0;
            hardPan = true;
        }
    }
    uint32_t goal = (l + r) / 16;
    if (!hardPan) c.pan = 0xFF;
    if (goal > 15) goal = 15;
    c.envGoal = (uint8_t) goal;
    c.sustainGoal = (uint8_t) ((c.envGoal * c.sustain + 15) >> 4);
    c.pan &= c.panMask;
}

void Mp2kEngine::cgbSound() {
    if (c15_)
        c15_--;
    else
        c15_ = 14;

    for (int ch = 1; ch <= 4; ++ch) {
        CgbChan& c = cgb_[ch - 1];
        if (!(c.status & SF_ON)) continue;
        int prevC15 = c15_;

        // 아래 흐름은 m4a.c CgbSound 의 goto 구조를 그대로 옮긴 것
        enum { STEP_REPEAT, STEP_COMPLETE, ENV_COMPLETE, CHAN_COMPLETE, OSC_OFF, DECAY_START, SUSTAIN_START, SUSTAIN, ECHO_START } next;

        if (c.status & SF_START) {
            if (!(c.status & SF_STOP)) {
                c.status = SF_ENV_ATTACK;
                c.modify = MO_PIT | MO_VOL;
                cgbModVol(c);
                cgbTrigger(ch);
                c.envCounter = c.attack;
                if ((int8_t) c.attack) {
                    c.envVol = 0;
                    next = STEP_COMPLETE;
                } else {
                    next = DECAY_START;
                }
            } else {
                next = OSC_OFF;
            }
        } else if (c.status & SF_IEC) {
            c.echoLen--;
            next = ((int8_t) c.echoLen <= 0) ? OSC_OFF : ENV_COMPLETE;
        } else if ((c.status & SF_STOP) && (c.status & SF_ENV)) {
            c.status &= ~SF_ENV;
            c.envCounter = c.release;
            if ((int8_t) c.release) {
                c.modify |= MO_VOL;
                next = STEP_COMPLETE;
            } else {
                next = ECHO_START;
            }
        } else {
            next = STEP_REPEAT;
        }

        bool done = false;
        while (!done) {
            switch (next) {
            case STEP_REPEAT:
                if (c.envCounter == 0) {
                    if (ch == 3) c.modify |= MO_VOL;
                    cgbModVol(c);
                    uint8_t env = c.status & SF_ENV;
                    if (env == SF_ENV_RELEASE) {
                        c.envVol--;
                        if ((int8_t) c.envVol <= 0) {
                            next = ECHO_START;
                            continue;
                        }
                        c.envCounter = c.release;
                    } else if (env == SF_ENV_SUSTAIN) {
                        next = SUSTAIN;
                        continue;
                    } else if (env == SF_ENV_DECAY) {
                        c.envVol--;
                        if ((int8_t) c.envVol <= (int8_t) c.sustainGoal) {
                            next = SUSTAIN_START;
                            continue;
                        }
                        c.envCounter = c.decay;
                    } else {
                        c.envVol++;
                        if (c.envVol >= c.envGoal) {
                            next = DECAY_START;
                            continue;
                        }
                        c.envCounter = c.attack;
                    }
                }
                next = STEP_COMPLETE;
                break;
            case DECAY_START:
                c.status--;
                c.envCounter = c.decay;
                if (c.envCounter) {
                    c.modify |= MO_VOL;
                    c.envVol = c.envGoal;
                    next = STEP_COMPLETE;
                } else {
                    next = SUSTAIN_START;
                }
                break;
            case SUSTAIN_START:
                if (c.sustain == 0) {
                    c.status &= ~SF_ENV;
                    next = ECHO_START;
                } else {
                    c.status--;
                    c.modify |= MO_VOL;
                    next = SUSTAIN;
                }
                break;
            case SUSTAIN:
                c.envVol = c.sustainGoal;
                c.envCounter = 7;
                next = STEP_COMPLETE;
                break;
            case ECHO_START:
                c.envVol = (uint8_t) ((c.envGoal * c.echoVol + 0xFF) >> 8);
                if (c.envVol) {
                    c.status |= SF_IEC;
                    c.modify |= MO_VOL;
                    next = ENV_COMPLETE;
                } else {
                    next = OSC_OFF;
                }
                break;
            case STEP_COMPLETE:
                c.envCounter--;
                if (prevC15 == 0) {
                    prevC15--;
                    next = STEP_REPEAT;
                } else {
                    next = ENV_COMPLETE;
                }
                break;
            case ENV_COMPLETE:
                if (c.modify & MO_PIT) {
                    if (ch == 4)
                        c.hwNoiseReg = (uint8_t) (((c.wavePointer & 1) << 3) | c.frequency);
                    else
                        c.hwFreq = c.frequency & 0x7FF;
                }
                // 채널 1/2/4 는 하드웨어 엔벨로프가 소프트웨어 envVol 과 같은 속도로 움직이므로 매 프레임 반영
                if (ch != 3) c.hwVol = c.envVol & 0xF;
                if (c.modify & MO_VOL) {
                    c.hwPan = c.pan;
                    if (ch == 3) c.hwCh3Scale = cgb3Scale(kCgb3Vol[c.envVol & 0xF]);
                }
                next = CHAN_COMPLETE;
                break;
            case OSC_OFF:
                cgbOscOff(ch);
                c.status = 0;
                next = CHAN_COMPLETE;
                break;
            case CHAN_COMPLETE:
                c.modify = 0;
                done = true;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- DirectSound 믹싱

const std::vector<int8_t>* Mp2kEngine::sampleData(uint32_t wav, bool reverse) {
    uint64_t key = ((uint64_t) wav << 1) | (reverse ? 1 : 0);
    auto it = sampleCache_.find(key);
    if (it != sampleCache_.end()) return &it->second;

    std::vector<int8_t> s;
    uint32_t size = rom_->u32(wav + 12);
    uint16_t wtype = rom_->u16(wav);
    if (size > 0 && size < 16 * 1024 * 1024) {
        if (wtype != 0) {
            // DPCM: 64 샘플마다 0x21 바이트 블록
            s.resize(size);
            for (uint32_t i = 0; i < size; i += 64) {
                uint32_t blk = wav + 0x10 + (i / 64) * 0x21;
                int8_t v = (int8_t) rom_->u8(blk);
                s[i] = v;
                uint32_t idx = 1;
                uint8_t b = rom_->u8(blk + 1);
                v = (int8_t) (v + kDeltaTable[b & 0xF]);
                if (i + idx < size) s[i + idx] = v;
                idx++;
                for (uint32_t j = 2; j < 0x21 && idx < 64; ++j) {
                    b = rom_->u8(blk + j);
                    v = (int8_t) (v + kDeltaTable[b >> 4]);
                    if (i + idx < size) s[i + idx] = v;
                    idx++;
                    v = (int8_t) (v + kDeltaTable[b & 0xF]);
                    if (i + idx < size) s[i + idx] = v;
                    idx++;
                }
            }
        } else if (rom_->valid(wav + 16, size)) {
            const int8_t* p = rom_->ptr(wav + 16);
            s.assign(p, p + size);
        }
        if (reverse) std::reverse(s.begin(), s.end());
    }
    return &sampleCache_.emplace(key, std::move(s)).first->second;
}

void Mp2kEngine::mixDirectSound() {
    const int spv = cfg_.samplesPerVBlank;
    const double pcmFreq = spv * kVBlankRate;
    const int slot = ringSlot_;
    const int nextSlot = (slot + 1) % cfg_.dmaPeriod;
    float* curR = &ringR_[(size_t) slot * spv];
    float* curL = &ringL_[(size_t) slot * spv];
    const float* nxR = &ringR_[(size_t) nextSlot * spv];
    const float* nxL = &ringL_[(size_t) nextSlot * spv];

    for (int i = 0; i < spv; ++i) {
        if (reverb_) {
            float v = (curR[i] + curL[i] + nxR[i] + nxL[i]) * reverb_ / 512.0f;
            chunkR_[i] = v;
            chunkL_[i] = v;
        } else {
            chunkR_[i] = 0;
            chunkL_[i] = 0;
        }
    }

    for (int ci = 0; ci < cfg_.maxChans; ++ci) {
        DsChan& c = ds_[ci];
        if (!(c.status & SF_ON)) continue;
        int env;
        if (c.status & SF_START) {
            if (c.status & SF_STOP) {
                c.status = 0;
                continue;
            }
            c.status = SF_ENV_ATTACK;
            c.reverse = (c.type & (TT_CMP | TT_REV)) && (c.type & TT_REV);
            bool compressed = (c.type & (TT_CMP | TT_REV)) && rom_->u16(c.wav) != 0;
            uint32_t size = rom_->u32(c.wav + 12);
            if (!compressed && !(c.type & TT_REV) && !rom_->valid(c.wav + 16, size)) {
                c.status = 0;
                continue;
            }
            c.samples = sampleData(c.wav, c.reverse);
            c.pos = c.count;
            c.loopStart = rom_->u32(c.wav + 8);
            env = 0;
            if ((rom_->u8(c.wav + 3) & 0xC0) && !c.reverse) c.status |= SF_LOOP;
            env += c.attack;
            if (env >= 0xFF) {
                env = 0xFF;
                c.status--;
            }
        } else {
            env = c.env;
            bool toEcho = false;
            if (c.status & SF_IEC) {
                c.echoLen--;
                if (c.echoLen == 0 || c.echoLen == 0xFF) {
                    c.status = 0;
                    continue;
                }
            } else if (c.status & SF_STOP) {
                env = (env * c.release) >> 8;
                if (env <= c.echoVol) toEcho = true;
            } else if ((c.status & SF_ENV) == SF_ENV_DECAY) {
                env = (env * c.decay) >> 8;
                if (env <= c.sustain) {
                    env = c.sustain;
                    if (env == 0) {
                        toEcho = true;
                    } else {
                        c.status--;
                    }
                }
            } else if ((c.status & SF_ENV) == SF_ENV_ATTACK) {
                env += c.attack;
                if (env >= 0xFF) {
                    env = 0xFF;
                    c.status--;
                }
            }
            if (toEcho) {
                env = c.echoVol;
                if (env == 0) {
                    c.status = 0;
                    continue;
                }
                c.status |= SF_IEC;
            }
        }
        c.env = (uint8_t) env;
        int e = (env * (cfg_.masterVolume + 1)) >> 4;
        c.envR = (uint8_t) ((c.rightVol * e) >> 8);
        c.envL = (uint8_t) ((c.leftVol * e) >> 8);

        const std::vector<int8_t>& s = *c.samples;
        const double size = (double) s.size();
        if (s.empty()) {
            c.status = 0;
            continue;
        }
        const bool loop = (c.status & SF_LOOP) != 0;
        const double loopLen = size - c.loopStart;
        const double step = (c.type & TT_FIX) ? 1.0 : c.frequency / pcmFreq;
        const float vr = c.envR / 256.0f, vl = c.envL / 256.0f;
        for (int i = 0; i < spv; ++i) {
            if (c.pos >= size) {
                if (loop && loopLen > 0) {
                    while (c.pos >= size) c.pos -= loopLen;
                } else {
                    c.status = 0;
                    break;
                }
            }
            size_t p = (size_t) c.pos;
            double frac = c.pos - (double) p;
            float a = s[p];
            float b;
            if (p + 1 < s.size())
                b = s[p + 1];
            else
                b = (loop && c.loopStart < s.size()) ? s[c.loopStart] : a;
            float v = a + (float) ((b - a) * frac);
            chunkR_[i] += v * vr;
            chunkL_[i] += v * vl;
            c.pos += step;
        }
    }

    for (int i = 0; i < spv; ++i) {
        chunkR_[i] = std::clamp(chunkR_[i], -128.0f, 127.0f);
        chunkL_[i] = std::clamp(chunkL_[i], -128.0f, 127.0f);
        curR[i] = chunkR_[i];
        curL[i] = chunkL_[i];
    }
    ringSlot_ = nextSlot;
}

void Mp2kEngine::vblank() {
    mplayMain();
    cgbSound();
    mixDirectSound();
}

// ---------------------------------------------------------------- 출력

void Mp2kEngine::render(float* out, int frames) {
    std::lock_guard<std::mutex> lk(mtx_);
    const int spv = cfg_.samplesPerVBlank;
    const double dt = 1.0 / cfg_.outRate;
    static const float kDuty[4] = {0.125f, 0.25f, 0.5f, 0.75f};

    for (int n = 0; n < frames; ++n) {
        if (paused_ || !song_) {
            out[n * 2] = 0;
            out[n * 2 + 1] = 0;
            continue;
        }
        if (frameOutPos_ >= frameOutTotal_) {
            lastDsR_ = chunkR_.empty() ? 0 : chunkR_[spv - 1];
            lastDsL_ = chunkL_.empty() ? 0 : chunkL_[spv - 1];
            vblank();
            outAcc_ += outPerFrame_;
            frameOutTotal_ = (int) outAcc_;
            outAcc_ -= frameOutTotal_;
            frameOutPos_ = 0;
            if (frameOutTotal_ <= 0) frameOutTotal_ = 1;
        }

        // DirectSound: 이번 프레임 청크를 출력 레이트로 선형 보간
        double f = (double) frameOutPos_ * spv / frameOutTotal_ - 1.0;
        float dsR, dsL;
        if (f < 0) {
            float fr = (float) (f + 1.0);
            dsR = lastDsR_ + (chunkR_[0] - lastDsR_) * fr;
            dsL = lastDsL_ + (chunkL_[0] - lastDsL_) * fr;
        } else {
            int k = (int) f;
            float fr = (float) (f - k);
            int k1 = std::min(k + 1, spv - 1);
            dsR = chunkR_[k] + (chunkR_[k1] - chunkR_[k]) * fr;
            dsL = chunkL_[k] + (chunkL_[k1] - chunkL_[k]) * fr;
        }
        frameOutPos_++;

        // PSG 4채널
        float psgR = 0, psgL = 0;
        for (int ch = 1; ch <= 4; ++ch) {
            CgbChan& c = cgb_[ch - 1];
            if (!c.hwOn) continue;
            if (c.lengthLeft >= 0) {
                c.lengthLeft -= dt;
                if (c.lengthLeft <= 0) {
                    c.hwOn = false;
                    continue;
                }
            }
            float level = 0;
            if (ch == 1 || ch == 2) {
                if (ch == 1) {
                    int time = (c.sweep >> 4) & 7;
                    int shift = c.sweep & 7;
                    if (time && shift) {
                        c.sweepTimer += dt;
                        double period = time / 128.0;
                        while (c.sweepTimer >= period) {
                            c.sweepTimer -= period;
                            int delta = (int) (c.hwFreq >> shift);
                            int nf = (c.sweep & 8) ? (int) c.hwFreq - delta : (int) c.hwFreq + delta;
                            if (nf > 2047) {
                                c.hwOn = false;
                                break;
                            }
                            if (nf >= 0) c.hwFreq = (uint32_t) nf;
                        }
                        if (!c.hwOn) continue;
                    }
                }
                double hz = 131072.0 / (2048 - (int) c.hwFreq);
                c.phase += hz * dt;
                c.phase -= std::floor(c.phase);
                level = (c.phase < kDuty[c.wavePointer & 3]) ? c.hwVol : 0;
            } else if (ch == 3) {
                double hz = 65536.0 / (2048 - (int) c.hwFreq);
                c.phase += hz * dt;
                c.phase -= std::floor(c.phase);
                level = c.wave[(int) (c.phase * 32) & 31] * c.hwCh3Scale;
            } else {
                int s = c.hwNoiseReg >> 4;
                int r = c.hwNoiseReg & 7;
                double rate = 524288.0 / (r ? r : 0.5) / (double) (2 << s);
                c.noiseAcc += rate * dt;
                int steps = 0;
                while (c.noiseAcc >= 1.0 && steps < 4096) {
                    c.noiseAcc -= 1.0;
                    ++steps;
                    uint16_t bit = (c.lfsr ^ (c.lfsr >> 1)) & 1;
                    c.lfsr = (uint16_t) ((c.lfsr >> 1) | (bit << 14));
                    if (c.hwNoiseReg & 8) c.lfsr = (uint16_t) ((c.lfsr & ~0x40) | (bit << 6));
                }
                if (c.noiseAcc >= 1.0) c.noiseAcc = 0;
                level = (~c.lfsr & 1) ? c.hwVol : 0;
            }
            if (c.hwPan & (0x01 << (ch - 1))) psgR += level;
            if (c.hwPan & (0x10 << (ch - 1))) psgL += level;
        }

        // mGBA 기준 비율: DMA(100%) = s8 * 4, PSG(100%, NR50=7) = 레벨 * 16  ->  512 로 정규화
        float r = dsR / 128.0f + psgR / 32.0f;
        float l = dsL / 128.0f + psgL / 32.0f;

        // PSG 는 단극성이라 DC 제거
        float yr = r - hpInR_ + 0.995f * hpR_;
        float yl = l - hpInL_ + 0.995f * hpL_;
        hpInR_ = r;
        hpInL_ = l;
        hpR_ = yr;
        hpL_ = yl;

        out[n * 2] = std::clamp(yl * cfg_.gain, -1.0f, 1.0f);
        out[n * 2 + 1] = std::clamp(yr * cfg_.gain, -1.0f, 1.0f);
    }
}
