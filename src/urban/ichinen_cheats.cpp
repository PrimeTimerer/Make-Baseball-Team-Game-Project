#include "urban/ichinen_cheats.h"

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

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_settingsPath;
        bool g_safeMode = false;
        std::mutex g_mutex;
        Settings g_settings;
        View g_view;
        std::vector<Action> g_actions;
        std::map<int, std::string> g_abilityKorean, g_conditionKorean;

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

        // EE thread, at every vblank after the .pnach patches: the menu's one-off writes, the held ones, and every 15
        // frames what the menu shows.
        void frame()
        {
            static unsigned tick = 0u;
            std::lock_guard<std::mutex> lock(g_mutex);
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

        // ichinen_cheats.ini: enabled=, points=, lockpoints=, rebuy=, slot=<slot>,<catalog number>,<lock>
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
                 << "rebuy=" << (settings.rebuy ? 1 : 0) << "\n";
            for (const auto &[slot, picked] : settings.slots)
                if (picked.item >= 0)
                    file << "slot=" << slot << "," << picked.item << "," << (picked.lock ? 1 : 0) << "\n";
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

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end, one-off writes are
        // done by the next frame.
        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 이치넨 치트가 꺼져 있습니다.");
                return;
            }
            Settings settings;
            View view;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                view = g_view;
            }
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

    void install(uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        g_settingsPath = userDir / "ichinen_cheats.ini";
        g_settings = loadSettings(g_settingsPath);
        loadKorean(userDir / "mods" / std::filesystem::path(u8"기능별") / std::filesystem::path(u8"한국어 이름.tsv"));
        std::fprintf(stderr, "[ichinen] cheats %s, %zu slots held, %zu Korean names\n", g_settings.enabled ? "on" : "off",
                     g_settings.slots.size(), g_abilityKorean.size() + g_conditionKorean.size());
#if defined(URBAN_ICHINEN_MENU)
        ps2x::menu::addPanel("이치넨", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode)
            ps2x::pnach::addFrameHook(frame);
    }
}
