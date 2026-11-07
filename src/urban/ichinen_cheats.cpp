#include "urban/ichinen_cheats.h"

#include "urban/function_hooks.h"
#include "urban/skill_table.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_ICHINEN_MENU 1
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
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace urban::ichinen
{
    namespace
    {
        // イチネンモード (SLPM-65853, 2026-11-05 from the owner's quick save at the shop; sub_3A1E98 buys, sub_3A2018
        // checks, sub_3A22B8 names): the state object and the skill-table manager exist only in this mode.
        constexpr uint32_t kStatePointer = 0x62B45Cu, kManagerPointer = 0x62B580u;
        constexpr uint32_t kPoints = 0x8u; // state + 8: the points (the game holds them at 99999 at most)
        constexpr uint32_t kCatalog = 0x14u, kCatalogCount = 0x18u, kAbilityNames = 0x24u, kConditionNames = 0x3Cu;
        constexpr uint32_t kLineup = 0x40u, kBoughtA = 0xBCu, kBoughtB = 0xC0u, kLineupCount = 0xC4u;
        constexpr uint32_t kSkillOwned = 0x102u, kConditionOwned = 0x16Eu;
        constexpr uint32_t kManagerSize = 0x178u;
        constexpr int kMaxSlots = 31, kMaxCatalog = 600, kAbilities = 54, kSkills = 108, kConditions = 7;
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr uint32_t kTeamTable = 0x5D1240u;
        constexpr long long kMaxPoints = 99999;

        // What an Ichinen-made skill does in a match (REA / rizin, 2026-11-07). An Ichinen run ends in a 0x4A-byte record, a
        // skill with an ID from 500 up, kept by the own-data object [0x5D12B8] at +0xC8 (8 records; sub_0015E740 looks in the
        // match's copy 0x19396E0 + side * 0x250 instead while [0x1939B80] holds a side). sub_00188CA8, the own club's
        // match-copy builder, calls sub_00160BC0(object, copy, ID) for each skill ID >= 500 of the player. That adds the record's
        // ability bytes to the copy as signed bytes and cuts every sum at 99 (movz with 99, word 0x00E3100A at 0x160C48), the 18
        // pitch-type levels too; only the stamina maximum (copy +0x64, 体力 x 100) takes its bonus uncut. With 노캡 the hook
        // makes the same adds with the bonus as 0..255 and stops at the byte's 255 (stamina at 32767). Batters' values above
        // 99 go through the at-bat setup as they are (sub_002C0578 / sub_002C0AB0 cut nothing); the pitch object
        // (sub_002B0748) cuts a pitcher's 球速 / 球威 / 制球 and pitch levels at 99 again.
        constexpr uint32_t kBonusFunction = 0x160BC0u, kOwnData = 0x5D12B8u, kRecords = 0xC8u, kRecordSize = 0x4Au;
        constexpr uint32_t kMatchSide = 0x1939B80u, kMatchRecords = 0x19396E0u, kMatchRecordsSize = 0x250u;
        constexpr uint32_t kPitchTypes = 0x679AA8u; // the pitch types of the record's 18 level bonuses (+0x32..)
        constexpr uint32_t kCopyStamina = 0x64u, kRecordStamina = 0x2Eu, kRecordPitches = 0x32u;
        constexpr uint32_t kCopyPitches = 0x6Cu; // 10 pairs (type, level)
        constexpr uint16_t kTestSkill = 500u;
        constexpr uint32_t kPlayerSkills = 0x44u; // a player record's 8 skill IDs (u16)
        constexpr uint32_t kAbilityTable = 0x6C5230u; // the Ichinen ability ID of the record's bytes +0x24.. (32 u32)
        constexpr uint32_t kRecordName = 0x02u, kRecordNameSize = 0x14u, kRecordSkills = 0x16u, kRecordBonus = 0x24u;
        constexpr uint32_t kOwnRosterClass = 0x6659F0u; // the own team's roster class (roster +4)
        // 作成スキル一覧: the 16 skills Ichinen runs made, kept in the system file (sub_002ECF90 copies these 0x4A0 bytes into
        // the save buffer, sub_002ED048 back). +0 is the list number 1-16 (sub_0039CEC0), 0 an empty slot (sub_003A94B0);
        // +0x49 the icon kind (7 for empty, sub_003A8DD0). The main game copies one into the club's 8 with an ID from 500
        // (sub_00161268); camp teaches it to a player (web: キャンプで…オリジナルスキルを伝承).
        constexpr uint32_t kCreatedList = 0x1939240u;
        constexpr int kCreatedCount = 16;
        // (record offset, copy offset) of the abilities in the game's order; +0x2D goes to two fields (the game's own code).
        constexpr std::array<std::pair<uint8_t, uint8_t>, 14> kBonusFields{{{0x24, 0x85}, {0x25, 0x86}, {0x26, 0x87},
                                                                            {0x27, 0x8B}, {0x28, 0x8E}, {0x29, 0x8F},
                                                                            {0x2A, 0x8A}, {0x2B, 0x8C}, {0x2C, 0x8D},
                                                                            {0x2D, 0x68}, {0x2D, 0x84}, {0x2F, 0x81},
                                                                            {0x30, 0x82}, {0x31, 0x83}}};

        // A catalog entry (16 bytes): +4 type (0 batter ability, 1 pitcher ability, 2/3 skill, 4 player, 5 special
        // player, 6 condition), +8 id (s16 for 0/1, else u32), +0xA level (0/1: shown as "＋level+1"), +0xC price.
        struct Item
        {
            uint32_t type = 0;
            int32_t id = 0;
            int16_t level = 0;
            uint32_t price = 0;
        };
        struct Slot
        {
            int item = -1; // the catalog number picked for this slot (-1: as it is)
            bool lock = false;
        };
        struct Settings
        {
            bool enabled = false; // the held items (slots, points, buyable again) are written while this is on
            long long points = -1;
            bool lockPoints = false;
            bool rebuy = false; // every slot buyable again at every frame
            bool noCap = false; // Ichinen-made skills add past the game's 99 in matches
            // The editor's bonus caps, for skills made on top of ability cheats (the owner's design, 2026-11-08): 교타 +72 and
            // 변화구 +99 unless unlocked; the other bytes go to 255.
            bool capContact = true, capPitches = true;
            int contactCap = 72, pitchCap = 99;
            // Final-value caps (2026-11-08): per player (any value > 0 wins), else the own team's default; 0 = none.
            bool finalCaps = false;
            std::array<int, kCapStats> capsOwn{};
            std::map<int, std::array<int, kCapStats>> caps;
            // 스킬 발동 100% (2026-11-08): the skill checks' chance roll always passes (their conditions still apply). Scope 0 =
            // the 7 skills inside Ichinen skills (effect slots 8-14), 1 = every slot and the staff's; all = the own team, else
            // the players listed.
            bool alwaysSkills = false;
            int alwaysScope = 0;
            // 이치넨 스킬 상시 작동 (the owner, 2026-11-08: "이치넨에서 만든 스킬이 상시 작동"): the 7 skills inside Ichinen skills
            // are checked as no condition, 100%, applied at once and kept the whole match (same players as above).
            bool ichinenForce = false;
            bool alwaysAll = true;
            std::vector<int> alwaysPlayers;
            std::map<int, Slot> slots;
        };
        struct View
        {
            bool active = false;
            long long points = 0;
            std::vector<Item> catalog;
            std::vector<int> lineup;
            uint32_t boughtA = 0;
            std::array<uint8_t, kSkills> skillOwned{};
            std::array<uint8_t, kConditions> conditionOwned{};
            std::vector<std::string> abilityJapanese, conditionJapanese;
            std::map<int32_t, std::string> players; // the catalog's players found on the rosters
        };
        struct Action
        {
            int kind; // 0 set a slot's item, 1 make a slot (-1: all) buyable again, 2 set the points
            int slot;
            long long value;
        };
        // Ichinen-made skills in the main game: the own data's 8 records (kOwnData + kRecords, kRecordSize each).
        struct SkillRecord
        {
            uint16_t id = 0;                  // +0: 0 empty, 500 up an Ichinen skill
            std::string name;                 // +2: SJIS, shown in UTF-8
            std::array<uint16_t, 7> skills{}; // +0x16: skills the player gets in a match (sub_002BDDE0 reads the low byte)
            std::array<uint8_t, 32> bonus{};  // +0x24: ability bonuses (14 used), +0x32: the 18 pitch types
        };
        struct OwnPlayer
        {
            uint16_t id = 0;
            std::string label;
            bool pitcher = false;
            std::array<uint16_t, 8> skills{};
        };
        struct SkillView
        {
            bool listActive = false;                     // the 作成スキル一覧 (any time after boot)
            std::array<SkillRecord, kCreatedCount> list;
            bool active = false;                         // the club's 8 (a loaded game)
            std::array<SkillRecord, 8> records;
            std::vector<OwnPlayer> players;        // the own team's roster
            std::array<uint32_t, 32> abilityIds{}; // the Ichinen ability ID of each bonus byte (the game's table 0x6C5230)
            // The game's skill definitions ("TBB1", [[0x5E75F8] + 0xD370] + 0x20, 30 bytes each, read by sub_002BDD70):
            // 0 id, 2 who (0 pitcher 1 batter 2 runner 3 fielder 4 staff), 3 when (the dispatcher sub_002BEA78's t0),
            // 4 condition (table 0x5E94B0, 0 = none), 5 chance %, 6 mode (0: special function 7 of table 0x5E95C0 decides),
            // 8 how long (+0xF3A), 9..14 three (effect kind for sub_002B8FF0, value) pairs.
            std::vector<std::array<int16_t, 15>> skillDefs;
        };
        void publishOwnIds(const std::vector<OwnPlayer> &players); // for the game thread's hooks (below, with the caps)
        struct SkillAction
        {
            int kind = 0; // 0 bonus byte, 1 skill, 2 create, 3 clear, 4 give to a player, 5 take from a player, 6 name,
                          // 7 bring a list skill into the club, 8 save the list to a PC file, 9 load it from the file
            int slot = 0;
            int index = 0;
            int value = 0;
            std::string text;
            bool list = false; // the 作成スキル一覧 (else the club's 8)
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_settingsPath;
        bool g_safeMode = false;
        std::mutex g_mutex;
        Settings g_settings;
        View g_view;
        std::vector<Action> g_actions;
        std::map<int, std::string> g_abilityKorean, g_conditionKorean;
        std::atomic<bool> g_noCap{false};
        GuestFunction g_bonusOriginal = nullptr;
        bool g_bonusLog = false;                    // URBAN_ICHINEN_LOG=1: a line per bonus
        // URBAN_TEST_ICHINEN="<player id>[/<player id>...],<bonus>[,contact[:n]+power[:n]]" (tests; contact / power alone get
        // the bonus or their own n; without the third field every ability byte gets the bonus)
        std::vector<int> g_testPlayers;
        int g_testBonus = 0;
        bool g_testContact = false, g_testPower = false;
        int g_testContactBonus = 0, g_testPowerBonus = 0;
        bool g_testPlaced = false;
        SkillView g_skillView;
        std::vector<SkillAction> g_skillActions;
        std::string g_skillLog; // the last edit's result, shown by the editor
        // URBAN_TEST_ICHINEN_EDIT="<slot>,<player id>[/<player id>...],<byte>:<value>[+<byte>:<value>...]" (tests): the editor's
        // own actions (make the slot, set the bytes, give it to the players), queued again whenever a loaded state took them
        // away.
        int g_testEditSlot = -1;
        std::vector<int> g_testEditPlayers;
        // URBAN_TEST_CLEAR_SKILLS="<player id>/..." (tests): those players' skills off but the Ichinen ones (ID 500 up)
        std::vector<int> g_testClearSkills;
        std::vector<std::pair<int, int>> g_testEditBytes;
        // URBAN_TEST_ICHINEN_LIST="<list slot>,<player id>,<byte>:<value>[+...]" (tests): a 作成スキル一覧 skill made with the
        // editor's actions, saved to the PC file once, brought into the club and given to the player.
        int g_testListSlot = -1, g_testListPlayer = 0;
        std::vector<std::pair<int, int>> g_testListBytes;
        bool g_testListSaved = false;

        std::string trim(std::string text)
        {
            const char *spaces = " \t\r\n";
            text.erase(0, text.find_first_not_of(spaces));
            const size_t last = text.find_last_not_of(spaces);
            text.erase(last == std::string::npos ? 0u : last + 1u);
            return text;
        }

        uint32_t read32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + address, sizeof(value));
            return value;
        }

        uint16_t read16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + address, sizeof(value));
            return value;
        }

        void write32(uint32_t address, uint32_t value)
        {
            std::memcpy(g_rdram + address, &value, sizeof(value));
        }

        void write16(uint32_t address, uint16_t value)
        {
            std::memcpy(g_rdram + address, &value, sizeof(value));
        }

        bool valid(uint32_t address, uint32_t size)
        {
            return (address & 3u) == 0u && address >= 0x100000u && address < kRamSize && size <= kRamSize - address;
        }

        std::string fromSjis(const char *text, size_t size)
        {
#if defined(_WIN32)
            wchar_t wide[64];
            const int count = size > 0 ? MultiByteToWideChar(932, 0, text, static_cast<int>(std::min<size_t>(size, 60)), wide, 64) : 0;
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
            char out[192];
            const int bytes = name.empty() ? 0
                                           : WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), out,
                                                                 static_cast<int>(sizeof(out)), nullptr, nullptr);
            return std::string(out, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
#else
            (void)text;
            (void)size;
            return std::string();
#endif
        }

        std::string stringAt(uint32_t pointer, size_t limit)
        {
            if (!valid(pointer & ~3u, 4u) || pointer >= kRamSize - limit)
                return std::string();
            const char *text = reinterpret_cast<const char *>(g_rdram + pointer);
            return fromSjis(text, strnlen(text, limit));
        }

        // The manager while the mode runs with its shop loaded, else 0.
        uint32_t manager()
        {
            const uint32_t m = read32(kManagerPointer);
            if (!valid(m, kManagerSize))
                return 0u;
            const uint32_t catalog = read32(m + kCatalog), count = read32(m + kCatalogCount), slots = read32(m + kLineupCount);
            if (count == 0u || count > static_cast<uint32_t>(kMaxCatalog) || !valid(catalog, count * 16u) || slots == 0u ||
                slots > static_cast<uint32_t>(kMaxSlots))
                return 0u;
            return m;
        }

        Item itemAt(uint32_t m, uint32_t index)
        {
            const uint32_t entry = read32(m + kCatalog) + 16u * index;
            Item item;
            item.type = read32(entry + 4u);
            item.id = item.type < 2u ? static_cast<int16_t>(read16(entry + 8u)) : static_cast<int32_t>(read32(entry + 8u));
            item.level = static_cast<int16_t>(read16(entry + 0xAu));
            item.price = read32(entry + 0xCu);
            return item;
        }

        // A slot buyable again: its bought bits, and for a skill or a condition the mark that it is had (an ability item's
        // +0xCC is its effect: kept).
        void rebuy(uint32_t m, uint32_t slot)
        {
            if (slot < 32u)
            {
                write32(m + kBoughtA, read32(m + kBoughtA) & ~(1u << slot));
                write32(m + kBoughtB, read32(m + kBoughtB) & ~(1u << slot));
            }
            const int32_t index = static_cast<int32_t>(read32(m + kLineup + 4u * slot));
            if (index < 0 || index >= static_cast<int32_t>(read32(m + kCatalogCount)))
                return;
            const Item item = itemAt(m, static_cast<uint32_t>(index));
            if ((item.type == 2u || item.type == 3u) && item.id >= 0 && item.id < kSkills)
                g_rdram[m + kSkillOwned + static_cast<uint32_t>(item.id)] = 0u;
            else if (item.type == 6u && item.id >= 0 && item.id < kConditions)
                g_rdram[m + kConditionOwned + static_cast<uint32_t>(item.id)] = 0u;
        }

        void setPoints(long long points)
        {
            const uint32_t state = read32(kStatePointer);
            if (valid(state, kPoints + 4u))
                write32(state + kPoints, static_cast<uint32_t>(std::clamp<long long>(points, 0, kMaxPoints)));
        }

        // sub_0015E740 as sub_00160BC0 calls it: the record with this ID among the 8 of the object, or of the match copy of
        // a side; 0 when none.
        uint32_t findRecord(uint32_t object, int16_t id)
        {
            const int8_t side = static_cast<int8_t>(g_rdram[kMatchSide]);
            const int64_t base = side == -1 ? static_cast<int64_t>(object & 0x1FFFFFFFu) + kRecords
                                            : static_cast<int64_t>(kMatchRecords) + static_cast<int64_t>(side) * kMatchRecordsSize;
            if (base < 0x100000 || base + 8 * kRecordSize > kRamSize)
                return 0u;
            for (uint32_t k = 0; k < 8u; ++k)
            {
                const uint32_t record = static_cast<uint32_t>(base) + k * kRecordSize;
                if (static_cast<int16_t>(read16(record)) == id)
                    return record;
            }
            return 0u;
        }

        // sub_00160BC0 (EE thread, while a match copy of the own club is built): the game's own adds (cut at 99), or with
        // 노캡 the same adds up to 255. Calls no game code, so it returns to the caller itself.
        void bonusHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, 4), copy = getRegU32(ctx, 5) & 0x1FFFFFFFu;
            const int16_t id = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
            const uint32_t record = findRecord(object, id);
            const bool copyOk = copy >= 0x100000u && copy + 0x90u <= kRamSize;
            if (!g_noCap.load(std::memory_order_relaxed) || record == 0u || !copyOk)
            {
                if (g_bonusLog && record != 0u && copyOk)
                    std::fprintf(stderr, "[ichinen] skill %d (game, cut at 99): copy 0x%x 교타 %u/%u + %d/%d, 장타 %u + %d, 구속/구위/제구 %u/%u/%u + %d/%d/%d\n",
                                 id, copy, rdram[copy + 0x85u], rdram[copy + 0x86u], static_cast<int8_t>(rdram[record + 0x24u]),
                                 static_cast<int8_t>(rdram[record + 0x25u]), rdram[copy + 0x87u], static_cast<int8_t>(rdram[record + 0x26u]),
                                 rdram[copy + 0x81u], rdram[copy + 0x82u], rdram[copy + 0x83u], static_cast<int8_t>(rdram[record + 0x2Fu]),
                                 static_cast<int8_t>(rdram[record + 0x30u]), static_cast<int8_t>(rdram[record + 0x31u]));
                g_bonusOriginal(rdram, ctx, runtime); // nothing after it: the game may continue it later
                return;
            }
            const unsigned before = rdram[copy + 0x87u], beforeL = rdram[copy + 0x85u], beforeR = rdram[copy + 0x86u];
            const unsigned beforeSpeed = rdram[copy + 0x81u], beforeStuff = rdram[copy + 0x82u], beforeControl = rdram[copy + 0x83u];
            uint16_t beforeStamina;
            std::memcpy(&beforeStamina, rdram + copy + kCopyStamina, sizeof(beforeStamina));
            for (const auto &[from, to] : kBonusFields)
                rdram[copy + to] = static_cast<uint8_t>(std::min(255u, rdram[copy + to] + static_cast<unsigned>(rdram[record + from])));
            uint16_t stamina;
            std::memcpy(&stamina, rdram + copy + kCopyStamina, sizeof(stamina));
            stamina = static_cast<uint16_t>(std::min(0x7FFFu, stamina + 100u * rdram[record + kRecordStamina]));
            std::memcpy(rdram + copy + kCopyStamina, &stamina, sizeof(stamina));
            for (uint32_t k = 0; k < 10u; ++k)
                for (uint32_t t = 0; t < 18u; ++t)
                    if (rdram[copy + kCopyPitches + 2u * k] == rdram[kPitchTypes + t])
                    {
                        uint8_t &level = rdram[copy + kCopyPitches + 2u * k + 1u];
                        level = static_cast<uint8_t>(std::min(255u, level + static_cast<unsigned>(rdram[record + kRecordPitches + t])));
                    }
            if (g_bonusLog)
                std::fprintf(stderr, "[ichinen] skill %d (노캡): copy 0x%x 교타 %u/%u -> %u/%u, 장타 %u -> %u, 구속/구위/제구 %u/%u/%u -> %u/%u/%u, 체력 %u -> %u\n",
                             id, copy, beforeL, beforeR, rdram[copy + 0x85u], rdram[copy + 0x86u], before, rdram[copy + 0x87u], beforeSpeed,
                             beforeStuff, beforeControl, rdram[copy + 0x81u], rdram[copy + 0x82u], rdram[copy + 0x83u], beforeStamina, stamina);
            ctx->r[2] = _mm_set_epi64x(0, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        // A player record by ID on the 12 rosters, 0 when not found.
        uint32_t findPlayer(int id)
        {
            for (int t = 0; t < 12; ++t)
            {
                const uint32_t team = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                const uint32_t roster = valid(team, 0x124u) ? read32(team + 0x120u) : 0u;
                if (!valid(roster, 8u + 4u * 40u))
                    continue;
                for (uint32_t k = 0; k < 40u; ++k)
                {
                    const uint32_t record = read32(roster + 8u + 4u * k);
                    if (valid(record, kPlayerSkills + 16u) && read16(record) == static_cast<uint16_t>(id))
                        return record;
                }
            }
            return 0u;
        }

        // URBAN_TEST_ICHINEN (tests): while the own data and the players are found, an Ichinen record (ID 500; the bonus in
        // every ability byte, or only 교타 좌·우) in its first free record and skill 500 on the players; written again when a
        // loaded state took them away.
        void placeTestSkill()
        {
            const uint32_t object = read32(kOwnData);
            if (!valid(object, kRecords + 8u * kRecordSize))
                return;
            std::vector<uint32_t> players;
            for (const int id : g_testPlayers)
                if (const uint32_t record = findPlayer(id))
                    players.push_back(record);
            if (players.empty())
                return;
            uint32_t record = 0u;
            for (uint32_t k = 0; k < 8u && record == 0u; ++k)
            {
                const uint32_t at = object + kRecords + k * kRecordSize;
                if (read16(at) == kTestSkill || read16(at) == 0u)
                    record = at;
            }
            if (record == 0u)
            {
                if (!g_testPlaced)
                    std::fprintf(stderr, "[ichinen] test: no free Ichinen record\n");
                g_testPlaced = true;
                return;
            }
            const uint32_t check = g_testContact ? 0x24u : 0x26u; // a byte this test gives a bonus
            const int checkBonus = g_testContact ? g_testContactBonus : g_testPower ? g_testPowerBonus : g_testBonus;
            const bool recordThere = read16(record) == kTestSkill && g_rdram[record + check] == static_cast<uint8_t>(checkBonus);
            std::memset(g_rdram + record, 0, kRecordSize);
            write16(record, kTestSkill);
            if (g_testContact || g_testPower)
            {
                if (g_testContact)
                {
                    g_rdram[record + 0x24u] = static_cast<uint8_t>(g_testContactBonus); // copy +0x85 교타 좌
                    g_rdram[record + 0x25u] = static_cast<uint8_t>(g_testContactBonus); // copy +0x86 교타 우
                }
                if (g_testPower)
                    g_rdram[record + 0x26u] = static_cast<uint8_t>(g_testPowerBonus); // copy +0x87 장타
            }
            else
            {
                for (const auto &[from, to] : kBonusFields)
                    g_rdram[record + from] = static_cast<uint8_t>(g_testBonus);
                g_rdram[record + kRecordStamina] = static_cast<uint8_t>(g_testBonus);
            }
            int placed = 0;
            for (const uint32_t player : players)
            {
                uint32_t freeSlot = 0u;
                bool has = false;
                for (uint32_t k = 0; k < 8u; ++k)
                {
                    const uint16_t skill = read16(player + kPlayerSkills + 2u * k);
                    has = has || skill == kTestSkill;
                    if (skill == 0u && freeSlot == 0u)
                        freeSlot = player + kPlayerSkills + 2u * k;
                }
                if (!has && freeSlot != 0u)
                {
                    write16(freeSlot, kTestSkill);
                    ++placed;
                }
            }
            if (placed > 0 || !recordThere)
                std::fprintf(stderr, "[ichinen] test: Ichinen skill %u (%s) on %d of %zu players (record 0x%x)\n", kTestSkill,
                             (g_testContact || g_testPower
                                  ? (g_testContact ? "교타 +" + std::to_string(g_testContactBonus) : std::string()) +
                                        (g_testContact && g_testPower ? ", " : "") +
                                        (g_testPower ? "장타 +" + std::to_string(g_testPowerBonus) : std::string())
                                  : "all +" + std::to_string(g_testBonus))
                                 .c_str(),
                             placed, players.size(), record);
            g_testPlaced = true;
        }

#if defined(_WIN32)
        // UTF-8 to SJIS (the game's names), at most `limit` bytes and never half a double-byte character.
        std::string toSjis(const std::string &text, size_t limit)
        {
            wchar_t wide[64];
            const int count = text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide, 63);
            char out[128];
            const int bytes = count > 0 ? WideCharToMultiByte(932, 0, wide, count, out, static_cast<int>(sizeof(out)), nullptr, nullptr) : 0;
            const std::string sjis(out, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
            size_t keep = 0;
            for (size_t i = 0; i < sjis.size();)
            {
                const unsigned char c = static_cast<unsigned char>(sjis[i]);
                const size_t width = (c >= 0x81u && c <= 0x9Fu) || (c >= 0xE0u && c <= 0xFCu) ? 2u : 1u;
                if (i + width > limit)
                    break;
                i += width;
                keep = i;
            }
            return sjis.substr(0, keep);
        }
#else
        std::string toSjis(const std::string &, size_t)
        {
            return std::string();
        }
#endif

        uint32_t ownData()
        {
            const uint32_t object = read32(kOwnData);
            return valid(object, kRecords + 8u * kRecordSize) ? object : 0u;
        }

        uint32_t ownRoster()
        {
            for (int t = 0; t < 12; ++t)
            {
                const uint32_t team = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                const uint32_t roster = valid(team, 0x124u) ? read32(team + 0x120u) : 0u;
                if (valid(roster, 8u + 4u * 40u) && read32(roster + 4u) == kOwnRosterClass)
                    return roster;
            }
            return 0u;
        }

        uint32_t findOwnPlayer(int id)
        {
            const uint32_t roster = ownRoster();
            for (uint32_t k = 0; roster != 0u && k < 40u; ++k)
            {
                const uint32_t record = read32(roster + 8u + 4u * k);
                if (valid(record, kPlayerSkills + 16u) && read16(record) == static_cast<uint16_t>(id))
                    return record;
            }
            return 0u;
        }

        bool hasSkill(uint32_t player, uint16_t id)
        {
            for (uint32_t k = 0; k < 8u; ++k)
                if (read16(player + kPlayerSkills + 2u * k) == id)
                    return true;
            return false;
        }

        // The ID into the player's first empty skill slot (false: all 8 taken), or out of it (the slot left empty).
        bool giveSkill(uint32_t player, uint16_t id)
        {
            if (hasSkill(player, id))
                return true;
            for (uint32_t k = 0; k < 8u; ++k)
                if (read16(player + kPlayerSkills + 2u * k) == 0u)
                {
                    write16(player + kPlayerSkills + 2u * k, id);
                    return true;
                }
            return false;
        }

        void takeSkill(uint32_t player, uint16_t id)
        {
            for (uint32_t k = 0; k < 8u; ++k)
                if (read16(player + kPlayerSkills + 2u * k) == id)
                    write16(player + kPlayerSkills + 2u * k, 0u);
        }

        uint16_t nextClubId(uint32_t object)
        {
            uint16_t next = 500u; // the lowest ID from 500 no club record has, as the game's sub_00161268
            for (bool used = true; used;)
            {
                used = false;
                for (uint32_t k = 0; k < 8u && !used; ++k)
                    used = read16(object + kRecords + k * kRecordSize) == next;
                if (used)
                    ++next;
            }
            return next;
        }

        std::filesystem::path listFile()
        {
            return g_settingsPath.parent_path() / "ichinen_skills.bin";
        }

        // One editor action (EE thread, the mutex held); what to show, or nothing.
        std::string applySkillAction(uint32_t object, const SkillAction &action)
        {
            if (action.kind == 8 || action.kind == 9) // the whole 作成スキル一覧 to / from a PC file
            {
                const uint32_t bytes = static_cast<uint32_t>(kCreatedCount) * kRecordSize;
                if (action.kind == 8)
                {
                    std::ofstream file(listFile(), std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char *>(g_rdram + kCreatedList), bytes);
                    return file ? "작성 스킬 16칸을 PC 파일에 보관했습니다 (" + listFile().filename().string() + ")" : "PC 파일에 쓰지 못했습니다";
                }
                std::ifstream file(listFile(), std::ios::binary);
                std::vector<char> data(bytes);
                if (!file.read(data.data(), bytes))
                    return "PC 파일이 없거나 크기가 맞지 않습니다 (" + listFile().filename().string() + ")";
                std::memcpy(g_rdram + kCreatedList, data.data(), bytes);
                return "PC 파일에서 작성 스킬 16칸을 불러왔습니다";
            }
            const int count = action.list ? kCreatedCount : 8;
            if (action.slot < 0 || action.slot >= count)
                return std::string();
            if (!action.list && object == 0u)
                return "구단 8칸은 게임을 불러온 뒤에 고칠 수 있습니다";
            const uint32_t record = action.list ? kCreatedList + static_cast<uint32_t>(action.slot) * kRecordSize
                                                : object + kRecords + static_cast<uint32_t>(action.slot) * kRecordSize;
            const uint16_t id = read16(record);
            const std::string slot = (action.list ? "작성 스킬 " : "구단 슬롯 ") + std::to_string(action.slot + 1);
            if (action.kind == 2) // new: the list number (1-16) in the list, an ID from 500 in the club
            {
                if (id != 0u)
                    return slot + "은(는) 이미 쓰는 중입니다";
                const uint16_t newId = action.list ? static_cast<uint16_t>(action.slot + 1) : nextClubId(object);
                std::memset(g_rdram + record, 0, kRecordSize);
                write16(record, newId);
                const std::string name = toSjis("イチネン" + std::to_string(action.slot + 1), kRecordNameSize - 1u);
                std::memcpy(g_rdram + record + kRecordName, name.data(), name.size());
                return slot + ": 새 이치넨 스킬" + (action.list ? std::string() : " (ID " + std::to_string(newId) + ")");
            }
            if (id == 0u)
                return slot + "이 비어 있습니다";
            switch (action.kind)
            {
            case 0:
                g_rdram[record + kRecordBonus + static_cast<uint32_t>(std::clamp(action.index, 0, 31))] = static_cast<uint8_t>(std::clamp(action.value, 0, 255));
                return std::string();
            case 1:
                write16(record + kRecordSkills + 2u * static_cast<uint32_t>(std::clamp(action.index, 0, 6)), static_cast<uint16_t>(std::clamp(action.value, 0, 255)));
                return std::string();
            case 3:
                if (!action.list) // the club's ID off every roster's players too
                    for (int t = 0; t < 12; ++t)
                    {
                        const uint32_t team = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                        const uint32_t roster = valid(team, 0x124u) ? read32(team + 0x120u) : 0u;
                        for (uint32_t k = 0; valid(roster, 8u + 4u * 40u) && k < 40u; ++k)
                        {
                            const uint32_t player = read32(roster + 8u + 4u * k);
                            if (valid(player, kPlayerSkills + 16u))
                                takeSkill(player, id);
                        }
                    }
                std::memset(g_rdram + record, 0, kRecordSize);
                return slot + " 지움" + (action.list ? std::string() : " (ID " + std::to_string(id) + "는 선수들 스킬 칸에서도 뺐습니다)");
            case 4:
            case 5:
            {
                if (action.list)
                    return std::string();
                const uint32_t player = findOwnPlayer(action.value);
                if (player == 0u)
                    return "내 구단에서 선수 ID " + std::to_string(action.value) + "을(를) 찾지 못했습니다";
                if (action.kind == 5)
                {
                    takeSkill(player, id);
                    return slot + "을(를) " + stringAt(player + 4u, 16) + "에게서 뺐습니다";
                }
                if (!giveSkill(player, id))
                    return stringAt(player + 4u, 16) + "의 스킬 칸 8개가 다 찼습니다";
                return slot + "을(를) " + stringAt(player + 4u, 16) + "에게 넣었습니다";
            }
            case 6:
            {
                const std::string name = toSjis(action.text, kRecordNameSize - 1u);
                std::memset(g_rdram + record + kRecordName, 0, kRecordNameSize);
                std::memcpy(g_rdram + record + kRecordName, name.data(), name.size());
                return slot + " 이름을 바꿨습니다";
            }
            case 7: // a list skill into the first empty club slot, with a club ID (as the game's sub_00161268)
            {
                if (!action.list)
                    return std::string();
                if (object == 0u)
                    return "구단으로 가져오려면 게임을 불러와야 합니다";
                for (uint32_t k = 0; k < 8u; ++k)
                {
                    const uint32_t target = object + kRecords + k * kRecordSize;
                    if (read16(target) != 0u)
                        continue;
                    const uint16_t newId = nextClubId(object);
                    std::memcpy(g_rdram + target, g_rdram + record, kRecordSize);
                    write16(target, newId);
                    return slot + "을(를) 구단 슬롯 " + std::to_string(k + 1) + "에 가져왔습니다 (ID " + std::to_string(newId) + ")";
                }
                return "구단 8칸이 다 찼습니다";
            }
            default:
                return std::string();
            }
        }

        template <size_t N>
        void readRecords(uint32_t base, std::array<SkillRecord, N> &out)
        {
            for (uint32_t k = 0; k < N; ++k)
            {
                const uint32_t record = base + k * kRecordSize;
                SkillRecord &entry = out[k];
                entry.id = read16(record);
                if (entry.id == 0u)
                    continue;
                entry.name = stringAt(record + kRecordName, kRecordNameSize);
                for (uint32_t j = 0; j < 7u; ++j)
                    entry.skills[j] = read16(record + kRecordSkills + 2u * j);
                std::memcpy(entry.bonus.data(), g_rdram + record + kRecordBonus, entry.bonus.size());
            }
        }

        // EE thread, every frame (the mutex held): the editor's actions (and the test's), and every 15 frames the view.
        void skillFrame(unsigned tick)
        {
            const uint32_t object = ownData();
            if (object != 0u && !g_testClearSkills.empty() && (tick % 60u) == 0u)
                for (const int who : g_testClearSkills)
                    if (const uint32_t player = findOwnPlayer(who))
                        for (uint32_t k = 0; k < 8u; ++k)
                        {
                            const uint16_t skill = read16(player + kPlayerSkills + 2u * k);
                            if (skill != 0u && skill < 500u)
                                write16(player + kPlayerSkills + 2u * k, 0u);
                        }
            if (object != 0u && g_testEditSlot >= 0 && (tick % 60u) == 0u)
            {
                const uint32_t record = object + kRecords + static_cast<uint32_t>(g_testEditSlot) * kRecordSize;
                const uint16_t id = read16(record);
                if (id == 0u)
                    g_skillActions.push_back(SkillAction{2, g_testEditSlot, 0, 0, {}});
                for (const auto &[byte, value] : g_testEditBytes)
                    if (byte >= 100) // the record's skill slots (+0x16)
                    {
                        if (id == 0u || read16(record + kRecordSkills + 2u * static_cast<uint32_t>(byte - 100)) != static_cast<uint16_t>(value))
                            g_skillActions.push_back(SkillAction{1, g_testEditSlot, byte - 100, value, {}});
                    }
                    else if (id == 0u || g_rdram[record + kRecordBonus + static_cast<uint32_t>(byte)] != static_cast<uint8_t>(value))
                        g_skillActions.push_back(SkillAction{0, g_testEditSlot, byte, value, {}});
                for (const int who : g_testEditPlayers)
                {
                    const uint32_t player = findOwnPlayer(who);
                    if (player != 0u && (id == 0u || !hasSkill(player, id)))
                        g_skillActions.push_back(SkillAction{4, g_testEditSlot, 0, who, {}});
                }
            }
            if (g_testListSlot >= 0 && (tick % 60u) == 30u)
            {
                const uint32_t listRecord = kCreatedList + static_cast<uint32_t>(g_testListSlot) * kRecordSize;
                auto listAction = [](int kind, int slot, int index, int value) {
                    SkillAction action{kind, slot, index, value, {}};
                    action.list = true;
                    return action;
                };
                const bool listed = read16(listRecord) != 0u;
                if (!listed)
                    g_skillActions.push_back(listAction(2, g_testListSlot, 0, 0));
                for (const auto &[byte, value] : g_testListBytes)
                    if (!listed || g_rdram[listRecord + kRecordBonus + static_cast<uint32_t>(byte)] != static_cast<uint8_t>(value))
                        g_skillActions.push_back(listAction(0, g_testListSlot, byte, value));
                if (listed && !g_testListSaved)
                {
                    g_skillActions.push_back(listAction(8, 0, 0, 0));
                    g_testListSaved = true;
                }
                if (listed && object != 0u)
                {
                    int clubSlot = -1;
                    for (uint32_t k = 0; k < 8u && clubSlot < 0; ++k)
                    {
                        const uint32_t clubRecord = object + kRecords + k * kRecordSize;
                        if (read16(clubRecord) != 0u && std::memcmp(g_rdram + clubRecord + kRecordName, g_rdram + listRecord + kRecordName, kRecordNameSize) == 0)
                            clubSlot = static_cast<int>(k);
                    }
                    if (clubSlot < 0)
                        g_skillActions.push_back(listAction(7, g_testListSlot, 0, 0));
                    else
                    {
                        const uint16_t clubId = read16(object + kRecords + static_cast<uint32_t>(clubSlot) * kRecordSize);
                        const uint32_t player = findOwnPlayer(g_testListPlayer);
                        if (player != 0u && !hasSkill(player, clubId))
                            g_skillActions.push_back(SkillAction{4, clubSlot, 0, g_testListPlayer, {}});
                    }
                }
            }
            const bool acted = !g_skillActions.empty();
            for (const SkillAction &action : g_skillActions)
            {
                const std::string result = applySkillAction(object, action);
                if (!result.empty())
                {
                    g_skillLog = result;
                    std::fprintf(stderr, "[ichinen] skill editor: %s\n", result.c_str());
                }
            }
            g_skillActions.clear();
            if ((tick % 15u) != 0u && !acted && g_skillView.listActive)
                return;
            SkillView view;
            view.listActive = true;
            readRecords(kCreatedList, view.list);
            for (uint32_t i = 0; i < 32u; ++i)
                view.abilityIds[i] = read32(kAbilityTable + 4u * i);
            if (const uint32_t holder = read32(0x5E75F8u); valid(holder, 0xD374u)) // the match skill object (sub_00292950)
                if (const uint32_t table = read32(holder + 0xD370u); valid(table, 0x20u + 118u * 30u) && std::memcmp(g_rdram + table, "TBB1", 4) == 0)
                    for (uint32_t i = 0; i < 118u; ++i)
                    {
                        std::array<int16_t, 15> fields{};
                        std::memcpy(fields.data(), g_rdram + table + 0x20u + 30u * i, sizeof(fields));
                        if (fields[0] <= 0)
                            break;
                        view.skillDefs.push_back(fields);
                    }
            view.active = object != 0u;
            if (object != 0u)
                readRecords(object + kRecords, view.records);
            if (const uint32_t roster = object != 0u ? ownRoster() : 0u)
                for (uint32_t k = 0; k < 40u; ++k)
                {
                    const uint32_t record = read32(roster + 8u + 4u * k);
                    if (!valid(record, kPlayerSkills + 16u) || read16(record) == 0u)
                        continue;
                    OwnPlayer player;
                    player.id = read16(record);
                    player.pitcher = g_rdram[record + 0x26u] != 0u;
                    player.label = "#" + std::to_string(g_rdram[record + 0x17u]) + " " + stringAt(record + 4u, 16) + (player.pitcher ? " (투)" : "");
                    for (uint32_t j = 0; j < 8u; ++j)
                        player.skills[j] = read16(record + kPlayerSkills + 2u * j);
                    view.players.push_back(player);
                }
            publishOwnIds(view.players);
            g_skillView = std::move(view);
        }

        // EE thread, at every vblank after the .pnach patches: the menu's one-off writes, the held ones, and every 15
        // frames what the menu shows.
        void frame()
        {
            static unsigned tick = 0u;
            std::lock_guard<std::mutex> lock(g_mutex);
            static unsigned testTick = 0u;
            if (!g_testPlayers.empty() && (++testTick % 30u) == 0u)
                placeTestSkill();
            static unsigned skillTick = 0u;
            skillFrame(++skillTick); // the own data's Ichinen skills: any mode
            const uint32_t m = manager();
            if (m == 0u)
            {
                g_actions.clear();
                if (g_view.active)
                    g_view = View{};
                return;
            }
            const uint32_t slots = read32(m + kLineupCount), count = read32(m + kCatalogCount);
            for (const Action &action : g_actions)
            {
                if (action.kind == 0)
                {
                    if (action.slot >= 0 && static_cast<uint32_t>(action.slot) < slots && action.value >= 0 &&
                        action.value < static_cast<long long>(count))
                        write32(m + kLineup + 4u * static_cast<uint32_t>(action.slot), static_cast<uint32_t>(action.value));
                }
                else if (action.kind == 1)
                {
                    for (uint32_t k = 0; k < slots; ++k)
                        if (action.slot < 0 || static_cast<uint32_t>(action.slot) == k)
                            rebuy(m, k);
                }
                else if (action.kind == 2)
                    setPoints(action.value);
            }
            const bool acted = !g_actions.empty();
            g_actions.clear();
            if (g_settings.enabled)
            {
                for (const auto &[k, slot] : g_settings.slots)
                    if (slot.lock && slot.item >= 0 && static_cast<uint32_t>(k) < slots && static_cast<uint32_t>(slot.item) < count)
                        write32(m + kLineup + 4u * static_cast<uint32_t>(k), static_cast<uint32_t>(slot.item));
                if (g_settings.rebuy)
                    for (uint32_t k = 0; k < slots; ++k)
                        rebuy(m, k);
                if (g_settings.lockPoints && g_settings.points >= 0)
                    setPoints(g_settings.points);
            }
            if ((++tick % 15u) != 0u && !acted && g_view.active)
                return;
            View view;
            view.active = true;
            const uint32_t state = read32(kStatePointer);
            view.points = valid(state, kPoints + 4u) ? static_cast<long long>(read32(state + kPoints)) : 0;
            for (uint32_t i = 0; i < count; ++i)
                view.catalog.push_back(itemAt(m, i));
            for (uint32_t k = 0; k < slots; ++k)
                view.lineup.push_back(static_cast<int32_t>(read32(m + kLineup + 4u * k)));
            view.boughtA = read32(m + kBoughtA);
            std::memcpy(view.skillOwned.data(), g_rdram + m + kSkillOwned, view.skillOwned.size());
            std::memcpy(view.conditionOwned.data(), g_rdram + m + kConditionOwned, view.conditionOwned.size());
            const uint32_t abilities = read32(m + kAbilityNames), conditions = read32(m + kConditionNames);
            for (int i = 0; i < kAbilities; ++i)
                view.abilityJapanese.push_back(valid(abilities, 4u * kAbilities) ? stringAt(read32(abilities + 4u * static_cast<uint32_t>(i)), 48) : "");
            for (int i = 0; i < kConditions; ++i)
                view.conditionJapanese.push_back(valid(conditions, 4u * kConditions) ? stringAt(read32(conditions + 4u * static_cast<uint32_t>(i)), 48) : "");
            // The players of the catalog (types 4 and 5) by their ID on the 12 rosters.
            std::map<int32_t, bool> wanted;
            for (const Item &item : view.catalog)
                if (item.type == 4u || item.type == 5u)
                    wanted[item.id] = true;
            for (int t = 0; t < 12 && !wanted.empty(); ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, 0x124u))
                    continue;
                const uint32_t roster = read32(object + 0x120u);
                if (!valid(roster, 8u + 4u * 40u))
                    continue;
                for (uint32_t k = 0; k < 40u; ++k)
                {
                    const uint32_t record = read32(roster + 8u + 4u * k);
                    if (!valid(record, 0x20u))
                        continue;
                    const int32_t id = read16(record);
                    if (id != 0 && wanted.count(id))
                        view.players[id] = stringAt(record + 4u, 16);
                }
            }
            g_view = std::move(view);
        }

        void loadKorean(const std::filesystem::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                std::vector<std::string> fields;
                std::istringstream stream(line);
                std::string field;
                while (std::getline(stream, field, '\t'))
                    fields.push_back(trim(field));
                if (fields.size() < 3u)
                    continue;
                if (fields[0] == "ability")
                    g_abilityKorean[std::atoi(fields[1].c_str())] = fields[2];
                else if (fields[0] == "condition")
                    g_conditionKorean[std::atoi(fields[1].c_str())] = fields[2];
            }
        }

        // ichinen_cheats.ini: enabled=, points=, lockpoints=, rebuy=, nocap=, slot=<slot>,<catalog number>,<lock>
        constexpr const char *kCapKeys[kCapStats] = {"contact", "power", "speed", "running", "eye", "bunt",
                                                     "velocity", "stuff", "control", "breaking", "mental"};
        constexpr const char *kCapLabels[kCapStats] = {"교타", "장타", "주력", "주루", "선구안", "번트",
                                                       "구속", "구위", "제구", "변화구", "정신력"};

        // "contact:200+power:255" <-> the caps
        std::array<int, kCapStats> capsOf(const std::string &text)
        {
            std::array<int, kCapStats> caps{};
            std::istringstream items(text);
            std::string item;
            while (std::getline(items, item, '+'))
            {
                const size_t colon = item.find(':');
                if (colon == std::string::npos)
                    continue;
                const std::string key = trim(item.substr(0, colon));
                for (int s = 0; s < kCapStats; ++s)
                    if (key == kCapKeys[s])
                        caps[static_cast<size_t>(s)] = std::clamp(std::atoi(item.c_str() + colon + 1), 0, 9999);
            }
            return caps;
        }

        std::string capText(const std::array<int, kCapStats> &caps)
        {
            std::string text;
            for (int s = 0; s < kCapStats; ++s)
                if (caps[static_cast<size_t>(s)] > 0)
                    text += (text.empty() ? "" : "+") + std::string(kCapKeys[s]) + ":" + std::to_string(caps[static_cast<size_t>(s)]);
            return text;
        }

        // The caps as the game thread reads them (match_script's hook), apart from g_mutex.
        std::mutex g_capMutex;
        bool g_capsOn = false;
        std::array<int, kCapStats> g_capsOwnNow{};
        std::map<int, std::array<int, kCapStats>> g_capsNow;
        struct SeenFinal
        {
            int before = -1, after = -1;
        };
        std::map<int, std::array<SeenFinal, kCapStats>> g_seenFinal; // the last values before / after the cap
        bool g_alwaysOn = false, g_alwaysAllNow = true, g_ichinenForceNow = false;
        int g_alwaysScopeNow = 0;
        std::vector<int> g_alwaysPlayersNow;
        std::vector<uint16_t> g_ownIdsNow; // the own roster (skillFrame)

        void publishCaps(const Settings &settings)
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_capsOn = settings.finalCaps;
            g_capsOwnNow = settings.capsOwn;
            g_capsNow = settings.caps;
            g_alwaysOn = settings.alwaysSkills;
            g_ichinenForceNow = settings.ichinenForce;
            g_alwaysScopeNow = settings.alwaysScope;
            g_alwaysAllNow = settings.alwaysAll;
            g_alwaysPlayersNow = settings.alwaysPlayers;
        }

        void publishOwnIds(const std::vector<OwnPlayer> &players)
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_ownIdsNow.clear();
            for (const OwnPlayer &player : players)
                g_ownIdsNow.push_back(player.id);
        }

        Settings loadSettings(const std::filesystem::path &path)
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
                if (key == "enabled")
                    settings.enabled = value == "1";
                else if (key == "points")
                    settings.points = std::atoll(value.c_str());
                else if (key == "lockpoints")
                    settings.lockPoints = value == "1";
                else if (key == "rebuy")
                    settings.rebuy = value == "1";
                else if (key == "nocap")
                    settings.noCap = value == "1";
                else if (key == "capcontact")
                    settings.capContact = value != "0";
                else if (key == "contactcap")
                    settings.contactCap = std::clamp(std::atoi(value.c_str()), 0, 255);
                else if (key == "cappitches")
                    settings.capPitches = value != "0";
                else if (key == "pitchcap")
                    settings.pitchCap = std::clamp(std::atoi(value.c_str()), 0, 255);
                else if (key == "finalcaps")
                    settings.finalCaps = value == "1";
                else if (key == "capsown")
                    settings.capsOwn = capsOf(value);
                else if (key == "alwaysskills")
                    settings.alwaysSkills = value == "1";
                else if (key == "ichinenforce")
                    settings.ichinenForce = value == "1";
                else if (key == "alwaysscope")
                    settings.alwaysScope = std::clamp(std::atoi(value.c_str()), 0, 1);
                else if (key == "alwaysall")
                    settings.alwaysAll = value != "0";
                else if (key == "alwaysplayers")
                {
                    std::istringstream ids(value);
                    std::string id;
                    while (std::getline(ids, id, '/'))
                        if (std::atoi(id.c_str()) > 0)
                            settings.alwaysPlayers.push_back(std::atoi(id.c_str()));
                }
                else if (key == "cap")
                {
                    const size_t comma = value.find(',');
                    const int player = std::atoi(value.c_str());
                    if (player > 0 && comma != std::string::npos)
                        settings.caps[player] = capsOf(value.substr(comma + 1));
                }
                else if (key == "slot")
                {
                    int slot = -1, item = -1, held = 0;
                    if (std::sscanf(value.c_str(), "%d,%d,%d", &slot, &item, &held) >= 2 && slot >= 0 && slot < kMaxSlots && item >= 0)
                        settings.slots[slot] = Slot{item, held != 0};
                }
            }
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            std::ofstream file(g_settingsPath, std::ios::binary | std::ios::trunc);
            file << "# 이치넨 치트 설정: 게임 메뉴(F2)의 \"이치넨\" 탭이 씁니다\n"
                 << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                 << "points=" << settings.points << "\n"
                 << "lockpoints=" << (settings.lockPoints ? 1 : 0) << "\n"
                 << "rebuy=" << (settings.rebuy ? 1 : 0) << "\n"
                 << "nocap=" << (settings.noCap ? 1 : 0) << "\n"
                 << "capcontact=" << (settings.capContact ? 1 : 0) << "\n"
                 << "contactcap=" << settings.contactCap << "\n"
                 << "cappitches=" << (settings.capPitches ? 1 : 0) << "\n"
                 << "pitchcap=" << settings.pitchCap << "\n";
            for (const auto &[slot, picked] : settings.slots)
                if (picked.item >= 0)
                    file << "slot=" << slot << "," << picked.item << "," << (picked.lock ? 1 : 0) << "\n";
            file << "finalcaps=" << (settings.finalCaps ? 1 : 0) << "\n"
                 << "capsown=" << capText(settings.capsOwn) << "\n"
                 << "ichinenforce=" << (settings.ichinenForce ? 1 : 0) << "\n"
                 << "alwaysskills=" << (settings.alwaysSkills ? 1 : 0) << "\n"
                 << "alwaysscope=" << settings.alwaysScope << "\n"
                 << "alwaysall=" << (settings.alwaysAll ? 1 : 0) << "\n"
                 << "alwaysplayers=";
            for (size_t i = 0; i < settings.alwaysPlayers.size(); ++i)
                file << (i ? "/" : "") << settings.alwaysPlayers[i];
            file << "\n";
            for (const auto &[player, caps] : settings.caps)
                if (!capText(caps).empty())
                    file << "cap=" << player << "," << capText(caps) << "\n";
        }

#if defined(URBAN_ICHINEN_MENU)
        std::string grouped(long long value)
        {
            std::string digits = std::to_string(value < 0 ? -value : value), text;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i && (digits.size() - i) % 3u == 0u)
                    text += ',';
                text += digits[i];
            }
            return (value < 0 ? "-" : "") + text;
        }

        // "[타자] 장타 ＋1 (長打) · 1000P": type, Korean name (and the game's), level, price.
        std::string itemLabel(const View &view, const Item &item)
        {
            std::string type, name;
            const auto japanese = [](const std::vector<std::string> &names, int id) {
                return id >= 0 && id < static_cast<int>(names.size()) && !names[static_cast<size_t>(id)].empty()
                           ? " (" + names[static_cast<size_t>(id)] + ")" : std::string();
            };
            switch (item.type)
            {
            case 0u:
            case 1u:
            {
                type = item.type == 0u ? "[타자]" : "[투수]";
                const auto korean = g_abilityKorean.find(item.id);
                name = (korean != g_abilityKorean.end() ? korean->second : "능력 " + std::to_string(item.id)) + " ＋" +
                       std::to_string(item.level + 1) + japanese(view.abilityJapanese, item.id);
                break;
            }
            case 2u:
            case 3u:
            {
                type = "[스킬]";
                const urban::skills::Skill *skill = urban::skills::find(static_cast<uint16_t>(item.id), false);
                name = skill ? urban::skills::label(g_rdram, *skill) : "스킬 " + std::to_string(item.id);
                break;
            }
            case 4u:
            case 5u:
            {
                type = item.type == 4u ? "[선수]" : "[특별 선수]";
                const auto player = view.players.find(item.id);
                name = player != view.players.end() && !player->second.empty() ? player->second : "선수 ID " + std::to_string(item.id);
                break;
            }
            case 6u:
            {
                type = "[조건]";
                const auto korean = g_conditionKorean.find(item.id);
                name = (korean != g_conditionKorean.end() ? korean->second : "조건 " + std::to_string(item.id)) +
                       japanese(view.conditionJapanese, item.id);
                break;
            }
            default:
                type = "[?]";
                name = std::to_string(item.id);
            }
            return type + " " + name + " · " + grouped(item.price) + "P";
        }

        bool owned(const View &view, const Item &item)
        {
            if ((item.type == 2u || item.type == 3u) && item.id >= 0 && item.id < kSkills)
                return view.skillOwned[static_cast<size_t>(item.id)] != 0u;
            if (item.type == 6u && item.id >= 0 && item.id < kConditions)
                return view.conditionOwned[static_cast<size_t>(item.id)] != 0u;
            return false;
        }

        int g_skillSource = 0;   // 0 the 作成スキル一覧 (16), 1 the club's 8
        int g_listSlot = 0, g_clubSlot = 0;
        int g_fillValue = 99;    // the "한꺼번에" value
        int g_nameKey = -1;      // source * 100 + slot g_nameBuffer was filled from
        int g_confirmClear = -1; // source * 100 + slot whose 지우기 waits for 정말 지우기
        char g_nameBuffer[64] = {};

        std::string abilityName(const SkillView &skills, int i)
        {
            const auto korean = g_abilityKorean.find(static_cast<int>(skills.abilityIds[static_cast<size_t>(i)]));
            return korean != g_abilityKorean.end() ? korean->second : "능력 " + std::to_string(skills.abilityIds[static_cast<size_t>(i)]);
        }

        // The Ichinen skill editor (GS thread): the 作成スキル一覧 (16, system file) or the club's 8 (this save), and who in
        // the club has them; edits go to the EE thread.
        // The highest bonus the editor writes into a byte (+0x24 + index): the caps while locked, else 255.
        int bonusCap(const Settings &settings, int index)
        {
            if (index <= 1) // 우 교타, 좌 교타
                return settings.capContact ? settings.contactCap : 255;
            if (index >= 14) // the 18 pitch types
                return settings.capPitches ? settings.pitchCap : 255;
            return 255;
        }

        void drawSkills(const Pcsx2GsUi *ui, const SkillView &skills, const std::string &log, Settings &settings)
        {
            ui->separator_text("이치넨 스킬 편집");
            // the caps (saved at once: this part shows outside the mode too)
            {
                bool capsChanged = false;
                int lockContact = settings.capContact ? 1 : 0, lockPitches = settings.capPitches ? 1 : 0;
                if (ui->checkbox("교타 보너스 상한", &lockContact))
                {
                    settings.capContact = lockContact != 0;
                    capsChanged = true;
                }
                ui->same_line();
                ui->set_next_width(90.0f);
                if (ui->input_int("##contactcap", &settings.contactCap, 1))
                {
                    settings.contactCap = std::clamp(settings.contactCap, 0, 255);
                    capsChanged = true;
                }
                ui->same_line_at(330.0f);
                if (ui->checkbox("변화구 보너스 상한", &lockPitches))
                {
                    settings.capPitches = lockPitches != 0;
                    capsChanged = true;
                }
                ui->same_line();
                ui->set_next_width(90.0f);
                if (ui->input_int("##pitchcap", &settings.pitchCap, 1))
                {
                    settings.pitchCap = std::clamp(settings.pitchCap, 0, 255);
                    capsChanged = true;
                }
                ui->text_disabled("선수 능력을 치트로 올린다는 전제의 상한입니다(교타 +72: 경기 교타 약 200에서 홈런이 남음, 변화구 +99). 체크를 풀면 "
                                  "그 칸은 255까지 넣을 수 있습니다(1번 타자용 스킬처럼 따로 만들 때). 장타·구위·제구·정신력·체력은 255까지, 구속은 99를 넘으면 효과가 없습니다.");
                if (!settings.noCap)
                    ui->text_colored(0xFF5050FFu, "경고: 위 '99 상한 풀기'가 꺼져 있어 경기에서는 128 이상 보너스가 빼기로 쓰이고 99에서 잘립니다.");
                if (capsChanged)
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_settings.capContact = settings.capContact;
                    g_settings.contactCap = settings.contactCap;
                    g_settings.capPitches = settings.capPitches;
                    g_settings.pitchCap = settings.pitchCap;
                    saveSettings(g_settings);
                }
            }
            const char *sources[] = {"作成スキル一覧 16칸 (시스템 파일)", "내 구단 8칸 (이 세이브)"};
            ui->set_next_width(300.0f);
            ui->combo("##ichinensource", &g_skillSource, sources, 2);
            g_skillSource = std::clamp(g_skillSource, 0, 1);
            const bool listMode = g_skillSource == 0;
            ui->text_disabled(listMode ? "이치넨 모드의 '作成スキル一覧'입니다. 게임이 시스템 데이터를 저장할 때 시스템 파일에 함께 저장됩니다. "
                                         "본편에서는 게임의 전수(캠프)로 쓰거나, 아래 '구단으로 가져오기'로 바로 구단 8칸에 넣을 수 있습니다."
                                       : "이 세이브의 구단이 가진 이치넨 스킬 8칸입니다(ID 500~). 선수 스킬 칸에 넣으면 경기에서 보너스가 더해집니다.");
            if (!listMode && !skills.active)
            {
                ui->text_disabled("게임을 불러오면 나옵니다.");
                return;
            }
            const int count = listMode ? kCreatedCount : 8;
            int &current = listMode ? g_listSlot : g_clubSlot;
            std::vector<SkillAction> actions;
            auto act = [&](int kind, int slot, int index, int value, std::string text = std::string()) {
                SkillAction action{kind, slot, index, value, std::move(text)};
                action.list = listMode;
                actions.push_back(std::move(action));
            };
            std::vector<std::string> labels;
            for (int k = 0; k < count; ++k)
            {
                const SkillRecord &entry = listMode ? skills.list[static_cast<size_t>(k)] : skills.records[static_cast<size_t>(k)];
                labels.push_back((listMode ? "작성 스킬 " : "구단 슬롯 ") + std::to_string(k + 1) + " · " +
                                 (entry.id ? (entry.name.empty() ? std::string("이름 없음") : entry.name) +
                                                 (listMode ? std::string() : " (ID " + std::to_string(entry.id) + ")")
                                           : std::string("빈 칸")));
            }
            std::vector<const char *> items;
            for (const std::string &label : labels)
                items.push_back(label.c_str());
            ui->set_next_width(340.0f);
            ui->combo("##ichinenslot", &current, items.data(), count);
            current = std::clamp(current, 0, count - 1);
            if (listMode)
            {
                ui->same_line();
                if (ui->small_button("PC 파일로 보관"))
                    act(8, 0, 0, 0);
                ui->tooltip("작성 스킬 16칸을 그대로 사용자 폴더의 ichinen_skills.bin에 씁니다.");
                ui->same_line();
                if (ui->small_button("PC 파일에서 불러오기"))
                    act(9, 0, 0, 0);
                ui->tooltip("ichinen_skills.bin의 16칸으로 지금 목록을 덮어씁니다. 게임에 남기려면 그 뒤 게임에서 저장하세요.");
            }
            if (!log.empty())
                ui->text_disabled(log.c_str());
            const SkillRecord &entry = listMode ? skills.list[static_cast<size_t>(current)] : skills.records[static_cast<size_t>(current)];
            const int key = g_skillSource * 100 + current;
            if (entry.id == 0u)
            {
                if (ui->small_button("이 칸에 새 이치넨 스킬 만들기"))
                    act(2, current, 0, 0);
                ui->tooltip(listMode ? "칸 번호가 붙고(게임과 같음) 보너스는 모두 0에서 시작합니다."
                                     : "ID는 게임처럼 500부터 비어 있는 번호를 씁니다. 보너스는 모두 0에서 시작합니다.");
            }
            else
            {
                // name, bringing into the club, clearing
                if (g_nameKey != key)
                {
                    std::snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", entry.name.c_str());
                    g_nameKey = key;
                }
                ui->set_next_width(200.0f);
                ui->input_text("##ichinenname", "이름 (전각 6자 정도)", g_nameBuffer, sizeof(g_nameBuffer));
                ui->same_line();
                if (ui->small_button("이름 바꾸기"))
                    act(6, current, 0, 0, g_nameBuffer);
                if (listMode)
                {
                    ui->same_line();
                    ui->begin_disabled(skills.active ? 0 : 1);
                    if (ui->small_button("구단으로 가져오기"))
                        act(7, current, 0, 0);
                    ui->end_disabled();
                    ui->tooltip("이 스킬을 지금 구단의 빈 칸(8칸 중)에 복사하고 ID 500~를 붙입니다(게임의 가져오기와 같음). 게임을 불러온 뒤에 됩니다.");
                }
                ui->same_line();
                if (g_confirmClear == key)
                {
                    if (ui->small_button("정말 지우기"))
                    {
                        act(3, current, 0, 0);
                        g_confirmClear = -1;
                    }
                    ui->same_line();
                    if (ui->small_button("취소"))
                        g_confirmClear = -1;
                }
                else if (ui->small_button("지우기"))
                    g_confirmClear = key;
                ui->tooltip(listMode ? "이 칸을 비웁니다." : "슬롯을 비우고, 이 스킬(ID)을 가진 선수들의 스킬 칸에서도 뺍니다.");

                // quick fill
                ui->text("한꺼번에");
                ui->same_line();
                ui->set_next_width(90.0f);
                if (ui->input_int("##ichinenfill", &g_fillValue, 1))
                    g_fillValue = std::clamp(g_fillValue, 0, 255);
                auto fill = [&](int from, int to, int value) {
                    for (int i = from; i <= to; ++i)
                        act(0, current, i, std::min(value, bonusCap(settings, i)));
                };
                ui->same_line();
                if (ui->small_button("타자 능력에"))
                    fill(0, 9, g_fillValue);
                ui->same_line();
                if (ui->small_button("투수 능력에"))
                    fill(9, 13, g_fillValue);
                ui->same_line();
                if (ui->small_button("변화구에"))
                    fill(14, 31, g_fillValue);
                ui->same_line();
                if (ui->small_button("모두 0으로"))
                    fill(0, 31, 0);
                ui->text_disabled("보너스는 경기에서 선수 능력에 더해집니다 (0~255). '이치넨 스킬 99 상한 풀기'를 끄면 게임처럼 128 이상은 빼기가 되고 합이 99에서 잘립니다.");

                // the 14 ability bytes, two a row
                for (int i = 0; i < 14; ++i)
                {
                    ui->push_id(100 + i);
                    if (i % 2 == 1)
                        ui->same_line_at(330.0f);
                    int value = entry.bonus[static_cast<size_t>(i)];
                    const int cap = bonusCap(settings, i);
                    const std::string note = i == 10 ? " (경기 체력 x100)" : i == 11 ? " (99 넘으면 효과 없음)" : cap < 255 ? " (상한 " + std::to_string(cap) + ")" : "";
                    ui->set_next_width(100.0f);
                    if (ui->input_int((abilityName(skills, i) + note).c_str(), &value, 1))
                        act(0, current, i, std::clamp(value, 0, cap));
                    if (value > cap)
                    {
                        ui->same_line();
                        ui->text_colored(0xFF5050FFu, "상한 초과");
                        ui->tooltip("상한을 풀었을 때 넣은 값입니다. 그대로 두면 그 값이 쓰입니다.");
                    }
                    ui->pop_id();
                }
                if (ui->collapsing_header(("변화구 보너스 (18종" + std::string(settings.capPitches ? ", 상한 " + std::to_string(settings.pitchCap) : std::string()) + ")").c_str(), 0))
                    for (int i = 14; i < 32; ++i)
                    {
                        ui->push_id(100 + i);
                        if ((i - 14) % 2 == 1)
                            ui->same_line_at(330.0f);
                        int value = entry.bonus[static_cast<size_t>(i)];
                        const int cap = bonusCap(settings, i);
                        ui->set_next_width(100.0f);
                        if (ui->input_int(abilityName(skills, i).c_str(), &value, 1))
                            act(0, current, i, std::clamp(value, 0, cap));
                        if (value > cap)
                        {
                            ui->same_line();
                            ui->text_colored(0xFF5050FFu, "상한 초과");
                        }
                        ui->pop_id();
                    }
                if (ui->collapsing_header("경기에서 함께 붙는 스킬 (7개)", 0))
                {
                    std::vector<std::string> skillLabels{"없음"};
                    std::vector<int> skillIds{0};
                    for (const urban::skills::Skill *skill = urban::skills::begin(); skill != urban::skills::end(); ++skill)
                        if (skill->group == urban::skills::kPlayer && skill->id > 0u && skill->id < 256u)
                        {
                            skillLabels.push_back(urban::skills::label(g_rdram, *skill));
                            skillIds.push_back(skill->id);
                        }
                    std::vector<const char *> skillItems;
                    for (const std::string &label : skillLabels)
                        skillItems.push_back(label.c_str());
                    for (int j = 0; j < 7; ++j)
                    {
                        ui->push_id(200 + j);
                        const int id = entry.skills[static_cast<size_t>(j)] & 0xFF;
                        int chosen = 0;
                        for (size_t n = 0; n < skillIds.size(); ++n)
                            if (skillIds[n] == id)
                                chosen = static_cast<int>(n);
                        ui->set_next_width(300.0f);
                        if (ui->combo(("스킬 " + std::to_string(j + 1)).c_str(), &chosen, skillItems.data(), static_cast<int>(skillItems.size())))
                            act(1, current, j, skillIds[static_cast<size_t>(chosen)]);
                        ui->pop_id();
                    }
                }

                // who has it (club slots only)
                if (!listMode)
                {
                    ui->separator_text("선수에게 넣기 / 빼기 (내 구단)");
                    ui->text_disabled("체크하면 그 선수의 스킬 칸에 이 이치넨 스킬을 넣습니다. 경기용 선수 데이터를 만들 때 보너스가 더해집니다 (내 구단 선수만, 다음 경기부터).");
                    if (ui->begin_child("##ichinenplayers", 0.0f, 220.0f))
                    {
                        for (const OwnPlayer &player : skills.players)
                        {
                            const bool has = std::find(player.skills.begin(), player.skills.end(), entry.id) != player.skills.end();
                            const bool full = std::find(player.skills.begin(), player.skills.end(), uint16_t{0}) == player.skills.end();
                            int on = has ? 1 : 0;
                            ui->push_id(player.id);
                            ui->begin_disabled(!has && full ? 1 : 0);
                            if (ui->checkbox((player.label + (!has && full ? "  (스킬 칸 가득)" : "")).c_str(), &on))
                                act(on ? 4 : 5, current, 0, player.id);
                            ui->end_disabled();
                            ui->pop_id();
                        }
                    }
                    ui->end_child();
                }
            }
            if (!actions.empty())
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_skillActions.insert(g_skillActions.end(), actions.begin(), actions.end());
            }
        }

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end, one-off writes are
        // done by the next frame.
        // The cap stat a skill effect kind (sub_002B8FF0's t0) changes on the skill's own player; -1 for team, opponent and
        // fielding effects. Kind 4 (정신력) writes the batter's +0xC8 or the pitcher's +0x6C by the side at bat.
        int capStatOfKind(int kind)
        {
            switch (kind)
            {
            case 6: return kCapContact;
            case 5: return kCapPower;
            case 9: return kCapSpeed;
            case 7: return kCapEye;
            case 11: return kCapBunt;
            case 3: return kCapVelocity;
            case 2: return kCapStuff;
            case 1: return kCapControl;
            case 4: return kCapMental;
            case 13: case 14: case 15: case 24: return kCapBreaking;
            default: return kind >= 27 && kind <= 41 ? kCapBreaking : -1;
            }
        }

        std::string kindName(int kind)
        {
            static const char *const pitches[] = {"없음", "직구", "체인지업", "슬라이더", "슈트", "커브", "슬로 커브", "싱커", "스크루", "포크",
                                                  "SFF", "종슬라이더", "팜볼", "너클", "컷볼", "고속 슈트", "투심", "서클 체인지업", "드롭 커브",
                                                  "고속 슬라이더"};
            static const int pitchOfKind[] = {2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18}; // kinds 27..41
            switch (kind)
            {
            case 1: return "제구";
            case 2: return "구위";
            case 3: return "구속";
            case 4: return "정신력";
            case 5: return "장타";
            case 6: return "교타";
            case 7: return "선구안";
            case 9: return "주력";
            case 11: return "번트";
            case 13: return pitches[3];
            case 14: return pitches[9];
            case 15: return pitches[19];
            case 18: return "상대 타자 장타";
            case 19: return "상대 타자 교타";
            case 20: return "팀 타자 장타";
            case 24: return "변화구 전부";
            case 25: return "상대 투수 제구";
            case 26: return "상대 투수 구위";
            case 46: case 47: return "팀 타자 교타";
            default: break;
            }
            if (kind >= 27 && kind <= 41)
                return pitches[pitchOfKind[kind - 27]];
            return "효과 #" + std::to_string(kind);
        }

        const std::array<int16_t, 15> *skillDef(const SkillView &skills, int id)
        {
            for (const auto &fields : skills.skillDefs)
                if (fields[0] == id)
                    return &fields;
            return nullptr;
        }

        std::string skillName(int id)
        {
            const urban::skills::Skill *skill = urban::skills::find(static_cast<uint16_t>(id), false);
            return skill ? std::string(skill->ko) : "스킬 " + std::to_string(id);
        }

        // "교타 +5, 장타 +5"
        std::string effectText(const std::array<int16_t, 15> &fields)
        {
            std::string text;
            for (int j = 9; j <= 13; j += 2)
                if (fields[static_cast<size_t>(j)] != 0 || fields[static_cast<size_t>(j + 1)] != 0)
                {
                    const int value = fields[static_cast<size_t>(j + 1)];
                    text += (text.empty() ? "" : ", ") + kindName(fields[static_cast<size_t>(j)]) + (value >= 0 ? " +" : " ") + std::to_string(value);
                }
            return text;
        }

        std::string whenText(const std::array<int16_t, 15> &fields)
        {
            const int when = fields[3];
            // When (the dispatcher's t0): 1 = match phase 1, once per plate appearance (more with runners on), 2 = phase 11,
            // once per half-inning start (counted in sims 11-08), 3 = the match start, 4 = staff (at those events).
            const std::string start = when == 3 ? "경기 시작" : when == 4 ? "감독·코치" : when == 2 ? "매 이닝" : "매 타석";
            const int condition = fields[4], mode = fields[6];
            const std::string rule = condition != 0 ? "조건 " + std::to_string(condition) : mode == 0 ? "특수 판정 " + std::to_string(fields[7]) : "조건 없음";
            return start + " · " + rule + " · " + std::to_string(fields[5]) + "%";
        }

        void drawFinalCaps(const Pcsx2GsUi *ui, const SkillView &skills, Settings &settings)
        {
            ui->separator_text("최종값 상한 (경기: 다른 스킬까지 더한 뒤)");
            bool changed = false;
            int on = settings.finalCaps ? 1 : 0;
            if (ui->checkbox("최종값 상한 켜기", &on))
            {
                settings.finalCaps = on != 0;
                changed = true;
            }
            ui->tooltip("매 투구의 판정 직전에, 그 선수의 경기 값(능력치 x 컨디션 + 이치넨 보너스 + 자기·팀·상대·감독 스킬 + 경기 스크립트 배율)을 "
                        "여기 정한 값에서 자릅니다. 어떤 스킬이 붙어도 설계한 값을 넘지 않습니다. 0은 상한 없음.");
            ui->text_disabled("선수 값이 0이면 '내 팀 전체' 값을 따릅니다. 타자는 그 타석의 값, 투수는 투구 값(구속·구위·제구·정신력·변화구 레벨)을 자릅니다.");
            static int target = 0; // 0 = the own team's default, else 1 + index into skills.players
            std::vector<std::string> names{"내 팀 전체 (기본)"};
            for (const OwnPlayer &player : skills.players)
            {
                const auto mine = settings.caps.find(player.id);
                const bool has = mine != settings.caps.end() && !capText(mine->second).empty();
                names.push_back(player.label + (has ? "  *" : ""));
            }
            std::vector<const char *> items;
            for (const std::string &name : names)
                items.push_back(name.c_str());
            target = std::clamp(target, 0, static_cast<int>(items.size()) - 1);
            ui->set_next_width(320.0f);
            ui->combo("대상##finalcap", &target, items.data(), static_cast<int>(items.size()));
            const OwnPlayer *player = target > 0 ? &skills.players[static_cast<size_t>(target - 1)] : nullptr;
            std::array<int, kCapStats> &caps = player ? settings.caps[player->id] : settings.capsOwn;
            for (int s = 0; s < kCapStats; ++s)
            {
                const int column = s < kCapVelocity ? s % 3 : (s - kCapVelocity) % 3;
                if (s == kCapContact)
                    ui->text_disabled("타자");
                else if (s == kCapVelocity)
                    ui->text_disabled("투수 (정신력은 타자에게도)");
                ui->push_id(7000 + s);
                if (column != 0)
                    ui->same_line_at(20.0f + 230.0f * static_cast<float>(column));
                int value = caps[static_cast<size_t>(s)];
                ui->set_next_width(110.0f);
                if (ui->input_int(kCapLabels[s], &value, 5))
                {
                    caps[static_cast<size_t>(s)] = std::clamp(value, 0, 9999);
                    changed = true;
                }
                ui->pop_id();
            }
            if (player && ui->small_button("이 선수 상한 지우기 (내 팀 기본을 따름)"))
            {
                settings.caps.erase(player->id);
                changed = true;
            }
            if (player)
            {
                // Preview: the Ichinen bonus and the player's own skills (8 slots and the 7 of his Ichinen skill) per stat.
                const SkillRecord *ichinen = nullptr;
                int ichinenCount = 0;
                for (const uint16_t id : player->skills)
                    if (id >= 500u)
                        for (const SkillRecord &record : skills.records)
                            if (record.id == id)
                            {
                                ichinen = &record; // the game fills effect slots 8-14 from the last one (sub_002BDDE0)
                                ++ichinenCount;
                            }
                std::array<int, kCapStats> bonus{};
                if (ichinen)
                {
                    const auto &b = ichinen->bonus;
                    bonus[kCapContact] = std::max(b[0], b[1]);
                    bonus[kCapPower] = b[2];
                    bonus[kCapRunning] = b[3];
                    bonus[kCapBunt] = b[6];
                    bonus[kCapEye] = b[8];
                    bonus[kCapMental] = b[9];
                    bonus[kCapVelocity] = b[11];
                    bonus[kCapStuff] = b[12];
                    bonus[kCapControl] = b[13];
                    for (int i = 14; i < 32; ++i)
                        bonus[kCapBreaking] = std::max<int>(bonus[kCapBreaking], b[static_cast<size_t>(i)]);
                }
                std::vector<int> ids;
                for (const uint16_t id : player->skills)
                    if (id != 0u && id < 200u)
                        ids.push_back(id);
                if (ichinen)
                    for (const uint16_t id : ichinen->skills)
                        if ((id & 0xFFu) != 0u)
                            ids.push_back(id & 0xFFu);
                std::array<int, kCapStats> most{};
                std::array<std::string, kCapStats> from;
                std::vector<std::string> others;
                for (const int id : ids)
                {
                    const auto *fields = skillDef(skills, id);
                    if (!fields)
                        continue;
                    for (int j = 9; j <= 13; j += 2)
                    {
                        const int kind = (*fields)[static_cast<size_t>(j)], value = (*fields)[static_cast<size_t>(j + 1)];
                        if (kind == 0 && value == 0)
                            continue;
                        const int stat = capStatOfKind(kind);
                        if (stat < 0)
                        {
                            others.push_back(skillName(id) + ": " + kindName(kind) + (value >= 0 ? " +" : " ") + std::to_string(value));
                            continue;
                        }
                        if (value > 0)
                            most[static_cast<size_t>(stat)] += value;
                        from[static_cast<size_t>(stat)] += (from[static_cast<size_t>(stat)].empty() ? "" : ", ") + skillName(id) + (value >= 0 ? " +" : " ") +
                                                          std::to_string(value) + " (" + std::to_string((*fields)[5]) + "%)";
                    }
                }
                SeenFinal seen[kCapStats];
                {
                    std::lock_guard<std::mutex> lock(g_capMutex);
                    const auto it = g_seenFinal.find(player->id);
                    if (it != g_seenFinal.end())
                        for (int s = 0; s < kCapStats; ++s)
                            seen[s] = it->second[static_cast<size_t>(s)];
                }
                if (skills.skillDefs.empty())
                    ui->text_disabled("스킬 정의 표가 아직 메모리에 없습니다 (게임을 진행하면 나옵니다).");
                if (ichinenCount > 1)
                    ui->text_colored(0xFF5050FFu, "이치넨 스킬이 2개 이상입니다: 경기에서는 마지막 칸의 이치넨 스킬 7개만 스킬로 붙습니다.");
                if (ui->begin_table("finalcap_preview", 5, PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_ROW_BG, 0.0f, 0.0f))
                {
                    ui->table_column("능력", 0, 90.0f);
                    ui->table_column("이치넨", 0, 60.0f);
                    ui->table_column("자기 스킬 최대", 0, 100.0f);
                    ui->table_column("마지막 경기 값", 0, 130.0f);
                    ui->table_column("붙는 스킬 (발동 확률)", 0, 0.0f);
                    ui->table_headers();
                    for (int s = 0; s < kCapStats; ++s)
                    {
                        if ((player->pitcher ? s < kCapVelocity : s >= kCapVelocity) && bonus[static_cast<size_t>(s)] == 0 && from[static_cast<size_t>(s)].empty())
                            continue;
                        ui->table_next_row();
                        ui->table_next_column();
                        ui->text(kCapLabels[s]);
                        ui->table_next_column();
                        ui->text(bonus[static_cast<size_t>(s)] ? ("+" + std::to_string(bonus[static_cast<size_t>(s)])).c_str() : "-");
                        ui->table_next_column();
                        ui->text(most[static_cast<size_t>(s)] ? ("+" + std::to_string(most[static_cast<size_t>(s)])).c_str() : "-");
                        ui->table_next_column();
                        if (seen[s].before >= 0)
                            ui->text((std::to_string(seen[s].before) + (seen[s].after != seen[s].before ? " -> " + std::to_string(seen[s].after) : "")).c_str());
                        else
                            ui->text_disabled("-");
                        ui->table_next_column();
                        ui->text_wrapped(from[static_cast<size_t>(s)].empty() ? "-" : from[static_cast<size_t>(s)].c_str());
                    }
                    ui->end_table();
                }
                ui->text_disabled("이치넨 = 이 선수 이치넨 스킬의 보너스. 자기 스킬 최대 = 이 선수의 스킬(이치넨 스킬에 넣은 7개 포함)이 모두 발동했을 때의 합. "
                                  "팀 리더·감독·코치 스킬과 상대 투수의 약화 스킬은 따로 더해집니다. 마지막 경기 값 = 상한 직전 값 -> 상한 뒤 값 (상한이 켜져 있을 때).");
                if (!others.empty())
                {
                    std::string text = "다른 효과: ";
                    for (size_t i = 0; i < others.size(); ++i)
                        text += (i ? ", " : "") + others[i];
                    ui->text_wrapped(text.c_str());
                }
            }
            if (changed)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings.finalCaps = settings.finalCaps;
                g_settings.capsOwn = settings.capsOwn;
                g_settings.caps = settings.caps;
                saveSettings(g_settings);
                publishCaps(g_settings);
            }
            ui->separator_text("스킬 상시 작동");
            bool alwaysChanged = false;
            int force = settings.ichinenForce ? 1 : 0;
            if (ui->checkbox("이치넨 스킬에 넣은 스킬을 상시 작동 (조건·확률 무시, 경기 내내)", &force))
            {
                settings.ichinenForce = force != 0;
                alwaysChanged = true;
            }
            ui->tooltip("이치넨 스킬의 스킬 칸 7개에 넣은 스킬이 경기에서 그 선수의 첫 판정 때 조건(좌우, 득점권, 홈·원정, 볼카운트 등)과 "
                        "확률 없이 바로 붙고, 경기 끝까지 유지됩니다. 타자 스킬은 타석에서, 투수 스킬은 투구에서 쓰입니다.");
            int always = settings.alwaysSkills ? 1 : 0;
            if (ui->checkbox("스킬 발동 확률을 100%로 (조건은 그대로)", &always))
            {
                settings.alwaysSkills = always != 0;
                alwaysChanged = true;
            }
            ui->tooltip("스킬은 정해진 판정(경기 시작·매 타석·매 이닝)마다 확률(10~100% x 팀 기세)로 발동합니다. 켜면 그 확률 굴림을 항상 "
                        "통과합니다. 상황 조건(좌우, 득점권, 홈·원정, 볼카운트 같은 특수 판정)은 그대로입니다.");
            if (ui->radio("이치넨 스킬에 넣은 7개만", settings.alwaysScope == 0 ? 1 : 0) && settings.alwaysScope != 0)
            {
                settings.alwaysScope = 0;
                alwaysChanged = true;
            }
            ui->same_line();
            if (ui->radio("모든 스킬 (감독·코치 포함)", settings.alwaysScope == 1 ? 1 : 0) && settings.alwaysScope != 1)
            {
                settings.alwaysScope = 1;
                alwaysChanged = true;
            }
            int whole = settings.alwaysAll ? 1 : 0;
            if (ui->checkbox("내 팀 전원 (끄면 아래에서 고른 선수만)", &whole))
            {
                settings.alwaysAll = whole != 0;
                alwaysChanged = true;
            }
            if (!settings.alwaysAll)
            {
                ui->begin_child("always_players", 0.0f, 150.0f);
                for (const OwnPlayer &one : skills.players)
                {
                    const auto at = std::find(settings.alwaysPlayers.begin(), settings.alwaysPlayers.end(), static_cast<int>(one.id));
                    int picked = at != settings.alwaysPlayers.end() ? 1 : 0;
                    ui->push_id(8000 + one.id);
                    if (ui->checkbox(one.label.c_str(), &picked))
                    {
                        if (picked)
                            settings.alwaysPlayers.push_back(one.id);
                        else if (at != settings.alwaysPlayers.end())
                            settings.alwaysPlayers.erase(at);
                        alwaysChanged = true;
                    }
                    ui->pop_id();
                }
                ui->end_child();
            }
            if (alwaysChanged)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings.alwaysSkills = settings.alwaysSkills;
                g_settings.ichinenForce = settings.ichinenForce;
                g_settings.alwaysScope = settings.alwaysScope;
                g_settings.alwaysAll = settings.alwaysAll;
                g_settings.alwaysPlayers = settings.alwaysPlayers;
                saveSettings(g_settings);
                publishCaps(g_settings);
            }
            if (ui->collapsing_header(("스킬 효과 표 (게임 데이터, " + std::to_string(skills.skillDefs.size()) + "개)").c_str(), 0))
            {
                static int onlyPlain = 0;
                ui->checkbox("조건 없는 스킬만", &onlyPlain);
                ui->text_disabled("판정: 경기 시작 = 경기마다 한 번, 매 타석 = 타석이 시작될 때, 매 이닝 = 공수 교대마다. 조건 없음 = 상황 조건 없이 확률만. "
                                  "특수 판정 = 볼카운트·구종 같은 자기 판정이 따로 있음. % = 판정마다의 발동 확률. 지속: 1구 / 타석 / 이닝 / 경기.");
                if (ui->begin_table("skill_defs", 6, PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_RESIZABLE, 0.0f, 360.0f))
                {
                    ui->table_freeze(0, 1);
                    ui->table_column("ID", 0, 40.0f);
                    ui->table_column("스킬", 0, 150.0f);
                    ui->table_column("대상", 0, 60.0f);
                    ui->table_column("판정", 0, 210.0f);
                    ui->table_column("지속", 0, 50.0f);
                    ui->table_column("효과", 0, 0.0f);
                    ui->table_headers();
                    static const char *const who[] = {"투수", "타자", "주자", "수비", "감독·코치"};
                    static const char *const lasting[] = {"1구", "타석", "이닝", "경기"};
                    for (const auto &fields : skills.skillDefs)
                    {
                        if (onlyPlain && (fields[4] != 0 || fields[6] == 0))
                            continue;
                        ui->table_next_row();
                        ui->table_next_column();
                        ui->text(std::to_string(fields[0]).c_str());
                        ui->table_next_column();
                        ui->text(skillName(fields[0]).c_str());
                        ui->table_next_column();
                        ui->text(fields[2] >= 0 && fields[2] <= 4 ? who[fields[2]] : "?");
                        ui->table_next_column();
                        ui->text(whenText(fields).c_str());
                        ui->table_next_column();
                        ui->text(fields[8] >= 0 && fields[8] <= 3 ? lasting[fields[8]] : "?");
                        ui->table_next_column();
                        ui->text_wrapped(effectText(fields).c_str());
                    }
                    ui->end_table();
                }
            }
        }

        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 이치넨 치트가 꺼져 있습니다.");
                return;
            }
            Settings settings;
            View view;
            SkillView skills;
            std::string skillLog;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                view = g_view;
                skills = g_skillView;
                skillLog = g_skillLog;
            }
            // Ichinen-made skills in matches: any mode (the skills are used in the main game).
            ui->separator_text("이치넨 스킬 (경기)");
            int noCap = settings.noCap ? 1 : 0;
            if (ui->checkbox("이치넨 스킬 99 상한 풀기 (99+99=198)", &noCap))
            {
                settings.noCap = noCap != 0;
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings.noCap = settings.noCap;
                g_noCap.store(settings.noCap);
                saveSettings(g_settings);
            }
            ui->tooltip("이치넨에서 만든 스킬(ID 500 이상)을 가진 선수가 경기에 나올 때 그 스킬의 능력 보너스를 더하는 방식입니다. "
                        "게임은 -128~127로 더한 뒤 99에서 자르고(체력만 예외), 켜면 0~255로 더해 255까지 둡니다. "
                        "타자 능력은 99를 넘은 값 그대로 타석에 쓰입니다. 투수의 구속·구위·제구·변화구는 게임이 투구 때 다시 99로 자릅니다.");
            ui->text_disabled(settings.noCap ? "켜짐: 이치넨 스킬 보너스를 255까지 더합니다 (다음 경기부터)." : "꺼짐: 게임 그대로 (보너스를 더한 뒤 99에서 자름).");
            drawSkills(ui, skills, skillLog, settings);
            drawFinalCaps(ui, skills, settings);
            if (!view.active)
            {
                ui->text_wrapped("이치넨 모드에서 상점 목록이 올라오면 여기에 나옵니다 (타이틀 > イチネンモード로 게임을 진행하세요).");
                return;
            }
            bool changed = false;
            std::vector<Action> actions;
            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("이치넨 치트 켜기 (고정한 슬롯·포인트·다시 살 수 있게를 씀)", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->text_disabled("한 번 바꾸기와 '다시 살 수 있게' 버튼은 스위치와 관계없이 바로 씁니다. 기능별 'ichinen 너클러'는 앞 12칸을 덮어쓰니 끄세요.");

            // Points
            ui->separator_text("포인트");
            const long long points = settings.points >= 0 ? settings.points : view.points;
            ui->text(("지금 " + grouped(view.points) + "P · 설정 " + grouped(points) + "P").c_str());
            long long next = points;
            for (long long step : {10000LL, 1000LL, 100LL})
            {
                ui->same_line();
                if (ui->small_button(("-" + grouped(step)).c_str()))
                    next = points - step;
            }
            for (long long step : {100LL, 1000LL, 10000LL})
            {
                ui->same_line();
                if (ui->small_button(("+" + grouped(step)).c_str()))
                    next = points + step;
            }
            ui->same_line();
            if (ui->small_button("최대"))
                next = kMaxPoints;
            next = std::clamp<long long>(next, 0, kMaxPoints);
            if (next != points)
            {
                settings.points = next;
                actions.push_back({2, -1, next});
                changed = true;
            }
            int hold = settings.lockPoints ? 1 : 0;
            if (ui->checkbox("포인트 고정 (매 프레임 이 값으로)", &hold))
            {
                settings.lockPoints = hold != 0;
                if (settings.points < 0)
                    settings.points = view.points;
                changed = true;
            }

            // Lineup
            ui->separator_text(("판매 목록 " + std::to_string(view.lineup.size()) + "칸 (상점 화면을 다시 열면 바뀐 목록이 보입니다)").c_str());
            if (ui->small_button("모든 슬롯 다시 살 수 있게"))
                actions.push_back({1, -1, 0});
            ui->same_line();
            int rebuyAll = settings.rebuy ? 1 : 0;
            if (ui->checkbox("계속 다시 살 수 있게 (산 표시를 매 프레임 지움)", &rebuyAll))
            {
                settings.rebuy = rebuyAll != 0;
                changed = true;
            }
            std::vector<std::string> labels;
            for (size_t i = 0; i < view.catalog.size(); ++i)
                labels.push_back(std::to_string(i) + "  " + itemLabel(view, view.catalog[i]));
            std::vector<const char *> pointers;
            for (const std::string &label : labels)
                pointers.push_back(label.c_str());
            ui->begin_child("ichinen slots", 0.0f, 0.0f);
            for (size_t k = 0; k < view.lineup.size(); ++k)
            {
                ui->push_id(static_cast<int>(k));
                const int index = view.lineup[k];
                const bool known = index >= 0 && index < static_cast<int>(view.catalog.size());
                const bool bought = k < 32u && ((view.boughtA >> k) & 1u) != 0u;
                char head[32];
                std::snprintf(head, sizeof(head), "슬롯 %2zu", k + 1u);
                ui->text(head);
                ui->same_line();
                if (bought)
                    ui->text_colored(0xFF5050FFu, "산 것");
                else if (known && owned(view, view.catalog[static_cast<size_t>(index)]))
                    ui->text_colored(0xFF66CCFFu, "보유");
                else
                    ui->text_disabled("판매");
                ui->same_line();
                ui->text(known ? itemLabel(view, view.catalog[static_cast<size_t>(index)]).c_str() : "(비어 있음)");
                ui->indent();
                Slot &picked = settings.slots[static_cast<int>(k)];
                int current = picked.item >= 0 ? picked.item : (known ? index : 0);
                ui->set_next_width(420.0f);
                if (!pointers.empty() && ui->combo("##item", &current, pointers.data(), static_cast<int>(pointers.size())))
                {
                    picked.item = current;
                    actions.push_back({0, static_cast<int>(k), current});
                    changed = true;
                }
                ui->same_line();
                int locked = picked.lock ? 1 : 0;
                if (ui->checkbox("고정", &locked))
                {
                    picked.lock = locked != 0;
                    if (picked.lock && picked.item < 0 && known)
                        picked.item = index;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("다시 살 수 있게"))
                    actions.push_back({1, static_cast<int>(k), 0});
                ui->unindent();
                ui->pop_id();
            }
            ui->end_child();
            if (!changed && actions.empty())
                return;
            for (auto it = settings.slots.begin(); it != settings.slots.end();)
                it = it->second.item < 0 && !it->second.lock ? settings.slots.erase(it) : std::next(it);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings = settings;
                g_actions.insert(g_actions.end(), actions.begin(), actions.end());
            }
            if (changed)
                saveSettings(settings);
        }
#endif
    }

    namespace
    {
        // sub_00295FE0(skill object, side): the chance multiplier x10 from the side's momentum (+0xEF4, table 0x5E76A8 ->
        // 0x5E76D8: 10 at 0, 50 at 100). sub_002BEA78 activates a skill when rand 0-99 (sub_0032E360) < chance x it / 10,
        // so 1000 there passes every chance; its call returns to 0x2BEBD4 with fp = side, s7 = player slot (28 = staff),
        // [sp + 0x10] = effect slot (0-7 the player's skills, 8-14 his Ichinen skill's 7) and s1 = the skill.
        constexpr uint32_t kChanceFactor = 0x295FE0u, kChanceCall = 0x2BEBD4u;
        GuestFunction g_chanceOriginal = nullptr;

        // The player at (side, slot) is one of the picked (the own team, or the list); slot 28 = the side's staff. Under g_capMutex.
        bool pickedLocked(uint32_t side, uint32_t slot)
        {
            if (side > 1u || slot > 28u)
                return false;
            const uint32_t team = read32(side == 0u ? 0x5D1278u : 0x5D1274u); // sub_002A46E8 -> sub_0017E2D8
            if (!valid(team, 0x2008u + 4u * 28u))
                return false;
            auto own = [](uint16_t id) { return std::find(g_ownIdsNow.begin(), g_ownIdsNow.end(), id) != g_ownIdsNow.end(); };
            if (slot == 28u) // the staff's skills: the own side, with the whole team picked
            {
                const uint32_t first = read32(team + 0x2008u);
                return g_alwaysAllNow && valid(first, 2u) && own(read16(first));
            }
            const uint32_t copy = read32(team + 0x2008u + 4u * slot);
            if (!valid(copy, 2u))
                return false;
            const uint16_t id = read16(copy);
            return g_alwaysAllNow ? own(id) : std::find(g_alwaysPlayersNow.begin(), g_alwaysPlayersNow.end(), id) != g_alwaysPlayersNow.end();
        }

        // 이치넨 스킬 상시 작동: effect slots 8-14 hold the player's Ichinen skill's 7 skills (sub_002BDDE0).
        bool forceFor(uint32_t side, uint32_t slot, uint32_t k)
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            return g_ichinenForceNow && k >= 8u && k <= 14u && slot < 28u && pickedLocked(side, slot);
        }

        // The chance roll passes: 스킬 발동 100% (its scope) or 이치넨 스킬 상시 작동.
        bool alwaysFor(uint32_t side, uint32_t slot, uint32_t k)
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            if (k > 14u)
                return false;
            if (g_ichinenForceNow && k >= 8u && slot < 28u && pickedLocked(side, slot))
                return true;
            return g_alwaysOn && (g_alwaysScopeNow != 0 || k >= 8u) && pickedLocked(side, slot);
        }

        // sub_002BDD70(skill object, skill, field) (a leaf): the field of the skill's definition record, 0 when none.
        uint32_t skillField(uint32_t object, uint32_t skill, uint32_t field)
        {
            if (static_cast<int32_t>(skill) <= 0)
                return 0u;
            const uint32_t table = read32(object + 0xD370u);
            if (table == 0u || !valid(table, 0x20u + 118u * 30u))
                return 0u;
            for (uint32_t t = 0; t < 0x76u; ++t)
                if (read16(table + 0x20u + 30u * t) == skill)
                    return read16(table + 0x20u + 30u * t + 2u * field);
            return 0u;
        }

        // In the dispatcher sub_002BEA78 (fp = side, s7 = slot, [sp + 0x10] = effect slot), an Ichinen skill's skill under
        // 상시 작동 reads: 4 condition 0 (table 0x5E94B0[0] returns 1), 5 chance 100, 6 apply now (1: no batter deferral),
        // 7 special function 0 (sub_002B9860: the three pairs), 8 duration 3 (sub_002BEE70 never clears it).
        constexpr uint32_t kSkillField = 0x2BDD70u, kDispatchStart = 0x2BEA78u, kDispatchEnd = 0x2BEE70u;
        GuestFunction g_fieldOriginal = nullptr;

        void fieldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), field = getRegU32(ctx, 6);
            if (ra >= kDispatchStart && ra < kDispatchEnd && field >= 4u && field <= 8u &&
                forceFor(getRegU32(ctx, 30), getRegU32(ctx, 23), read32(getRegU32(ctx, 29) + 0x10u)))
            {
                static const uint32_t forced[5] = {0u, 100u, 1u, 0u, 3u};
                ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(forced[field - 4u]));
                ctx->pc = ra;
                return;
            }
            if (g_fieldOriginal)
            {
                g_fieldOriginal(rdram, ctx, runtime); // nothing after it
                return;
            }
            ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(skillField(getRegU32(ctx, 4), getRegU32(ctx, 5), field)));
            ctx->pc = ra;
        }

        uint32_t chanceFactor(uint32_t object, uint32_t side)
        {
            const int16_t mood = static_cast<int16_t>(read16(object + 0xEF4u + 2u * side));
            if (mood < -99)
                return 1u;
            uint32_t i = 0;
            if (mood < static_cast<int16_t>(read16(0x5E76A8u)))
                for (i = 1; static_cast<int16_t>(read16(0x5E76A8u + 2u * i)) != -9999 && mood < static_cast<int16_t>(read16(0x5E76A8u + 2u * i)); ++i)
                {
                }
            return read32(0x5E76D8u + 4u * i);
        }

        void chanceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (ra == kChanceCall)
            {
                const uint32_t side = getRegU32(ctx, 30), slot = getRegU32(ctx, 23), k = read32(getRegU32(ctx, 29) + 0x10u);
                if (alwaysFor(side, slot, k))
                {
                    static const bool log = std::getenv("URBAN_ALWAYS_LOG") != nullptr;
                    if (log)
                        std::fprintf(stderr, "[always] side %u slot %u k %u skill %u\n", side, slot, k, getRegU32(ctx, 17) & 0xFFu);
                    ctx->r[2] = _mm_set_epi64x(0, 1000);
                    ctx->pc = ra;
                    return;
                }
            }
            if (g_chanceOriginal)
            {
                g_chanceOriginal(rdram, ctx, runtime); // nothing after it
                return;
            }
            ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(chanceFactor(getRegU32(ctx, 4), getRegU32(ctx, 5) & 1u)));
            ctx->pc = ra;
        }
    }

    bool finalCapsOn()
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        return g_capsOn;
    }

    int finalCap(uint16_t player, bool own, int stat)
    {
        if (stat < 0 || stat >= kCapStats)
            return 0;
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (!g_capsOn)
            return 0;
        const auto mine = g_capsNow.find(player);
        if (mine != g_capsNow.end() && mine->second[static_cast<size_t>(stat)] > 0)
            return mine->second[static_cast<size_t>(stat)];
        return own ? g_capsOwnNow[static_cast<size_t>(stat)] : 0;
    }

    void noteFinal(uint16_t player, int stat, int before, int after)
    {
        if (stat < 0 || stat >= kCapStats)
            return;
        std::lock_guard<std::mutex> lock(g_capMutex);
        g_seenFinal[player][static_cast<size_t>(stat)] = SeenFinal{before, after};
    }

    void install(uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        g_settingsPath = userDir / "ichinen_cheats.ini";
        g_settings = loadSettings(g_settingsPath);
        g_noCap.store(g_settings.noCap);
        publishCaps(g_settings);
        if (const char *log = std::getenv("URBAN_ICHINEN_LOG"))
            g_bonusLog = std::strcmp(log, "1") == 0;
        if (const char *test = std::getenv("URBAN_TEST_ICHINEN"); test && *test)
        {
            const std::string spec = test;
            const size_t first = spec.find(','), second = spec.find(',', first == std::string::npos ? first : first + 1);
            if (first != std::string::npos)
            {
                std::istringstream ids(spec.substr(0, first));
                std::string id;
                while (std::getline(ids, id, '/'))
                    if (std::atoi(id.c_str()) > 0)
                        g_testPlayers.push_back(std::atoi(id.c_str()));
                g_testBonus = std::atoi(spec.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1).c_str());
                const std::string fields = second != std::string::npos ? spec.substr(second + 1) : std::string();
                auto fieldBonus = [&](const std::string &name, int &bonus) {
                    const size_t at = fields.find(name);
                    if (at == std::string::npos)
                        return false;
                    const size_t colon = at + name.size();
                    bonus = colon < fields.size() && fields[colon] == ':' ? std::atoi(fields.c_str() + colon + 1) : g_testBonus;
                    return true;
                };
                g_testContact = fieldBonus("contact", g_testContactBonus);
                g_testPower = fieldBonus("power", g_testPowerBonus);
            }
            g_bonusLog = true;
        }
        if (const char *test = std::getenv("URBAN_TEST_CLEAR_SKILLS"); test && *test)
        {
            std::istringstream ids(test);
            std::string id;
            while (std::getline(ids, id, '/'))
                if (std::atoi(id.c_str()) > 0)
                    g_testClearSkills.push_back(std::atoi(id.c_str()));
        }
        if (const char *test = std::getenv("URBAN_TEST_ICHINEN_LIST"); test && *test)
        {
            std::istringstream fields(test);
            std::string slot, player, bytes;
            if (std::getline(fields, slot, ',') && std::getline(fields, player, ',') && std::getline(fields, bytes, ','))
            {
                g_testListSlot = std::clamp(std::atoi(slot.c_str()), 0, kCreatedCount - 1);
                g_testListPlayer = std::atoi(player.c_str());
                std::istringstream pairs(bytes);
                std::string pair;
                while (std::getline(pairs, pair, '+'))
                {
                    const size_t colon = pair.find(':');
                    if (colon != std::string::npos)
                        g_testListBytes.emplace_back(std::clamp(std::atoi(pair.substr(0, colon).c_str()), 0, 31),
                                                     std::clamp(std::atoi(pair.substr(colon + 1).c_str()), 0, 255));
                }
            }
            g_bonusLog = true;
        }
        if (const char *test = std::getenv("URBAN_TEST_ICHINEN_EDIT"); test && *test)
        {
            std::istringstream fields(test);
            std::string slot, player, bytes;
            if (std::getline(fields, slot, ',') && std::getline(fields, player, ',') && std::getline(fields, bytes, ','))
            {
                g_testEditSlot = std::clamp(std::atoi(slot.c_str()), 0, 7);
                std::istringstream ids(player);
                std::string id;
                while (std::getline(ids, id, '/'))
                    if (std::atoi(id.c_str()) > 0)
                        g_testEditPlayers.push_back(std::atoi(id.c_str()));
                std::istringstream pairs(bytes);
                std::string pair;
                while (std::getline(pairs, pair, '+'))
                {
                    const size_t colon = pair.find(':');
                    const int byte = colon != std::string::npos ? std::atoi(pair.substr(0, colon).c_str()) : -1;
                    if (colon != std::string::npos) // 0-31 the bonus bytes, 100-106 the skill slots
                        g_testEditBytes.emplace_back(byte >= 100 ? std::clamp(byte, 100, 106) : std::clamp(byte, 0, 31),
                                                     std::clamp(std::atoi(pair.substr(colon + 1).c_str()), 0, 255));
                }
            }
            g_bonusLog = true;
        }
        loadKorean(userDir / "mods" / std::filesystem::path(u8"기능별") / std::filesystem::path(u8"한국어 이름.tsv"));
        std::fprintf(stderr, "[ichinen] cheats %s, %zu slots held, %zu Korean names, Ichinen skills %s\n", g_settings.enabled ? "on" : "off",
                     g_settings.slots.size(), g_abilityKorean.size() + g_conditionKorean.size(),
                     g_settings.noCap ? "past 99 (노캡)" : "cut at 99 (the game)");
#if defined(URBAN_ICHINEN_MENU)
        ps2x::menu::addPanel("이치넨", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode)
            ps2x::pnach::addFrameHook(frame);
    }

    void installHooks(PS2Runtime &runtime)
    {
        (void)runtime;
        if (!g_rdram || g_safeMode)
            return;
        FunctionHooks &hooks = FunctionHooks::instance();
        if (hooks.replace(kChanceFactor, "ichinen_skill_chance", &chanceHook, ReplacementStatus::Native,
                          "ichinen cheats: 스킬 발동 100% (the skill checks' chance multiplier); off: the game's own"))
            g_chanceOriginal = hooks.original(kChanceFactor);
        if (hooks.replace(kSkillField, "ichinen_skill_field", &fieldHook, ReplacementStatus::Native,
                          "ichinen cheats: 이치넨 스킬 상시 작동 (the dispatcher's definition reads); off: the game's own"))
            g_fieldOriginal = hooks.original(kSkillField);
        if (!hooks.replace(kBonusFunction, "ichinen_skill_bonus", &bonusHook, ReplacementStatus::Native,
                           "ichinen cheats: Ichinen-made skills past 99 (노캡); off: the game's own"))
            return;
        g_bonusOriginal = hooks.original(kBonusFunction);
        if (!g_bonusOriginal)
            std::fprintf(stderr, "[ichinen] sub_00160BC0 has no recompiled function: Ichinen skills always past 99\n");
    }
}
