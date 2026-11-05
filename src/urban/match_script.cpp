#include "urban/match_script.h"
#include "urban/function_hooks.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_menu.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_SCRIPT_MENU 1
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace urban::matchscript
{
    namespace
    {
        constexpr uint32_t kRealgamePtr = 0x614E88u; // realgame object ("rg"), 0x17F0 bytes
        constexpr uint32_t kMatchPtr = 0x5E783Cu;    // simgame match: +0x18 batting team, +0xD8 strikes, +0xD9 balls
        constexpr uint32_t kTeamObj0 = 0x5D1278u;    // match team objects: +0x2008 + slot*4 = player's match copy
        constexpr uint32_t kTeamObj1 = 0x5D1274u;
        constexpr uint32_t kTeamTable = 0x5D1240u;   // the 12 clubs: +0x120 roster (+4 class, +8+4k records)
        constexpr uint32_t kOwnRosterClass = 0x6659F0u;
        constexpr int kTeams = 12;

        // The guest functions hooked. The runtime may pause a guest call and resume it later from its scheduler, so a
        // hook only changes registers and memory first and then hands over to the original (nothing runs after it).
        constexpr uint32_t kSwing = 0x3299E8u;      // in sub_329F78 just before the swing decision (sign-forced swing)
        constexpr uint32_t kNoContact = 0x3298D8u;  // strike / ball / strikeout / walk of a pitch not in play
        constexpr uint32_t kPlaySetup = 0x32CF38u;  // the 3D play of a ball in play (called by sub_32BB30)
        constexpr uint32_t kStealResult = 0x32DF18u; // a steal's result (1 = safe), read when the pitch's result is set

        // The sign fields of rg that sub_329F78 obeys while +0x124 (the sign) is set: +0x128 = 1 swings, +0x12C = 1 makes
        // the swing a fair contact (else a miss), and with sign 4 +0x134 != 1 makes the ball in play a hit.
        constexpr uint32_t kSign = 0x124u, kSignSwing = 0x128u, kSignContact = 0x12Cu, kSignOut = 0x134u;

        enum class Action
        {
            None,
            HomeRun,
            Hit,
            Strikeout,
            Walk,
        };
        constexpr const char *kActionKeys[] = {"", "hr", "hit", "k", "bb"};

        struct Rule
        {
            int target = 0; // >= 0: global player ID; -1 own team, -2 opponents, -3 everyone
            bool pitcher = false;
            Action action = Action::None;
            bool operator==(const Rule &other) const
            {
                return target == other.target && pitcher == other.pitcher && action == other.action;
            }
        };

        // Stacking: an ability's match value times a multiplier, past the 99 the match copy holds.
        // Batter: the at-bat fields of rg (sub_328A50: 巧打 vs the pitcher's hand, 長打, バント, 選球眼, 走力, 走塁), which
        // the rolls of the same pitch read. Pitcher: the pitch object [gp-0x7FF0] (sub_2B0748: 球速 +0x60, 球威 +0x64,
        // 制球 +0x68, breaking ball levels +0x14+8k), which the next pitch's choice reads.
        struct Stat
        {
            const char *key;
            const char *label;
            bool pitcher;
            uint32_t offset; // rg (batter) or pitch object (pitcher); 0x14 = the 10 breaking ball levels
        };
        constexpr Stat kStats[] = {
            {"contact", "교타 (巧打)", false, 0xACu}, {"power", "장타 (長打)", false, 0xB0u}, {"speed", "주력 (走力)", false, 0xC0u},
            {"running", "주루 (走塁)", false, 0xC4u}, {"eye", "선구안 (選球眼)", false, 0xB8u}, {"bunt", "번트 (バント)", false, 0xB4u},
            {"velocity", "구속 (球速)", true, 0x60u}, {"stuff", "구위 (球威)", true, 0x64u}, {"control", "제구 (制球)", true, 0x68u},
            {"breaking", "변화구 (変化球)", true, 0x14u},
            // Swing-and-miss and hits are rolled from the batter's 選球眼 and 巧打 (sub_329F78), not from the pitcher's
            // speed or stuff: this one divides the facing batter's 巧打 and 選球眼 while the pitcher pitches.
            {"dominance", "탈삼진력 (상대 타자 교타·선구안 ÷)", true, 0u},
            // Hits mostly land in the shallow outfield, where no HR is rolled: this one moves them deeper.
            {"distance", "타구 비거리 (얕은 외야 안타 → 깊은 외야)", false, 1u},
        };
        constexpr uint32_t kDominance = 0u, kDistance = 1u; // the offsets of the two items above
        constexpr int kStatCount = static_cast<int>(sizeof(kStats) / sizeof(kStats[0]));

        struct Boost
        {
            int target = 0; // as Rule::target
            int stat = 0;   // index into kStats
            int percent = 100; // the multiplier, in percent
            int add = 0;       // added after the multiplier (any value; the result is kept within 0..9999)
        };

        struct Settings
        {
            bool enabled = false;
            bool steal = false; // the own team's steals are always safe
            int limit = 0;      // forced HR / hit / walk per half-inning, 0 = no limit (an all-HR team never makes an out)
            std::vector<Rule> rules;
            std::vector<Boost> boosts;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_path;
        std::filesystem::file_time_type g_stamp{};
        bool g_haveStamp = false;
        Settings g_settings; // game thread (the hooks); the menu keeps its own copy and writes the file

        GuestFunction g_swing = nullptr, g_noContact = nullptr, g_playSetup = nullptr, g_stealResult = nullptr;

        // The pitch being decided (all hooks run inside one sub_329F78 call on the game thread).
        Action g_action = Action::None;
        uint16_t g_batter = 0, g_pitcher = 0;
        uint32_t g_rg = 0;
        bool g_injected = false;
        uint32_t g_saved[4] = {};

        // The half-inning (match +0x14 inning, +0x18 top/bottom) and the forced HR / hit / walk results in it.
        uint32_t g_half = ~0u;
        int g_forcedInHalf = 0;

        // Stacking: the values the game set (base) and the ones written over them, per object; a value that differs
        // from what was written means the game set it again (a new at-bat or pitcher), so it becomes the new base.
        struct Stacked
        {
            uint32_t object = 0;
            std::vector<std::pair<uint32_t, uint32_t>> base;    // (address, value the game set)
            std::vector<std::pair<uint32_t, uint32_t>> written; // (address, value written)
        };
        Stacked g_batterStack, g_pitcherStack;

        uint32_t r32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + (address & 0x01FFFFFFu), 4);
            return value;
        }
        uint16_t r16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + (address & 0x01FFFFFFu), 2);
            return value;
        }
        uint8_t r8(uint32_t address) { return g_rdram[address & 0x01FFFFFFu]; }
        void w32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & 0x01FFFFFFu), &value, 4); }
        bool valid(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

        void setReg(R5900Context *ctx, int reg, uint32_t value)
        {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        // match_script.ini: enabled=0|1, steal=0|1, rule=<own|opp|all|ID>,<bat|pit>,<hr|hit|k|bb> (one line per rule)
        Settings load(const std::filesystem::path &path)
        {
            Settings settings;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            while (std::getline(file, line))
            {
                line = trim(line);
                const size_t equals = line.find('=');
                if (line.empty() || line[0] == '#' || equals == std::string::npos)
                    continue;
                const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
                std::vector<std::string> parts;
                for (size_t start = 0;;)
                {
                    const size_t comma = value.find(',', start);
                    parts.push_back(trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
                const int target = parts[0] == "own" ? -1 : parts[0] == "opp" ? -2 : parts[0] == "all" ? -3 : std::atoi(parts[0].c_str());
                if (key == "enabled")
                    settings.enabled = value == "1";
                else if (key == "steal")
                    settings.steal = value == "1";
                else if (key == "limit")
                    settings.limit = std::atoi(value.c_str());
                else if (key == "rule" && parts.size() >= 3)
                {
                    Rule rule;
                    rule.target = target;
                    rule.pitcher = parts[1] == "pit";
                    for (int a = 1; a < 5; ++a)
                        if (parts[2] == kActionKeys[a])
                            rule.action = static_cast<Action>(a);
                    if (rule.action != Action::None && rule.target >= -3)
                        settings.rules.push_back(rule);
                }
                else if (key == "boost" && parts.size() >= 3) // boost=<target>,<stat key>,<percent>
                {
                    Boost boost;
                    boost.target = target;
                    boost.stat = -1;
                    for (int s = 0; s < kStatCount; ++s)
                        if (parts[1] == kStats[s].key)
                            boost.stat = s;
                    boost.percent = std::atoi(parts[2].c_str());
                    boost.add = parts.size() >= 4 ? std::atoi(parts[3].c_str()) : 0;
                    if (boost.stat >= 0 && boost.target >= -3 && boost.percent > 0)
                        settings.boosts.push_back(boost);
                }
            }
            return settings;
        }

        void save(const Settings &settings)
        {
            const std::filesystem::path temp = g_path.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file << "# 경기 스크립트 (게임 메뉴 F2 > 경기 스크립트 탭이 씀)\r\n";
                file << "# rule=<own|opp|all|선수 ID>,<bat|pit>,<hr|hit|k|bb>  steal=1: 내 구단 도루 무조건 성공\r\n";
                file << "# limit=한 이닝에 강제하는 홈런·안타·볼넷 수  boost=<own|opp|all|선수 ID>,<능력치>,<배율 %>,<더하는 값>\r\n";
                file << "enabled=" << (settings.enabled ? 1 : 0) << "\r\n";
                file << "steal=" << (settings.steal ? 1 : 0) << "\r\n";
                file << "limit=" << settings.limit << "\r\n";
                auto targetKey = [](int target) {
                    return target == -1 ? std::string("own") : target == -2 ? std::string("opp") : target == -3 ? std::string("all") : std::to_string(target);
                };
                for (const Rule &rule : settings.rules)
                    file << "rule=" << targetKey(rule.target) << ',' << (rule.pitcher ? "pit" : "bat") << ','
                         << kActionKeys[static_cast<int>(rule.action)] << "\r\n";
                for (const Boost &boost : settings.boosts)
                    file << "boost=" << targetKey(boost.target) << ',' << kStats[boost.stat].key << ',' << boost.percent << ','
                         << boost.add << "\r\n";
            }
            std::error_code error;
            std::filesystem::rename(temp, g_path, error); // whole file at once: the game thread reads it when it changes
        }

        void reloadIfChanged()
        {
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(g_path, error);
            if (error)
            {
                g_settings = Settings{};
                g_haveStamp = false;
                return;
            }
            if (g_haveStamp && stamp == g_stamp)
                return;
            g_stamp = stamp;
            g_haveStamp = true;
            g_settings = load(g_path);
            std::fprintf(stderr, "[script] match scripts %s, %zu rules, %zu boosts, limit %d per half-inning, steals %s\n",
                         g_settings.enabled ? "on" : "off", g_settings.rules.size(), g_settings.boosts.size(), g_settings.limit,
                         g_settings.steal ? "always safe" : "as they are");
        }

        bool isOwn(uint16_t id)
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                if (!valid(object))
                    continue;
                const uint32_t roster = r32(object + 0x120u);
                if (!valid(roster) || r32(roster + 4u) != kOwnRosterClass)
                    continue;
                for (uint32_t k = 0; k < 40; ++k)
                {
                    const uint32_t record = r32(roster + 8u + 4u * k);
                    if (valid(record) && r16(record) == id)
                        return true;
                }
            }
            return false;
        }

        uint16_t batterId(uint32_t rg)
        {
            const uint32_t batter = r32(rg + 0xD4u); // the batter's match copy
            return valid(batter) ? r16(batter) : 0;
        }

        uint16_t pitcherId()
        {
            const uint32_t match = r32(kMatchPtr);
            if (!valid(match))
                return 0;
            const uint32_t fielding = r32(match + 0x18u) == 0u ? 1u : 0u;
            for (uint32_t i = 0; i < 10; ++i)
            {
                if (r8(match + 0x6Cu + fielding * 10u + i) != 0u) // fielding position 0 = pitcher
                    continue;
                const int slot = static_cast<int8_t>(r8(match + 0x1Cu + fielding * 0x14u + i * 2u));
                const uint32_t team = r32(fielding ? kTeamObj1 : kTeamObj0);
                if (slot < 0 || !valid(team))
                    return 0;
                const uint32_t record = r32(team + 0x2008u + 4u * static_cast<uint32_t>(slot));
                return valid(record) ? r16(record) : 0;
            }
            return 0;
        }

        // The batter's and the pitcher's club this pitch (own roster or not), set once per pitch by the swing hook.
        bool g_batterOwn = false, g_pitcherOwn = false;

        // A group target: -1 the own club's players, -2 the opponents in the own club's games, -3 everyone (every game,
        // the CPU clubs' games too: those are decided by the same code when the schedule moves on).
        bool groupMatches(int target, bool selfOwn, bool otherOwn)
        {
            return target == -3 || (target == -1 && selfOwn) || (target == -2 && !selfOwn && otherOwn);
        }

        Action pick(uint16_t batter, uint16_t pitcher)
        {
            const std::vector<Rule> &rules = g_settings.rules;
            auto byId = [&rules](uint16_t id, bool pitcherRule) {
                for (const Rule &rule : rules)
                    if (rule.pitcher == pitcherRule && rule.target == id)
                        return rule.action;
                return Action::None;
            };
            auto byGroup = [&rules](bool pitcherRule) {
                const bool self = pitcherRule ? g_pitcherOwn : g_batterOwn, other = pitcherRule ? g_batterOwn : g_pitcherOwn;
                for (const Rule &rule : rules)
                    if (rule.pitcher == pitcherRule && rule.target < 0 && groupMatches(rule.target, self, other))
                        return rule.action;
                return Action::None;
            };
            // A picked player's rule first, the batter before the pitcher; then the group rules.
            if (batter)
                if (const Action action = byId(batter, false); action != Action::None)
                    return action;
            if (pitcher)
                if (const Action action = byId(pitcher, true); action != Action::None)
                    return action;
            if (batter)
                if (const Action action = byGroup(false); action != Action::None)
                    return action;
            if (pitcher)
                if (const Action action = byGroup(true); action != Action::None)
                    return action;
            return Action::None;
        }

        void restoreSign()
        {
            if (!g_injected)
                return;
            w32(g_rg + kSign, g_saved[0]);
            w32(g_rg + kSignSwing, g_saved[1]);
            w32(g_rg + kSignContact, g_saved[2]);
            w32(g_rg + kSignOut, g_saved[3]);
            g_injected = false;
        }

        void log(Action action, const char *what, uint32_t from, uint32_t to)
        {
            std::fprintf(stderr, "[script] batter %u vs pitcher %u (%s): %s 0x%x -> 0x%x\n", g_batter, g_pitcher,
                         kActionKeys[static_cast<int>(action)], what, from, to);
        }

        // A stat's factor for the batter or the pitcher: a picked player's boost first, then the groups.
        struct Factor
        {
            int percent = 100;
            int add = 0;
            bool identity() const { return percent == 100 && add == 0; }
        };

        Factor boostFor(uint16_t id, int stat, bool pitcherSide)
        {
            for (const Boost &boost : g_settings.boosts)
                if (boost.stat == stat && boost.target == id)
                    return Factor{boost.percent, boost.add};
            const bool self = pitcherSide ? g_pitcherOwn : g_batterOwn, other = pitcherSide ? g_batterOwn : g_pitcherOwn;
            for (const Boost &boost : g_settings.boosts)
                if (boost.stat == stat && boost.target < 0 && groupMatches(boost.target, self, other))
                    return Factor{boost.percent, boost.add};
            return Factor{};
        }

        struct Field
        {
            uint32_t address;
            Factor factor;
        };

        // Writes base x percent + add over the fields; a field back at x1 +0 gets its base again. key is the player (and
        // object): a new one starts from the values the game set.
        void applyStack(Stacked &stack, uint32_t key, const std::vector<Field> &fields)
        {
            static const bool debug = std::getenv("URBAN_SCRIPT_DEBUG") != nullptr;
            const bool fresh = stack.object != key;
            if (fresh)
            {
                stack.object = key;
                stack.base.clear();
                stack.written.clear();
            }
            std::vector<std::pair<uint32_t, uint32_t>> base, written;
            for (const Field &field : fields)
            {
                uint32_t value = r32(field.address);
                for (size_t i = 0; i < stack.written.size(); ++i)
                    if (stack.written[i].first == field.address && stack.written[i].second == value)
                        value = stack.base[i].second; // still ours: the game's value is the base kept
                if (field.factor.identity())
                {
                    w32(field.address, value);
                    continue;
                }
                const int64_t boosted = static_cast<int64_t>(static_cast<int32_t>(value)) * field.factor.percent / 100 + field.factor.add;
                const uint32_t result = static_cast<uint32_t>(std::clamp<int64_t>(boosted, 0, 9999));
                w32(field.address, result);
                base.emplace_back(field.address, value);
                written.emplace_back(field.address, result);
            }
            // Fields boosted before and not listed now: their base goes back if nothing changed them since.
            for (size_t i = 0; i < stack.written.size(); ++i)
            {
                bool listed = false;
                for (const Field &field : fields)
                    listed = listed || field.address == stack.written[i].first;
                if (!listed && r32(stack.written[i].first) == stack.written[i].second)
                    w32(stack.written[i].first, stack.base[i].second);
            }
            if (debug && fresh && !base.empty())
            {
                std::string text;
                for (size_t i = 0; i < base.size(); ++i)
                    text += " " + std::to_string(base[i].first & 0xFFFu) + ":" + std::to_string(base[i].second) + "->" + std::to_string(written[i].second);
                std::fprintf(stderr, "[script] stack key %08x batter %u pitcher %u:%s\n", key, g_batter, g_pitcher, text.c_str());
            }
            stack.base = std::move(base);
            stack.written = std::move(written);
        }

        // 타구 비거리: the chance (0-100) that a hit to the shallow outfield (zones 18, 21, 24, in front of the
        // fielders, where sub_32BB30 rolls no HR) goes to the deep outfield of the same side instead. x N gives
        // 1 - 1/N, + C adds C points.
        int g_deepChance = 0;

        int deepChance(const Factor &factor)
        {
            const int fromFactor = factor.percent > 100 ? 100 - 10000 / factor.percent : 0;
            return std::clamp(fromFactor + factor.add, 0, 100);
        }

        void applyBoosts(R5900Context *ctx, uint32_t rg)
        {
            g_deepChance = 0;
            if (g_batterStack.written.empty() && g_pitcherStack.written.empty() && (!g_settings.enabled || g_settings.boosts.empty()))
                return;
            const bool on = g_settings.enabled && !g_settings.boosts.empty();
            // Batter: the at-bat fields of rg; the batter's match copy pointer tells at-bats apart.
            std::vector<Field> fields;
            Factor dominance;
            for (int s = 0; s < kStatCount; ++s)
                if (kStats[s].pitcher && kStats[s].offset == kDominance && on && g_pitcher)
                    dominance = boostFor(g_pitcher, s, true);
            for (int s = 0; s < kStatCount; ++s)
            {
                if (kStats[s].pitcher)
                    continue;
                Factor factor = on && g_batter ? boostFor(g_batter, s, false) : Factor{};
                if (kStats[s].offset == kDistance)
                {
                    g_deepChance = deepChance(factor);
                    continue;
                }
                if (!dominance.identity() && (kStats[s].offset == 0xACu || kStats[s].offset == 0xB8u))
                {
                    // The pitcher's 탈삼진력: the facing batter's 巧打 and 選球眼 divided by its multiplier, less its value.
                    factor.percent = std::max(1, factor.percent * 100 / std::max(1, dominance.percent));
                    factor.add -= dominance.add;
                }
                fields.push_back(Field{rg + kStats[s].offset, factor});
            }
            applyStack(g_batterStack, r32(rg + 0xD4u), fields); // the batter's match copy: a new at-bat, new values
            // Pitcher: the pitch object [gp-0x7FF0] (sub_2B02D0), read by the next pitch's choice.
            const uint32_t pitch = r32(getRegU32(ctx, 28) - 0x7FF0u);
            if (!valid(pitch))
                return;
            fields.clear();
            for (int s = 0; s < kStatCount; ++s)
            {
                if (!kStats[s].pitcher || kStats[s].offset == kDominance)
                    continue;
                const Factor factor = on && g_pitcher ? boostFor(g_pitcher, s, true) : Factor{};
                if (kStats[s].offset == 0x14u)
                {
                    for (uint32_t k = 0; k < 10; ++k)
                        if (r32(pitch + 0x10u + 8u * k) != 0u || factor.identity()) // a breaking ball's level (type at +0x10)
                            fields.push_back(Field{pitch + 0x14u + 8u * k, factor});
                }
                else
                    fields.push_back(Field{pitch + kStats[s].offset, factor});
            }
            applyStack(g_pitcherStack, pitch ^ (static_cast<uint32_t>(g_pitcher) << 24), fields);
        }

        // sub_32C690(rg, zone type, zone): called by sub_32BB30 for a hit once its zone is rolled, before the detail and
        // the HR roll read the zone from $s7. A shallow outfield zone becomes the deep one of the same side.
        GuestFunction g_hitKind = nullptr;

        void hitKindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t zone = getRegU32(ctx, 6);
            if (g_deepChance > 0 && (zone == 18u || zone == 21u || zone == 24u) && getRegU32(ctx, 23) == zone)
            {
                static uint32_t seed = 0x9E3779B9u;
                seed = seed * 1664525u + 1013904223u;
                if (static_cast<int>((seed >> 16) % 100u) < g_deepChance)
                {
                    const uint32_t deeper = zone + 1u + ((seed >> 8) & 1u); // the middle or the deep zone behind
                    setReg(ctx, 6, deeper);
                    setReg(ctx, 23, deeper);
                    static const bool debug = std::getenv("URBAN_SCRIPT_DEBUG") != nullptr;
                    if (debug)
                        std::fprintf(stderr, "[script] batter %u: hit zone %u -> %u\n", g_batter, zone, deeper);
                }
            }
            g_hitKind(rdram, ctx, runtime);
        }

        // Before the swing decision: a sign that makes the batter swing and hit (HR, hit), swing and miss (strikeout)
        // or take the pitch (walk). The sign is put back when the pitch's result is set (the two hooks below).
        void swingHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            g_action = Action::None;
            const uint32_t rg = getRegU32(ctx, 4);
            reloadIfChanged();
            if (valid(rg) && rg == r32(kRealgamePtr))
            {
                g_batter = batterId(rg);
                g_pitcher = pitcherId();
                const bool anyGroup = [] {
                    for (const Rule &rule : g_settings.rules)
                        if (rule.target < 0)
                            return true;
                    for (const Boost &boost : g_settings.boosts)
                        if (boost.target < 0)
                            return true;
                    return false;
                }();
                g_batterOwn = anyGroup && g_settings.enabled && g_batter && isOwn(g_batter);
                g_pitcherOwn = anyGroup && g_settings.enabled && g_pitcher && isOwn(g_pitcher);
                const uint32_t match = r32(kMatchPtr);
                if (valid(match))
                {
                    const uint32_t half = r32(match + 0x14u) * 2u + r32(match + 0x18u);
                    if (half != g_half)
                    {
                        g_half = half;
                        g_forcedInHalf = 0;
                    }
                }
                applyBoosts(ctx, rg);
                // URBAN_SCRIPT_STATS=1: one line per plate appearance ("pa <batter>") and per ball in play ("play <batter>
                // <detail>", HR = 0x15 / 0x1A / 0x1F), to count a player's results over simulated games.
                static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
                static uint32_t lastAtBat = 0;
                if (stats && r32(rg + 0xD4u) != lastAtBat)
                {
                    lastAtBat = r32(rg + 0xD4u);
                    std::fprintf(stderr, "[stat] pa %u vs %u\n", g_batter, g_pitcher);
                }
                if (g_settings.enabled && !g_settings.rules.empty())
                    g_action = pick(g_batter, g_pitcher);
                if (g_settings.limit > 0 && g_action != Action::None && g_action != Action::Strikeout && g_forcedInHalf >= g_settings.limit)
                    g_action = Action::None; // the half-inning has had its forced HR / hit / walk results
            }
            if (g_action != Action::None)
            {
                g_rg = rg;
                g_saved[0] = r32(rg + kSign);
                g_saved[1] = r32(rg + kSignSwing);
                g_saved[2] = r32(rg + kSignContact);
                g_saved[3] = r32(rg + kSignOut);
                g_injected = true;
                const bool swing = g_action != Action::Walk;
                const bool contact = g_action == Action::HomeRun || g_action == Action::Hit;
                w32(rg + kSign, 4u);
                w32(rg + kSignSwing, swing ? 1u : 0u);
                w32(rg + kSignContact, contact ? 1u : 0u);
                w32(rg + kSignOut, 0u);
            }
            g_swing(rdram, ctx, runtime);
        }

        void noContactHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            const Action action = g_action;
            g_action = Action::None;
            if (action == Action::Strikeout || action == Action::Walk)
            {
                const uint32_t match = r32(kMatchPtr);
                const uint32_t strikes = r8(match + 0xD8u), balls = r8(match + 0xD9u);
                const uint32_t result = action == Action::Strikeout ? (strikes >= 2u ? 6u : 1u) : (balls >= 3u ? 7u : 2u);
                if (result == 7u)
                    ++g_forcedInHalf;
                log(action, "pitch", strikes * 16u + balls, result);
                setReturnU32(ctx, result);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_noContact(rdram, ctx, runtime);
        }

        void playSetupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            const Action action = g_action;
            g_action = Action::None;
            const uint32_t rg = getRegU32(ctx, 4);
            if (action == Action::HomeRun || action == Action::Hit)
                ++g_forcedInHalf;
            if (action == Action::HomeRun || (action == Action::Hit && r32(rg + 0x1618u) != 4u))
            {
                // The center HR (zone 23, detail 0x1A) and the center single (zone 22, detail 0x16) of the game's own
                // tables, with the kind and side of a natural hit. $s7 is sub_32BB30's zone: it returns it after this
                // call (sub_32CF38 saves and restores $s7), and sub_329F78 keeps it at rg+0x74.
                const bool homeRun = action == Action::HomeRun;
                const uint32_t zone = homeRun ? 0x17u : 0x16u, detail = homeRun ? 0x1Au : 0x16u;
                const uint32_t before = r32(rg + 0x1618u) * 0x100u + r32(rg + 0x161Cu);
                setReg(ctx, 5, zone);
                setReg(ctx, 6, 1u);
                setReg(ctx, 7, 0u);
                setReg(ctx, 23, zone);
                w32(rg + 0x70u, 1u);
                w32(rg + 0x1618u, 4u);
                w32(rg + 0x161Cu, detail);
                w32(rg + 0x1620u, 0u);
                w32(rg + 0x16D4u, 0u);
                log(action, "in play", before, 0x400u + detail);
            }
            static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
            if (stats)
                std::fprintf(stderr, "[stat] play %u %x\n", batterId(rg), r32(rg + 0x161Cu));
            g_playSetup(rdram, ctx, runtime);
        }

        // A steal's result: sub_32DF88 calls this with the base the runner leaves (1 first, 2 second, 3 third).
        void stealHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_settings.steal && g_settings.enabled)
            {
                const uint32_t rg = r32(kRealgamePtr);
                const uint16_t batter = valid(rg) ? batterId(rg) : 0;
                if (batter && isOwn(batter)) // the runners are the batting team's
                {
                    std::fprintf(stderr, "[script] steal from base %u: safe (natural %d)\n", getRegU32(ctx, 5),
                                 static_cast<int8_t>(r8(r32(0x5E75F8u) + 0xEB7u)));
                    setReturnU32(ctx, 1u);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            g_stealResult(rdram, ctx, runtime);
        }

#if defined(URBAN_SCRIPT_MENU)
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스", "파이터즈", "마린즈", "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프", "요코하마"};
        constexpr const char *kBatterActions[] = {"홈런", "안타", "삼진", "볼넷"};
        constexpr const char *kPitcherActions[] = {"홈런 맞기", "안타 맞기", "삼진 잡기", "볼넷 주기"};
        constexpr const char *kTargets[] = {"내 구단 전원", "내 경기의 상대 전원", "모든 경기의 모든 선수"};

        std::mutex g_menuMutex;
        Settings g_menu; // the menu's copy (GS thread)
        int g_pickTeam = 0, g_pickKind = 0, g_groupTarget = 0, g_groupRole = 0, g_groupAction = 0;
        std::set<uint16_t> g_selected; // the checked players (global IDs), menu thread
        int g_boostTarget = 0, g_boostStat = 0;

        struct Player
        {
            uint16_t id = 0;
            bool pitcher = false;
            std::string label;
        };

        std::string nameOf(uint32_t record)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + (record & 0x01FFFFFFu) + 4u, sizeof(sjis));
            int length = 0;
            while (length < 16 && sjis[length] != '\0')
                ++length;
            wchar_t wide[16];
            const int count = length > 0 ? MultiByteToWideChar(932, 0, sjis, length, wide, 16) : 0;
            std::wstring name;
            bool gap = false;
            for (int i = 0; i < count; ++i)
            {
                if (wide[i] == L' ' || wide[i] == 0x3000)
                {
                    gap = !name.empty();
                    continue;
                }
                if (gap)
                    name += L' ';
                gap = false;
                name += wide[i];
            }
            char text[96];
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), text, sizeof(text) - 1, nullptr, nullptr);
            return std::string(text, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
#else
            (void)record;
            return {};
#endif
        }

        int ownTeam()
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
                if (valid(roster) && r32(roster + 4u) == kOwnRosterClass)
                    return team;
            }
            return -1;
        }

        std::vector<Player> playersOf(int team)
        {
            std::vector<Player> players;
            const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
            const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
            if (!valid(roster))
                return players;
            for (uint32_t k = 0; k < 40; ++k)
            {
                const uint32_t record = r32(roster + 8u + 4u * k);
                if (!valid(record))
                    continue;
                Player player;
                player.id = r16(record);
                const bool pitcher = r8(record + 0x26u) != 0u;
                player.pitcher = pitcher;
                player.label = "#" + std::to_string(r8(record + 0x17u)) + " " + nameOf(record) + (pitcher ? " (투)" : "");
                players.push_back(player);
            }
            return players;
        }

        // The name of a player by global ID (any club), for the rule list.
        std::string playerName(int id)
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
                if (!valid(roster))
                    continue;
                for (uint32_t k = 0; k < 40; ++k)
                {
                    const uint32_t record = r32(roster + 8u + 4u * k);
                    if (valid(record) && r16(record) == id)
                        return nameOf(record) + " (" + kTeamNames[team] + ")";
                }
            }
            return "선수 ID " + std::to_string(id);
        }

        void draw(const Pcsx2GsUi *ui)
        {
            std::lock_guard<std::mutex> lock(g_menuMutex);
            Settings &settings = g_menu;
            bool changed = false;
            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("경기 스크립트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->text_disabled("3D 경기와 결과 표시·간이 경기(시뮬레이션)에서 고른 타자·투수의 결과를 정합니다.");
            ui->text_disabled("스윙과 화면 연출, 점수와 기록이 그 결과를 따라갑니다.");

            // The player picker (several players, any clubs), shared by the rules and the stacking below.
            const int own = ownTeam();
            std::vector<std::string> teamLabels;
            for (int team = 0; team < kTeams; ++team)
                teamLabels.push_back(std::string(kTeamNames[team]) + (team == own ? " (내 구단)" : ""));
            std::vector<const char *> teamItems;
            for (const std::string &label : teamLabels)
                teamItems.push_back(label.c_str());
            const std::vector<Player> players = playersOf(g_pickTeam);
            const bool havePlayer = !g_selected.empty();
            const std::string picked = havePlayer ? "고른 선수 " + std::to_string(g_selected.size()) + "명" : "고른 선수 (위에서 체크)";
            const char *targets[] = {kTargets[0], kTargets[1], kTargets[2], picked.c_str()};
            // A choice of the target combos: a group (one entry), or every checked player (one entry each).
            auto targetsOf = [&](int choice) {
                std::vector<int> list;
                if (choice < 3)
                    list.push_back(-(choice + 1));
                else
                    for (const uint16_t id : g_selected)
                        list.push_back(id);
                return list;
            };
            auto nameOfTarget = [](int target) { return target >= 0 ? playerName(target) : std::string(kTargets[-target - 1]); };

            ui->separator_text("선수 고르기 (여러 명 체크, 구단을 바꿔 가며 고를 수 있음 · 대상 '고른 선수 N명'에 쓰임)");
            ui->set_next_width(170.0f);
            ui->combo("##pickteam", &g_pickTeam, teamItems.data(), kTeams);
            ui->same_line();
            const char *kinds[] = {"전체", "투수만", "야수만"};
            ui->set_next_width(90.0f);
            ui->combo("##pickkind", &g_pickKind, kinds, 3);
            ui->same_line();
            if (ui->small_button("보이는 선수 모두 체크"))
                for (const Player &player : players)
                    if (g_pickKind == 0 || (g_pickKind == 1) == player.pitcher)
                        g_selected.insert(player.id);
            ui->same_line();
            if (ui->small_button("체크 모두 풀기"))
                g_selected.clear();
            if (players.empty())
                ui->text_disabled("이 구단의 선수 목록을 아직 읽을 수 없습니다 (게임을 불러온 뒤에 나옵니다).");
            else if (ui->begin_child("##pickplayers", 0.0f, 190.0f))
            {
                int column = 0;
                for (const Player &player : players)
                {
                    if (g_pickKind != 0 && (g_pickKind == 1) != player.pitcher)
                        continue;
                    if (column++ % 3 != 0)
                        ui->same_line();
                    ui->push_id(player.id);
                    int on = g_selected.count(player.id) ? 1 : 0;
                    ui->set_next_width(0.0f);
                    if (ui->checkbox(player.label.c_str(), &on))
                    {
                        if (on)
                            g_selected.insert(player.id);
                        else
                            g_selected.erase(player.id);
                    }
                    ui->pop_id();
                }
                ui->end_child();
            }
            else
                ui->end_child();
            if (havePlayer)
            {
                std::string names;
                int shown = 0;
                for (const uint16_t id : g_selected)
                {
                    if (shown++ == 8)
                    {
                        names += " 외 " + std::to_string(g_selected.size() - 8) + "명";
                        break;
                    }
                    names += (names.empty() ? "" : ", ") + playerName(id);
                }
                ui->text_wrapped(("고른 선수 " + std::to_string(g_selected.size()) + "명: " + names).c_str());
            }

            // Forced results
            ui->separator_text("결과 강제 (먼저 맞는 것: 고른 선수 타자 > 고른 선수 투수 > 전체 타자 > 전체 투수)");
            if (settings.rules.empty())
                ui->text_disabled("규칙이 없습니다.");
            int remove = -1;
            for (size_t i = 0; i < settings.rules.size(); ++i)
            {
                const Rule &rule = settings.rules[i];
                const char *what = (rule.pitcher ? kPitcherActions : kBatterActions)[static_cast<int>(rule.action) - 1];
                ui->push_id(static_cast<int>(i));
                if (ui->small_button("빼기"))
                    remove = static_cast<int>(i);
                ui->same_line();
                ui->text((nameOfTarget(rule.target) + " · " + (rule.pitcher ? "투수일 때 " : "타자일 때 ") + what).c_str());
                ui->pop_id();
            }
            if (remove >= 0)
            {
                settings.rules.erase(settings.rules.begin() + remove);
                changed = true;
            }
            const char *roles[] = {"타자", "투수"};
            ui->set_next_width(200.0f);
            ui->combo("##ruletarget", &g_groupTarget, targets, havePlayer ? 4 : 3);
            ui->same_line();
            ui->set_next_width(70.0f);
            ui->combo("##rulerole", &g_groupRole, roles, 2);
            ui->same_line();
            ui->set_next_width(110.0f);
            ui->combo("##ruleaction", &g_groupAction, g_groupRole ? kPitcherActions : kBatterActions, 4);
            ui->same_line();
            if (ui->button("더하기##rule") && (g_groupTarget < 3 || havePlayer))
            {
                for (const int target : targetsOf(g_groupTarget))
                {
                    const Rule rule{target, g_groupRole == 1, static_cast<Action>(g_groupAction + 1)};
                    bool replaced = false;
                    for (Rule &existing : settings.rules)
                        if (existing.target == rule.target && existing.pitcher == rule.pitcher)
                        {
                            existing.action = rule.action; // one result per target and role
                            replaced = true;
                        }
                    if (!replaced)
                        settings.rules.push_back(rule);
                }
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("규칙 모두 빼기") && !settings.rules.empty())
            {
                settings.rules.clear();
                changed = true;
            }
            ui->text(settings.limit > 0 ? ("한 이닝에 강제하는 홈런·안타·볼넷: " + std::to_string(settings.limit) + "번까지").c_str()
                                        : "한 이닝에 강제하는 홈런·안타·볼넷: 제한 없음");
            ui->same_line();
            if (ui->small_button("제한 없음##limit"))
            {
                settings.limit = 0;
                changed = true;
            }
            for (int step : {-5, -1, 1, 5})
            {
                ui->same_line();
                const std::string label = (step > 0 ? "+" : "") + std::to_string(step) + "##limit";
                if (ui->small_button(label.c_str()))
                {
                    settings.limit = std::clamp(settings.limit + step, 0, 99);
                    changed = true;
                }
            }
            ui->text_disabled("제한 없음일 때 팀 전체에 홈런·안타·볼넷을 걸면 아웃이 나오지 않아 이닝이 끝나지 않습니다(게임이 멈춤).");
            ui->text_disabled("횟수를 정하면 그 이닝에 그만큼만 강제하고 나머지는 원래대로 진행합니다. 삼진은 세지 않습니다.");

            ui->separator_text("도루");
            int steal = settings.steal ? 1 : 0;
            if (ui->checkbox("내 구단 도루 무조건 성공 (도루 작전을 내렸을 때)", &steal))
            {
                settings.steal = steal != 0;
                changed = true;
            }

            // Stacking
            ui->separator_text("능력치 중첩 (배율 x 더하는 값, 99를 넘어서)");
            ui->text_disabled("경기 계산에 쓰는 값 = 원래 값 x 배율 + 더하는 값. 경기용 복사본은 99에서 잘리지만 이 값은 그 뒤라 99를 넘습니다.");
            ui->text_disabled("3D 경기, 결과 표시, 다른 구단 경기(일정 진행) 모두 같습니다. 타자는 그 투구부터, 투수는 다음 투구부터 반영됩니다.");
            ui->text_disabled("교타를 아주 크게 올리면 안타는 늘지만 게임 공식상 홈런 확률은 줄어듭니다. 홈런은 장타 + 타구 비거리를 같이 올리세요.");
            int removeBoost = -1;
            for (size_t i = 0; i < settings.boosts.size(); ++i)
            {
                Boost &boost = settings.boosts[i];
                ui->push_id(1000 + static_cast<int>(i));
                if (ui->small_button("빼기"))
                    removeBoost = static_cast<int>(i);
                ui->same_line();
                char factor[96];
                const uint32_t offset = kStats[boost.stat].offset;
                if (offset == kDistance && !kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d (깊은 외야로 갈 확률 %d%%)", boost.percent / 100, (boost.percent % 100) / 10,
                                  boost.add, deepChance(Factor{boost.percent, boost.add}));
                else if (offset == kDominance && kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " 상대 교타·선구안 ÷%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, -boost.add);
                else
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, boost.add);
                ui->text((nameOfTarget(boost.target) + " · " + kStats[boost.stat].label + factor).c_str());
                // The multiplier in steps of 0.1 and 1 (0.1 .. 100), the added value in steps of 1, 10 and 100.
                ui->text_disabled("    배율");
                for (const int step : {-100, -10, 10, 100})
                {
                    ui->same_line();
                    char label[32];
                    std::snprintf(label, sizeof(label), "%s%d.%d##m%d", step > 0 ? "+" : "-", std::abs(step) / 100, (std::abs(step) % 100) / 10, step);
                    if (ui->small_button(label))
                    {
                        boost.percent = std::clamp(boost.percent + step, 10, 10000);
                        changed = true;
                    }
                }
                ui->same_line();
                ui->text_disabled("  더하기");
                for (const int step : {-100, -10, -1, 1, 10, 100})
                {
                    ui->same_line();
                    char label[32];
                    std::snprintf(label, sizeof(label), "%+d##a%d", step, step);
                    if (ui->small_button(label))
                    {
                        boost.add = std::clamp(boost.add + step, -9999, 9999);
                        changed = true;
                    }
                }
                ui->pop_id();
            }
            if (removeBoost >= 0)
            {
                settings.boosts.erase(settings.boosts.begin() + removeBoost);
                changed = true;
            }
            std::vector<const char *> statItems;
            for (const Stat &stat : kStats)
                statItems.push_back(stat.label);
            ui->set_next_width(200.0f);
            ui->combo("##boosttarget", &g_boostTarget, targets, havePlayer ? 4 : 3);
            ui->same_line();
            ui->set_next_width(180.0f);
            ui->combo("##booststat", &g_boostStat, statItems.data(), kStatCount);
            ui->same_line();
            if (ui->button("더하기 (x2.0 +0, 위에서 조절)##boost") && (g_boostTarget < 3 || havePlayer))
            {
                for (const int target : targetsOf(g_boostTarget))
                {
                    const Boost boost{target, g_boostStat, 200, 0};
                    bool exists = false;
                    for (const Boost &existing : settings.boosts)
                        exists = exists || (existing.target == boost.target && existing.stat == boost.stat);
                    if (!exists)
                    {
                        settings.boosts.push_back(boost);
                        changed = true;
                    }
                }
            }
            ui->same_line();
            if (ui->small_button("중첩 모두 빼기") && !settings.boosts.empty())
            {
                settings.boosts.clear();
                changed = true;
            }
            if (changed)
                save(settings);
        }
#endif
    }

    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram || !runtime.hasFunction(kSwing) || !runtime.hasFunction(kNoContact) || !runtime.hasFunction(kPlaySetup) ||
            !runtime.hasFunction(kStealResult))
            return;
        const char *mods = std::getenv("URBAN_MODS");
        if (mods && std::strcmp(mods, ";") == 0) // the launcher's safe start
            return;
        g_rdram = rdram;
        g_path = userDir / "match_script.ini";
        reloadIfChanged();
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kSwing, "match_script_swing", &swingHook, ReplacementStatus::Native, "match scripts: forced results");
        hooks.replace(kNoContact, "match_script_no_contact", &noContactHook, ReplacementStatus::Native, "match scripts");
        hooks.replace(kPlaySetup, "match_script_play_setup", &playSetupHook, ReplacementStatus::Native, "match scripts");
        hooks.replace(kStealResult, "match_script_steal", &stealHook, ReplacementStatus::Native, "match scripts: steals");
        if (runtime.hasFunction(0x32C690u))
        {
            hooks.replace(0x32C690u, "match_script_hit_zone", &hitKindHook, ReplacementStatus::Native, "match scripts: 타구 비거리");
            g_hitKind = hooks.original(0x32C690u);
        }
        g_swing = hooks.original(kSwing);
        g_noContact = hooks.original(kNoContact);
        g_playSetup = hooks.original(kPlaySetup);
        g_stealResult = hooks.original(kStealResult);
        if (!g_swing || !g_noContact || !g_playSetup || !g_stealResult || !g_hitKind)
        {
            std::fprintf(stderr, "[script] hooks not installed\n");
            return;
        }
#if defined(URBAN_SCRIPT_MENU)
        g_menu = g_settings;
        ps2x::menu::addPanel("경기 스크립트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
