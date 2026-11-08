#include "urban/ability_limits.h"
#include "urban/save_identity.h"

#include "urban/function_hooks.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_LIMITS_MENU 1
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace urban::limits
{
    namespace
    {
        // REA / rizin, 2026-11-07..08 (SLPM-65853).
        constexpr uint32_t kExpValue = 0x180238u; // sub_00180238(entry): the value an ability's experience reaches
        constexpr uint32_t kOwnValue = 0x18C1E0u; // sub_0018C1E0(entry, a, b, fatigue): the own club's value in a match
        constexpr uint32_t kExpTable = 0x5D1320u; // u16 experience per value 0-100 (table[100] = 65535)
        constexpr uint32_t kFactors = 0x5D25A0u;  // floats: sub_0018C1E0's condition factors by a / b
        constexpr uint32_t kFatigueFactors = 0x5D25B8u; // 5 floats: fatigue below 7000, 8000, 9000, 9999, and the rest
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        // The match-copy builders and their per-ability helper: the callers whose sub_00180238 takes the value byte.
        struct Range
        {
            uint32_t start, end;
        };
        constexpr Range kCopyBuilders[] = {{0x188178u, 0x1888F0u}, {0x188CA8u, 0x189440u}, {0x18C1E0u, 0x18C338u}};
        // The pitch object's cuts (sub_002B0748): address, the game's word, the word without the 99 (cut at 255 instead).
        // Keep the same as recomp/code_variants/pitch_object_no_cap.pnach: the compiled code runs the version whose
        // words memory holds, and only the two versions listed there exist.
        struct Patch
        {
            uint32_t address, game, lifted;
        };
        constexpr Patch kPitchPatches[] = {
            {0x2B0A40u, 0x28430064u, 0x28430100u}, {0x2B0A4Cu, 0x24020063u, 0x240200FFu}, // 球威 +0x64
            {0x2B0AACu, 0x28430064u, 0x28430100u}, {0x2B0AB8u, 0x24020063u, 0x240200FFu}, // 制球 +0x68
            {0x2B0AF8u, 0x28430064u, 0x28430100u}, {0x2B0B04u, 0x24020063u, 0x240200FFu}, // +0x6C
            {0x2B0BF4u, 0x28620064u, 0x28620100u}, {0x2B0C14u, 0x24030063u, 0x240300FFu}, // pitch levels (skills)
            {0x2B0D24u, 0x28620064u, 0x28620100u}, {0x2B0D44u, 0x24030063u, 0x240300FFu}, // pitch levels (per type)
        };

        uint8_t *g_rdram = nullptr;
        // Per save (owner, 11-08): cheat_saves/<save>/ability_limits.ini; empty before any game. g_pathMutex: frame (EE) and menu.
        std::filesystem::path g_settingsPath;
        std::mutex g_pathMutex;
        std::string g_profile;
        uint32_t g_generation = ~0u;
        std::atomic<bool> g_on{false};
        GuestFunction g_ownValueOriginal = nullptr;
        bool g_log = false; // URBAN_LIMITS_LOG=1: the first values past 99 a match copy takes
        std::atomic<int> g_logged{0};

        // Base runners past 99 (owner, 11-08: "치트사용한 선수들의 도루가 조금 약한것 같은데"). The runner object
        // ([0x5E8E90], sub_002B5FE8: 4 entries of 0x30) gets the player's 走力 (copy +0x88) and 走塁 (copy +0x8B) in entry
        // +0x28 / +0x29 as bytes, from three builders: sub_002B6D40 then adds the skill bonus, makes a "negative" byte
        // (128 and up) 0 and cuts at 99; sub_002B7408 and sub_002B7B00 keep the raw byte. The steal judgements
        // (sub_002C1268, sub_002C1050) read them as signed bytes. So with 99 제한 풀기 a 255 runner ran like a 0 (or a -1).
        // Each builder calls sub_002A0458 right after its two stores, with the entry in a saved register: the hook cuts the
        // two bytes at 99 there, and the game goes on as with a 99 runner (bonus, cap, steals).
        struct RunnerSite
        {
            uint32_t ra;
            int entryRegister;
        };
        constexpr RunnerSite kRunnerSites[] = {{0x2B6EF0u, 16}, {0x2B74E8u, 17}, {0x2B7CB0u, 18}}; // s0, s1, s2
        constexpr uint32_t kRunnerCall = 0x2A0458u, kRunnerSpeed = 0x28u, kRunnerRunning = 0x29u;
        GuestFunction g_runnerCallOriginal = nullptr;
        std::atomic<int> g_runnerFixes{0};

        uint16_t read16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + (address & kRamMask), sizeof(value));
            return value;
        }

        float readFloat(uint32_t address)
        {
            float value;
            std::memcpy(&value, g_rdram + (address & kRamMask), sizeof(value));
            return value;
        }

        void setReturn(R5900Context *ctx, uint32_t value)
        {
            ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        // sub_00180238 as the game has it: one less than the first value whose experience is above the entry's (+4),
        // 3 for no entry or when none is above.
        uint32_t expValue(uint32_t entry)
        {
            if (entry == 0u)
                return 3u;
            const uint16_t experience = read16(entry + 4u);
            for (uint32_t value = 0; value < 101u; ++value)
                if (experience < read16(kExpTable + 2u * value))
                    return (value - 1u) & 0xFFFFu;
            return 3u;
        }

        bool fromCopyBuilder(uint32_t ra)
        {
            for (const Range &range : kCopyBuilders)
                if (ra >= range.start && ra < range.end)
                    return true;
            return false;
        }

        // The builders' 体力 calls (the value times 100 makes the match stamina, 0-9900 in the game). Not past 99: with the
        // S+ codes' 255 the copies had 25500 and the own club changed pitchers after every batter (sim test, 2026-11-08).
        constexpr uint32_t kStaminaCalls[] = {0x1886B4u, 0x189138u}; // ra after sub_00188178 0x1886AC, sub_00188CA8 0x189130

        // The value byte (+2) when it is above 99 and above what the experience gives (switch on).
        uint32_t pastLimit(uint32_t entry, uint32_t value, uint32_t ra)
        {
            if (entry == 0u)
                return value;
            for (uint32_t call : kStaminaCalls)
                if (ra == call)
                    return value;
            const uint8_t byte = g_rdram[(entry + 2u) & kRamMask];
            if (byte <= 99u || byte <= value)
                return value;
            if (g_log && g_logged.fetch_add(1) < 40)
                std::fprintf(stderr, "[limits] match copy: entry 0x%x takes %u (experience gives %u), caller 0x%x\n", entry, byte, value, ra);
            return byte;
        }

        // sub_00180238 (a leaf): the game's own, or the value byte past 99 for the match-copy builders with the switch on.
        void expValueHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)rdram;
            (void)runtime;
            const uint32_t entry = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            uint32_t value = expValue(entry);
            if (g_on.load(std::memory_order_relaxed) && fromCopyBuilder(ra))
                value = pastLimit(entry, value, ra);
            setReturn(ctx, value);
            ctx->pc = ra;
        }

        // sub_0018C1E0: value x condition factor (a) x condition factor (b) x fatigue factor, cut at 99 and at the
        // ability's limit byte (+3), truncated. Switch on: the value past 99 (above) and the cut at 255 instead of 99,
        // the same float steps otherwise. Off: the game's function.
        void ownValueHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool on = g_on.load(std::memory_order_relaxed);
            if (!on && g_ownValueOriginal)
            {
                g_ownValueOriginal(rdram, ctx, runtime); // nothing after it
                return;
            }
            const uint32_t entry = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            const int8_t a = static_cast<int8_t>(getRegU32(ctx, 5) & 0xFFu), b = static_cast<int8_t>(getRegU32(ctx, 6) & 0xFFu);
            const uint32_t fatigue = getRegU32(ctx, 7) & 0xFFFFu;
            const uint32_t value = on ? pastLimit(entry, expValue(entry) & 0xFFu, ra) : expValue(entry) & 0xFFu;
            const float cut = on ? 255.0f : 99.0f;
            const float fa = a < 0 ? 1.0f : readFloat(kFactors + 4u * static_cast<uint32_t>(a));
            const float fb = b < 0 ? 1.0f : readFloat(kFactors + 4u * static_cast<uint32_t>(b));
            const uint32_t step = fatigue < 7000u ? 0u : fatigue < 8000u ? 1u : fatigue < 9000u ? 2u : fatigue < 9999u ? 3u : 4u;
            const float ff = readFloat(kFatigueFactors + 4u * step);
            float result = static_cast<float>(static_cast<int32_t>(value));
            result = result * fa;
            result = result * fb;
            result = result * ff;
            if (cut < result) // the game: 99.0
                result = cut;
            const float limit = static_cast<float>(entry ? rdram[(entry + 3u) & kRamMask] : 0u);
            if (limit < result)
                result = limit;
            setReturn(ctx, static_cast<uint32_t>(static_cast<int32_t>(result)) & 0xFFu);
            ctx->pc = ra;
        }

        // EE thread, every vblank: the pitch object's words as the switch wants them (again after a loaded state). Only
        // over one of the two known words, never over another cheat's.
        bool loadSetting(const std::filesystem::path &path);

        void frame()
        {
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                const std::string profile = urban::saveid::current();
                const std::filesystem::path path = urban::saveid::settingsFile(profile, "ability_limits.ini");
                {
                    std::lock_guard<std::mutex> lock(g_pathMutex);
                    g_settingsPath = path;
                    g_profile = profile;
                }
                g_on.store(!path.empty() && loadSetting(path));
                std::fprintf(stderr, "[limits] save %s: 99 제한 풀기 %s\n", profile.empty() ? "(none)" : profile.c_str(), g_on.load() ? "on" : "off");
            }
            const bool on = g_on.load(std::memory_order_relaxed);
            for (const Patch &patch : kPitchPatches)
            {
                uint32_t now;
                std::memcpy(&now, g_rdram + patch.address, sizeof(now));
                const uint32_t want = on ? patch.lifted : patch.game;
                if (now != want && (now == patch.game || now == patch.lifted))
                    std::memcpy(g_rdram + patch.address, &want, sizeof(want));
            }
        }

        bool loadSetting(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            std::string line;
            bool on = true; // no file or no key (a new save): on (owner, 11-08: "둘 다 기본 켜줘"); nolimit=0 keeps it off
            while (std::getline(file, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.rfind("nolimit=", 0) == 0)
                    on = line.substr(8) == "1";
            }
            return on;
        }

        void saveSetting(bool on)
        {
            std::filesystem::path path;
            {
                std::lock_guard<std::mutex> lock(g_pathMutex);
                path = g_settingsPath;
            }
            if (path.empty())
                return; // before any game: nothing to keep it for
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "# 99 제한 설정: 게임 메뉴(F2)의 \"99 제한\" 탭이 씁니다\n"
                 << "nolimit=" << (on ? 1 : 0) << "\n";
        }

#if defined(URBAN_LIMITS_MENU)
        void draw(const Pcsx2GsUi *ui)
        {
            {
                std::string profile;
                {
                    std::lock_guard<std::mutex> lock(g_pathMutex);
                    profile = g_profile;
                }
                ui->text_disabled(("세이브: " + urban::saveid::describe(profile) + " — 이 세이브의 설정").c_str());
            }
            int on = g_on.load() ? 1 : 0;
            if (ui->checkbox("타자·투수 능력 99 제한 풀기", &on))
            {
                g_on.store(on != 0);
                saveSetting(on != 0);
            }
            ui->text_disabled(on ? "켜짐: 다음 경기부터 (경기용 선수 사본을 만들 때와 투구 준비 때 적용)" : "꺼짐: 게임 그대로 (경기 능력은 99까지)");
            ui->separator_text("켜면 바뀌는 것");
            ui->text_wrapped("· 경기용 선수 사본: 능력 값이 99를 넘으면(선수 치트의 198·255, S+ 코드) 그 값을 그대로 씁니다. "
                             "게임은 값 대신 경험치로 계산해서 99까지만 나옵니다.");
            ui->text_wrapped("· 내 구단 선수의 컨디션 계산: 99에서 자르던 것을 255까지 둡니다 (선수의 상한 값까지는 게임 그대로).");
            ui->text_wrapped("· 투수: 구위·제구·변화구 레벨을 투구 준비 때 99에서 자르던 것을 255까지 둡니다.");
            ui->separator_text("그대로인 것");
            ui->text_wrapped("· 구속: 게임의 구속 표가 99에서 끝나서(100 이상은 가장 느린 공이 됨) 99 그대로입니다.");
            ui->text_wrapped("· 체력: 경기 체력이 값 × 100(0~9900)이라 99 그대로입니다. 255를 쓰면 투수가 타자 한 명마다 바뀌었습니다.");
            ui->text_wrapped("· 이치넨 스킬의 보너스: '이치넨' 탭의 '이치넨 스킬 99 상한 풀기'로 따로 켭니다.");
            ui->text_wrapped("· 게임 화면의 등급: 99까지만 매겨서 100 이상은 D로 보입니다.");
        }
#endif
    }

    namespace
    {
        // sub_002A0458 (EE thread): from a runner builder (by the return address), its entry's 走力 / 走塁 cut at 99 first
        // while 99 제한 풀기 is on; then the game's function (nothing after it).
        void runnerCallHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool off = std::getenv("URBAN_TEST_NO_RUNNER_FIX") != nullptr; // tests: the game's bytes as they were
            if (!off && g_on.load(std::memory_order_relaxed))
            {
                const uint32_t ra = getRegU32(ctx, 31);
                for (const RunnerSite &site : kRunnerSites)
                    if (ra == site.ra)
                    {
                        const uint32_t entry = getRegU32(ctx, site.entryRegister) & kRamMask;
                        if (entry >= 0x100000u && entry < kRamMask - 0x30u)
                            for (const uint32_t field : {kRunnerSpeed, kRunnerRunning})
                                if (rdram[entry + field] > 99u)
                                {
                                    if (g_runnerFixes.fetch_add(1) < 12)
                                        std::fprintf(stderr, "[limits] runner 0x%08x %s %u -> 99 (builder ra 0x%x)\n", entry,
                                                     field == kRunnerSpeed ? "speed" : "running", rdram[entry + field], ra);
                                    rdram[entry + field] = 99u;
                                }
                        break;
                    }
            }
            g_runnerCallOriginal(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir)
    {
        (void)runtime;
        if (!rdram)
            return;
        const char *mods = std::getenv("URBAN_MODS");
        if (mods && std::strcmp(mods, ";") == 0) // the launcher's safe start
            return;
        g_rdram = rdram;
        (void)userDir; // per save: cheat_saves/<save>/ability_limits.ini, read in frame() when a game is there
        g_on.store(false);
        if (const char *log = std::getenv("URBAN_LIMITS_LOG"))
            g_log = std::strcmp(log, "1") == 0;
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kExpValue, "experience_value", &expValueHook, ReplacementStatus::Native,
                      "99 제한: the value byte past 99 for the match copies (switch on); the game's own otherwise");
        if (hooks.replace(kOwnValue, "own_value_condition", &ownValueHook, ReplacementStatus::Native,
                          "99 제한: cut at 255 instead of 99 (switch on); off: the game's function"))
            g_ownValueOriginal = hooks.original(kOwnValue);
        if (runtime.hasFunction(kRunnerCall) &&
            hooks.replace(kRunnerCall, "runner_speed_cap", &runnerCallHook, ReplacementStatus::Native,
                          "99 제한: a base runner's 走力 / 走塁 cut at 99 (255 read as negative by the steals); off: as is"))
            g_runnerCallOriginal = hooks.original(kRunnerCall);
        ps2x::pnach::addFrameHook(frame);
#if defined(URBAN_LIMITS_MENU)
        ps2x::menu::addPanel("99 제한", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        std::fprintf(stderr, "[limits] abilities past 99 in matches: %s\n", g_on.load() ? "on (99 제한 풀기)" : "off (the game)");
    }
}
