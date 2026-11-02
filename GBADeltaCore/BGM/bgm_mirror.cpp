#include "bgm_mirror.h"

#include <algorithm>

namespace {
constexpr uint32_t STATUS_PAUSE = 0x80000000u;
constexpr uint32_t STATUS_TRACK = 0x0000FFFFu;
constexpr uint32_t TRACK_SIZE = 0x50;
}  // namespace

bool isPlayerInfoValid(GbaBus& bus, uint32_t info) {
    uint32_t ident = bus.r32(info + 0x34);
    // MPlayMain 실행 중에는 ident 가 1 증가해 있다
    return ident == kMp2kIdent || ident == kMp2kIdent + 1;
}

uint32_t findBgmPlayerInfo(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    for (const auto& c : cands) {
        if (c.entries.empty()) continue;
        const MPlayEntry& e = c.entries[0];
        if (!isPlayerInfoValid(bus, e.info)) continue;
        if (bus.r8(e.info + 8) != e.numTracks) continue;
        if (bus.r32(e.info + 0x2C) != e.tracks) continue;
        return e.info;
    }
    return 0;
}

BgmSnapshot readBgm(GbaBus& bus, uint32_t info) {
    BgmSnapshot s;
    s.header = bus.r32(info);
    s.status = bus.r32(info + 4);
    s.clock = bus.r32(info + 0x0C);
    s.fadeOI = bus.r16(info + 0x24);
    return s;
}

SoundInfoSnapshot readSoundInfo(GbaBus& bus) {
    SoundInfoSnapshot s;
    uint32_t p = bus.r32(kSoundInfoPtr);
    const bool inEwram = p >= 0x02000000u && p <= 0x02040000u - 0x14u;
    const bool inIwram = p >= 0x03000000u && p <= 0x03008000u - 0x14u;
    if (!inEwram && !inIwram) return s;
    uint32_t ident = bus.r32(p);
    if (ident != kMp2kIdent && ident != kMp2kIdent + 1) return s;
    s.valid = true;
    s.reverb = bus.r8(p + 5);
    s.maxChans = bus.r8(p + 6);
    s.masterVolume = bus.r8(p + 7);
    s.dmaPeriod = bus.r8(p + 0x0B);
    s.samplesPerVBlank = (int) bus.r32(p + 0x10);
    return s;
}

void muteBgmTracks(GbaBus& bus, uint32_t info) {
    uint8_t n = bus.r8(info + 8);
    uint32_t tracks = bus.r32(info + 0x2C);
    if (n > 16) n = 16;
    for (uint8_t i = 0; i < n; ++i) {
        uint32_t t = tracks + i * TRACK_SIZE;
        uint8_t flags = bus.r8(t);
        if (!(flags & 0x80)) continue;
        bus.w8(t + 0x12, 0);  // vol
        bus.w8(t + 0x13, 0);  // volX
        bus.w8(t, (uint8_t) (flags | 0x03));  // MPT_FLG_VOLCHG
    }
}

void restartGameSong(GbaBus& bus, uint32_t info) {
    // m4a MPlayStart 와 같은 순서: 트랙을 멈추고(TrackStop) 곡 머리의 트랙 위치부터 다시 시작하게 한다.
    // 트랙 초기화(볼륨 등)는 게임의 MPlayMain 이 MPT_FLG_START 를 보고 한다
    const uint32_t header = bus.r32(info);
    if (header < 0x08000000u || header >= 0x0A000000u) return;
    auto w16 = [&](uint32_t a, uint16_t v) {
        bus.w8(a, (uint8_t) v);
        bus.w8(a + 1, (uint8_t) (v >> 8));
    };
    auto w32 = [&](uint32_t a, uint32_t v) {
        w16(a, (uint16_t) v);
        w16(a + 2, (uint16_t) (v >> 16));
    };
    const uint8_t songTracks = bus.r8(header);
    uint8_t n = bus.r8(info + 8);
    if (n > 16) n = 16;
    const uint32_t tracks = bus.r32(info + 0x2C);
    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t t = tracks + i * TRACK_SIZE;
        if (bus.r8(t) & 0x80) {  // TrackStop
            uint32_t chan = bus.r32(t + 0x20);
            for (int guard = 0; chan && guard < 32; ++guard) {
                const uint8_t flags = bus.r8(chan);
                if (flags) {
                    // GB 음원 채널은 게임이 끄도록 정지 표시만, 다이렉트 사운드 채널은 바로 끈다
                    bus.w8(chan, (bus.r8(chan + 1) & 0x07) ? (uint8_t) (flags | 0x40) : 0);
                }
                w32(chan + 0x2C, 0);           // track
                chan = bus.r32(chan + 0x34);  // nextChannelPointer
            }
            w32(t + 0x20, 0);  // chan
        }
        if (i < songTracks) {
            bus.w8(t, 0xC0);                                 // MPT_FLG_EXIST | MPT_FLG_START
            w32(t + 0x40, bus.r32(header + 8 + 4u * i));  // cmdPtr
        } else {
            bus.w8(t, 0);
        }
    }
    w32(info + 4, 0);                       // status
    bus.w8(info + 9, bus.r8(header + 2));   // priority
    w32(info + 0x0C, 0);                    // clock
    w16(info + 0x1C, 150);                  // tempoD
    w16(info + 0x1E, 0x100);                // tempoU
    w16(info + 0x20, 150);                  // tempoI
    w16(info + 0x22, 0);                    // tempoC
    w16(info + 0x24, 0);                    // fadeOI
    w32(info + 0x30, bus.r32(header + 4));  // tone
}

bool isLoopingSong(const Rom& rom, uint32_t header) {
    const uint8_t n = rom.u8(header);
    if (n == 0 || n > 16 || !rom.valid(header, 8 + 4u * n)) return false;
    for (uint8_t t = 0; t < n; ++t) {
        uint32_t p = rom.u32(header + 8 + 4u * t);
        uint8_t running = 0;
        for (int steps = 0; steps < 20000 && rom.valid(p); ++steps) {
            const uint32_t at = p;
            uint8_t cmd = rom.u8(p);
            if (cmd < 0x80) {
                cmd = running;  // 러닝 스테이터스: 이 바이트는 인자
            } else {
                ++p;
                if (cmd >= 0xBD) running = cmd;
            }
            if (cmd >= 0xCF || cmd == 0xCE) {  // 음표 / EOT: 0x80 미만 인자 최대 3개
                for (int i = 0; i < 3 && rom.u8(p) < 0x80; ++i) ++p;
            } else if (cmd >= 0x80 && cmd <= 0xB0) {  // 대기
            } else if (cmd == 0xB1) {  // FINE
                break;
            } else if (cmd == 0xB2) {  // GOTO
                uint32_t target = rom.u32(p);
                if (target <= at) return true;
                break;
            } else if (cmd == 0xB3) {  // PATT
                p += 4;
            } else if (cmd == 0xB4) {  // PEND
            } else if (cmd == 0xB5) {  // REPT
                p += 5;
            } else if (cmd == 0xB9) {  // MEMACC
                uint8_t op = rom.u8(p);
                p += 3;
                if (op >= 6 && op <= 17) p += 4;
            } else if (cmd == 0xCC) {  // PORT
                p += 2;
            } else if (cmd == 0xCD) {  // XCMD
                uint8_t x = rom.u8(p++);
                if (x == 1 || x == 13) p += 4;
                else if (x == 12) p += 2;
                else if (x >= 2 && x <= 11) p += 1;
                else break;
            } else if (cmd >= 0xBA && cmd <= 0xC8) {  // 인자 1개
                p += 1;
            } else {
                break;  // 알 수 없는 명령 (원본도 FINE 처리)
            }
        }
    }
    return false;
}

bool BgmPlayerSelector::locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    reset();
    for (const auto& c : cands) {
        if (c.entries.empty()) continue;
        const MPlayEntry& e0 = c.entries[0];
        if (!isPlayerInfoValid(bus, e0.info)) continue;
        if (bus.r8(e0.info + 8) != e0.numTracks) continue;
        if (bus.r32(e0.info + 0x2C) != e0.tracks) continue;
        int maxTracks = 0;
        for (const auto& e : c.entries) maxTracks = std::max<int>(maxTracks, e.numTracks);
        const int threshold = std::max(4, (maxTracks + 1) / 2);
        for (const auto& e : c.entries) {
            if (e.numTracks >= threshold && isPlayerInfoValid(bus, e.info)) m_players.push_back(e.info);
        }
        m_state.assign(m_players.size(), State());
        return !m_players.empty();
    }
    return false;
}

bool BgmPlayerSelector::stillValid(GbaBus& bus) const {
    for (uint32_t p : m_players) {
        if (!isPlayerInfoValid(bus, p)) return false;
    }
    return !m_players.empty();
}

void BgmPlayerSelector::reset() {
    m_players.clear();
    m_state.clear();
    m_active = 0;
    m_frame = 0;
    m_loopCache.clear();
}

uint32_t BgmPlayerSelector::update(GbaBus& bus, const Rom& rom, BgmSnapshot& snap, bool& changed) {
    ++m_frame;
    changed = false;
    uint32_t best = 0;
    uint64_t bestStart = 0;
    bool activePlaying = false;
    BgmSnapshot activeSnap, bestSnap;
    for (size_t i = 0; i < m_players.size(); ++i) {
        BgmSnapshot s = readBgm(bus, m_players[i]);
        State& st = m_state[i];
        if (s.header != st.header || s.clock < st.clock) st.startedAt = m_frame;  // 곡 시작/재시작
        st.header = s.header;
        st.clock = s.clock;
        const bool playing = s.header && !(s.status & 0x80000000u) && (s.status & 0xFFFFu);
        if (m_players[i] == m_active) {
            activePlaying = playing;
            activeSnap = s;
        }
        bool looping = false;
        if (playing) {
            auto it = m_loopCache.find(s.header);
            if (it == m_loopCache.end()) it = m_loopCache.emplace(s.header, isLoopingSong(rom, s.header)).first;
            looping = it->second;
        }
        if (playing && looping && st.startedAt >= bestStart) {
            best = m_players[i];
            bestStart = st.startedAt;
            bestSnap = s;
        }
    }
    // 재생 중인 플레이어 중 가장 최근에 시작한 쪽을 따른다. 아무것도 재생 중이 아니면 현재 플레이어 유지
    if (best && best != m_active) {
        uint64_t activeStart = 0;
        for (size_t i = 0; i < m_players.size(); ++i) {
            if (m_players[i] == m_active) activeStart = m_state[i].startedAt;
        }
        if (!activePlaying || bestStart > activeStart) {
            m_active = best;
            changed = true;
            snap = bestSnap;
            return m_active;
        }
    }
    if (!m_active && !m_players.empty()) {
        m_active = m_players[0];
        changed = true;
        activeSnap = readBgm(bus, m_active);
    }
    snap = activeSnap;
    return m_active;
}

void BgmMirror::reset() {
    have_ = false;
    last_ = BgmSnapshot();
}

void BgmMirror::update(const BgmSnapshot& s) {
    const bool paused = (s.status & STATUS_PAUSE) != 0;

    if (s.header == 0) {
        if (eng_.currentSong()) eng_.stop();
        last_ = s;
        have_ = true;
        return;
    }

    // 곡 변경 또는 같은 곡 재시작(MPlayStart 가 clock 을 0 으로 되돌림)
    bool restart = !have_ || s.header != last_.header || s.clock < last_.clock;
    if (restart) {
        if (!have_ && paused) {
            // 연결 시점에 이미 멈춰 있던 곡은 시작하지 않는다
        } else {
            eng_.start(s.header);
        }
    }

    if (paused) {
        bool explicitStop = (s.status & STATUS_TRACK) != 0;  // m4aMPlayStop / 임시 페이드아웃
        bool fadeEnded = have_ && last_.fadeOI != 0;          // 페이드아웃 완료
        // 트랙이 모두 끝난 자연 종료는 외부 재생기가 스스로 끝날 때까지 둔다
        if (explicitStop || fadeEnded) eng_.setPaused(true);
    } else if (eng_.paused()) {
        eng_.setPaused(false);  // MPlayContinue
    }

    last_ = s;
    have_ = true;
}

double songSeconds(const Rom& rom, uint32_t header) {
    // MP2K 대기 명령(0x80~0xB0)의 틱 수
    static const uint8_t kWait[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                    17, 18, 19, 20, 21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52,
                                    54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96};
    const uint8_t n = rom.u8(header);
    if (n == 0 || n > 16 || !rom.valid(header, 8 + 4u * n)) return 0;
    uint32_t longest = 0;
    int tempo = 0;
    for (uint8_t t = 0; t < n; ++t) {
        uint32_t p = rom.u32(header + 8 + 4u * t);
        uint8_t running = 0;
        uint32_t ticks = 0;
        for (int steps = 0; steps < 20000 && rom.valid(p); ++steps) {
            const uint32_t at = p;
            uint8_t cmd = rom.u8(p);
            if (cmd < 0x80) {
                cmd = running;
            } else {
                ++p;
                if (cmd >= 0xBD) running = cmd;
            }
            if (cmd >= 0xCF || cmd == 0xCE) {
                for (int i = 0; i < 3 && rom.u8(p) < 0x80; ++i) ++p;
            } else if (cmd >= 0x80 && cmd <= 0xB0) {
                ticks += kWait[cmd - 0x80];
            } else if (cmd == 0xB1) {
                break;
            } else if (cmd == 0xB2) {
                (void) at;
                break;  // GOTO: 반복 곡은 첫 반복 지점까지, 앞으로 건너뛰는 곡은 거기서 끝으로 본다
            } else if (cmd == 0xB3) {
                p += 4;
            } else if (cmd == 0xB4) {
            } else if (cmd == 0xB5) {
                p += 5;
            } else if (cmd == 0xB9) {
                uint8_t op = rom.u8(p);
                p += 3;
                if (op >= 6 && op <= 17) p += 4;
            } else if (cmd == 0xBB) {
                if (!tempo) tempo = rom.u8(p) * 2;  // 템포 = 인자 x 2 (분당 4분음표)
                p += 1;
            } else if (cmd == 0xCC) {
                p += 2;
            } else if (cmd == 0xCD) {
                uint8_t x = rom.u8(p++);
                if (x == 1 || x == 13) p += 4;
                else if (x == 12) p += 2;
                else if (x >= 2 && x <= 11) p += 1;
                else break;
            } else if (cmd >= 0xBA && cmd <= 0xC8) {
                p += 1;
            } else {
                break;
            }
        }
        longest = std::max(longest, ticks);
    }
    // 4분음표 = 24틱
    return longest * 60.0 / (24.0 * (tempo > 0 ? tempo : 150));
}

bool isMusicSong(const Rom& rom, uint32_t header) {
    const uint8_t n = rom.u8(header);
    if (n == 0 || n > 16) return false;
    return n >= 4 || isLoopingSong(rom, header);
}

bool BgmMultiMirror::locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    if (cands.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    players_.clear();
    for (const auto& c : cands) {
        if (c.entries.empty()) continue;
        const MPlayEntry& e0 = c.entries[0];
        if (!isPlayerInfoValid(bus, e0.info)) continue;
        if (bus.r8(e0.info + 8) != e0.numTracks) continue;
        if (bus.r32(e0.info + 0x2C) != e0.tracks) continue;
        int maxTracks = 0;
        for (const auto& e : c.entries) maxTracks = std::max<int>(maxTracks, e.numTracks);
        const int threshold = std::max(4, (maxTracks + 1) / 2);
        for (const auto& e : c.entries) {
            if (!isPlayerInfoValid(bus, e.info)) continue;
            Player p;
            p.info = e.info;
            p.se = e.numTracks < threshold;
            p.eng.reset(new Mp2kEngine(rom_));
            p.eng->configure(cfg_);
            p.mirror.reset(new BgmMirror(*p.eng));
            players_.push_back(std::move(p));
        }
        return !players_.empty();
    }
    return false;
}

bool BgmMultiMirror::located() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !players_.empty();
}

bool BgmMultiMirror::stillValid(GbaBus& bus) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& p : players_) {
        if (!isPlayerInfoValid(bus, p.info)) return false;
    }
    return !players_.empty();
}

void BgmMultiMirror::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    players_.clear();
    cutscene_ = false;
}

void BgmMultiMirror::stopAll() {
    for (auto& p : players_) {
        p.eng->stop();
        p.mirror->reset();
        p.header = 0xFFFFFFFF;
        p.music = false;
        p.kind = p.engKind = Kind_Sound;
        p.orphan = p.gameDone = false;
        p.seOn = false;
        p.seHeader = p.seClock = 0;
    }
    cutscene_ = false;
}

void BgmMultiMirror::releaseMute(GbaBus& bus) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& p : players_) {
        if (!p.music) continue;
        const BgmSnapshot s = readBgm(bus, p.info);
        if (s.header && !(s.status & STATUS_PAUSE) && (s.status & STATUS_TRACK)) restartGameSong(bus, p.info);
    }
    stopAll();
}

void BgmMultiMirror::configure(const EngineConfig& cfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    cfg_ = cfg;
    for (auto& p : players_) {
        uint32_t song = p.eng->currentSong();
        p.eng->configure(cfg);
        if (song && p.music) p.eng->start(song);
    }
}

void BgmMultiMirror::setRuntimeParams(const SoundInfoSnapshot& si) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& p : players_) {
        p.eng->setRuntimeParams(si.maxChans, si.masterVolume, si.reverb, si.samplesPerVBlank, si.dmaPeriod);
    }
}

int BgmMultiMirror::classify(uint32_t header) {
    if (!header) return Kind_Sound;
    auto it = kindCache_.find(header);
    if (it != kindCache_.end()) return it->second;
    int kind = Kind_Sound;
    if (isMusicSong(*rom_, header)) {
        if (isLoopingSong(*rom_, header)) kind = Kind_Music;
        // 반복 없는 음악: 12초 이하는 팡파레, 그보다 길면 연출 곡 (NDS·3DS 판과 같은 기준)
        else kind = songSeconds(*rom_, header) <= 12.0 ? Kind_Jingle : Kind_Cutscene;
    }
    kindCache_.emplace(header, kind);
    return kind;
}

bool BgmMultiMirror::isSfxSong(uint32_t header) {
    // 곡 머리가 롬에 있고, 음악이 아니고, 반복하지 않는 소리
    if (header < 0x08000000u || header >= 0x0A000000u || !rom_->valid(header, 8)) return false;
    const uint8_t n = rom_->u8(header);
    if (n == 0 || n > 16) return false;
    return classify(header) == Kind_Sound && !isLoopingSong(*rom_, header);
}

void BgmMultiMirror::onSeFrame(GbaBus& bus, Player& p, bool mute) {
    const BgmSnapshot s = readBgm(bus, p.info);
    if (s.header != p.header) {
        p.header = s.header;
        p.seOn = sfx1x_ && isSfxSong(s.header);
    }
    // 대체하지 않는 소리로 바뀌어도, 재생기에서 나오던 효과음은 끝까지 둔다
    if (!p.seOn) {
        p.seHeader = 0;
        return;
    }
    seStep(bus, p, s, mute);
}

// 효과음 하나를 대체 중인 플레이어의 한 프레임 (효과음용·음악용 플레이어 공통)
void BgmMultiMirror::seStep(GbaBus& bus, Player& p, const BgmSnapshot& s, bool mute) {
    if (mute) muteBgmTracks(bus, p.info);
    // 게임이 소리를 새로 냈는지: 다른 소리이거나, 같은 소리를 다시 냄(MPlayStart 가 clock 을 0 으로 되돌림)
    const bool started = s.header != p.seHeader || s.clock < p.seClock;
    p.seHeader = s.header;
    p.seClock = s.clock;
    // 재생기에서 앞의 효과음이 아직 울리는 동안 새로 낸 소리는 건너뛴다. 배속 중에는 게임이 같은 소리를
    // 아주 빨리 거듭 내므로, 매번 처음부터 다시 틀면 소리가 나기도 전에 끊겨 들리지 않는다.
    // 게임에서 먼저 끝나거나 멈춰도 재생기는 끝까지 낸다
    if (started && (!p.eng->currentSong() || p.eng->finished())) p.eng->start(s.header);
}

// 게임에서는 이미 끝났거나 지나갔는데, 재생기는 아직 1배속으로 내고 있는 팡파레
bool BgmMultiMirror::jingleLagging(const Player& p) const {
    return p.engKind == Kind_Jingle && p.eng->currentSong() && !p.eng->finished() && (p.orphan || p.gameDone);
}

bool BgmMultiMirror::anyJingleLagging(const Player* except) const {
    for (const auto& p : players_) {
        if (&p != except && jingleLagging(p)) return true;
    }
    return false;
}

void BgmMultiMirror::cutLaggingJingles(const Player* except) {
    for (auto& p : players_) {
        if (&p == except || !jingleLagging(p)) continue;
        p.eng->stop();
        p.mirror->reset();
        p.orphan = false;
        p.engKind = Kind_Sound;
    }
}

void BgmMultiMirror::onFrame(GbaBus& bus, bool mute) {
    const SoundInfoSnapshot si = readSoundInfo(bus);
    if (si.valid) setRuntimeParams(si);
    std::lock_guard<std::mutex> lk(mtx_);
    bool cutscene = false;
    for (auto& p : players_) {
        if (p.se) {
            onSeFrame(bus, p, mute);
            continue;
        }
        BgmSnapshot s = readBgm(bus, p.info);
        const bool playing = s.header && !(s.status & STATUS_PAUSE) && (s.status & STATUS_TRACK);
        if (s.header != p.header) {
            p.header = s.header;
            const int kind = classify(s.header);
            const bool music = kind == Kind_Music || kind == Kind_Jingle;
            if (!music && jingleLagging(p)) {
                // 게임은 이 플레이어로 효과음 등을 내기 시작했지만, 팡파레는 1배속으로 끝까지 낸다
                p.orphan = true;
            } else if (!music && p.music) {
                p.eng->stop();  // 음악 -> 효과음/빈 곡/연출 곡: 대체 재생을 멈춘다
                p.mirror->reset();
                p.engKind = Kind_Sound;
            }
            if (music) {
                // 새 곡이 시작된다 = 장면이 넘어갔다: 게임에서는 지나간 팡파레가 아직 나오고 있으면 끊는다
                cutLaggingJingles(&p);
                if (p.orphan) {
                    p.eng->stop();
                    p.mirror->reset();
                    p.orphan = false;
                }
                p.engKind = kind;
                p.gameDone = false;
            }
            p.kind = kind;
            p.music = music;
            // 음악용 플레이어로 내는 효과음 (예: 포켓몬 에메랄드 계열의 선택음)
            p.seOn = !music && sfx1x_ && isSfxSong(s.header);
        }
        if (p.seOn && !p.orphan) {
            seStep(bus, p, s, mute);
            continue;
        }
        p.seHeader = 0;
        if (p.kind == Kind_Cutscene && playing) cutscene = true;
        if (p.orphan) {
            if (p.eng->finished() || !p.eng->currentSong()) {
                p.eng->stop();
                p.mirror->reset();
                p.orphan = false;
                p.engKind = Kind_Sound;
            }
            continue;
        }
        if (!p.music) continue;
        if (p.kind == Kind_Jingle && !(s.status & STATUS_TRACK)) p.gameDone = true;  // 게임에서는 끝남
        if (mute) muteBgmTracks(bus, p.info);
        // 멈춰 있던 곡을 게임이 다시 이어 갈 때, 게임에서는 지나간 팡파레가 아직 나오고 있으면
        // 팡파레가 1배속으로 끝날 때까지 기다린다
        if (p.kind == Kind_Music && p.eng->paused() && !(s.status & STATUS_PAUSE) && anyJingleLagging(&p)) {
            continue;
        }
        p.mirror->update(s);
    }
    cutscene_ = cutscene;
}

void BgmMultiMirror::render(float* out, int frames) {
    std::lock_guard<std::mutex> lk(mtx_);
    const size_t n = static_cast<size_t>(frames) * 2;
    std::fill(out, out + n, 0.0f);
    if (tmp_.size() < n) tmp_.resize(n);
    for (auto& p : players_) {
        if (!p.eng->currentSong()) continue;
        p.eng->render(tmp_.data(), frames);
        // 음악·팡파레가 아닌 소리 = 효과음
        const float gain = p.engKind == Kind_Sound ? sfxGain_.load() : 1.0f;
        for (size_t i = 0; i < n; ++i) out[i] += tmp_[i] * gain;
    }
}
