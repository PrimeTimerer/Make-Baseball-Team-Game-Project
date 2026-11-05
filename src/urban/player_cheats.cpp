#include "urban/player_cheats.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_PLAYER_MENU 1
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

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace urban::players
{
    namespace
    {
        // YKTK3 (SLPM-65853): the same chain after a new game and after loading a save. This port's heap is laid out
        // 0x40 below PCSX2's, so fixed addresses from PCSX2 cheats miss; the chain does not.
        constexpr uint32_t kTeamTable = 0x5D1240u;    // 12 team pointers: lions 0 ... yokohama 11
        constexpr uint32_t kTeamRoster = 0x120u;      // team -> roster
        constexpr uint32_t kRosterClass = 0x4u;       // roster -> its class; the player's own team has its own one
        constexpr uint32_t kOwnTeamClass = 0x6659F0u; // (CPU teams: 0x665378)
        constexpr uint32_t kRosterPlayers = 0x8u;     // roster -> 40 player record pointers
        constexpr uint32_t kRecordSize = 0x1CCu;      // the own team's records (CPU teams' are 0xF8)
        // Record fields: player ID (+0, 0 in an empty slot), name (+4, 16 bytes Shift-JIS), uniform number (+0x17),
        // age (+0x18), not 0 at +0x26 for pitchers.
        constexpr uint32_t kName = 0x4u, kNumber = 0x17u, kAge = 0x18u, kPitcher = 0x26u;
        constexpr int kTeams = 12, kSlots = 40;
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스",   "파이터즈", "마린즈",   "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프",     "요코하마"};

        struct Run
        {
            uint32_t offset = 0u;
            std::vector<uint8_t> bytes;
        };
        struct Item
        {
            std::string key, label, note, section;
            std::vector<Run> runs;
        };
        struct Pin
        {
            bool on = true;
            uint32_t mask = 0u;
            std::string name;
        };
        struct Settings
        {
            bool enabled = false;
            bool all = true;    // the players without a pin
            uint32_t mask = 0u; // their items (bit i = g_items[i])
            std::map<uint16_t, Pin> pins;
        };
        struct Row
        {
            uint16_t id = 0u;
            int number = 0, age = 0;
            bool pitcher = false;
            std::string name;
        };

        uint8_t *g_rdram = nullptr;
        std::vector<Item> g_items; // read once at start
        std::filesystem::path g_itemsPath, g_settingsPath;
        bool g_safeMode = false;
        std::mutex g_mutex; // the settings and the roster: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        int g_team = -1;
        std::vector<Row> g_rows;

        std::string trim(std::string text)
        {
            const char *spaces = " \t\r\n";
            text.erase(0, text.find_first_not_of(spaces));
            const size_t last = text.find_last_not_of(spaces);
            text.erase(last == std::string::npos ? 0u : last + 1u);
            return text;
        }

        std::string utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
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

        // A guest pointer this code follows: word aligned, past the kernel, with `size` bytes inside the 32 MB.
        bool valid(uint32_t address, uint32_t size)
        {
            return (address & 3u) == 0u && address >= 0x100000u && address < kRamSize && size <= kRamSize - address;
        }

        uint32_t allItems()
        {
            return g_items.size() >= 32u ? 0xFFFFFFFFu : (1u << g_items.size()) - 1u;
        }

        // player_cheat_items.txt: "== section ==", "[key] label", then "> note" and "offset: byte byte ..." lines (hex,
        // from the start of a player record); "#" comments. At most 32 items.
        std::vector<Item> loadItems(const std::filesystem::path &path)
        {
            std::vector<Item> items;
            std::ifstream file(path, std::ios::binary);
            std::string line, section;
            bool first = true;
            while (std::getline(file, line))
            {
                if (first && line.rfind("\xEF\xBB\xBF", 0) == 0u)
                    line.erase(0, 3);
                first = false;
                line = trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line.rfind("==", 0) == 0u)
                {
                    const size_t start = line.find_first_not_of("= ");
                    const size_t end = line.find_last_not_of("= ");
                    section = start == std::string::npos ? std::string() : line.substr(start, end - start + 1);
                    continue;
                }
                if (line[0] == '[')
                {
                    const size_t close = line.find(']');
                    if (close == std::string::npos || items.size() >= 32u)
                        continue;
                    Item item;
                    item.section = section;
                    item.key = trim(line.substr(1, close - 1));
                    item.label = trim(line.substr(close + 1));
                    if (item.label.empty())
                        item.label = item.key;
                    items.push_back(std::move(item));
                    continue;
                }
                if (items.empty())
                    continue;
                if (line[0] == '>')
                {
                    items.back().note = trim(line.substr(1));
                    continue;
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                Run run;
                char *end = nullptr;
                const std::string offset = trim(line.substr(0, colon));
                run.offset = static_cast<uint32_t>(std::strtoul(offset.c_str(), &end, 16));
                if (offset.empty() || *end != '\0')
                    continue;
                std::istringstream bytes(line.substr(colon + 1));
                std::string byte;
                while (bytes >> byte)
                    run.bytes.push_back(static_cast<uint8_t>(std::strtoul(byte.c_str(), nullptr, 16)));
                if (!run.bytes.empty() && run.offset + run.bytes.size() <= kRecordSize)
                    items.back().runs.push_back(std::move(run));
            }
            return items;
        }

        uint32_t maskOf(const std::string &keys)
        {
            uint32_t mask = 0u;
            std::istringstream list(keys);
            std::string key;
            while (std::getline(list, key, '+'))
                for (size_t i = 0; i < g_items.size(); ++i)
                    if (g_items[i].key == trim(key))
                        mask |= 1u << i;
            return mask;
        }

        std::string keysOf(uint32_t mask)
        {
            std::string keys;
            for (size_t i = 0; i < g_items.size(); ++i)
                if (mask & (1u << i))
                    keys += (keys.empty() ? "" : "+") + g_items[i].key;
            return keys;
        }

        // player_cheats.ini: enabled=, all=, items=key+key, pin=<player id>,<on>,<key+key>,<name>
        Settings loadSettings(const std::filesystem::path &path)
        {
            Settings settings;
            settings.mask = allItems();
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
                else if (key == "all")
                    settings.all = value == "1";
                else if (key == "items")
                    settings.mask = maskOf(value);
                else if (key == "pin")
                {
                    std::istringstream fields(value);
                    std::string id, on, items, name;
                    if (!std::getline(fields, id, ',') || !std::getline(fields, on, ',') || !std::getline(fields, items, ','))
                        continue;
                    std::getline(fields, name);
                    const unsigned long number = std::strtoul(trim(id).c_str(), nullptr, 10);
                    if (number == 0u || number > 0xFFFFu)
                        continue;
                    Pin pin;
                    pin.on = trim(on) == "1";
                    pin.mask = maskOf(items);
                    pin.name = trim(name);
                    settings.pins[static_cast<uint16_t>(number)] = std::move(pin);
                }
            }
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            std::ofstream file(g_settingsPath, std::ios::binary | std::ios::trunc);
            file << "# 선수 치트 설정: 게임 메뉴(F2)의 \"선수 치트\" 탭이 씁니다\n"
                 << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                 << "all=" << (settings.all ? 1 : 0) << "\n"
                 << "items=" << keysOf(settings.mask) << "\n";
            for (const auto &[id, pin] : settings.pins)
                file << "pin=" << id << "," << (pin.on ? 1 : 0) << "," << keysOf(pin.mask) << "," << pin.name << "\n";
        }

        // The record's name (Shift-JIS, padded with spaces) as UTF-8 with single spaces.
        std::string nameOf(uint32_t record)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + record + kName, sizeof(sjis));
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
            const int size = name.empty() ? 0
                                          : WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), text,
                                                                static_cast<int>(sizeof(text)), nullptr, nullptr);
            return std::string(text, size > 0 ? static_cast<size_t>(size) : 0u);
#else
            (void)record;
            return std::string();
#endif
        }

        // EE thread, at every vblank after the .pnach patches: find the own team and write the chosen items.
        void frame()
        {
            static unsigned tick = 0u;
            const bool refresh = (++tick % 30u) == 0u;
            std::lock_guard<std::mutex> lock(g_mutex);
            int team = -1;
            uint32_t roster = 0u;
            for (int t = 0; t < kTeams && team < 0; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, kTeamRoster + 4u))
                    continue;
                const uint32_t candidate = read32(object + kTeamRoster);
                if (valid(candidate, kRosterPlayers + 4u * kSlots) && read32(candidate + kRosterClass) == kOwnTeamClass)
                {
                    team = t;
                    roster = candidate;
                }
            }
            if (team != g_team || refresh)
            {
                if (team != g_team)
                    std::fprintf(stderr, "[players] own team: %d\n", team);
                g_team = team;
                g_rows.clear();
                for (int k = 0; team >= 0 && k < kSlots; ++k)
                {
                    const uint32_t record = read32(roster + kRosterPlayers + 4u * static_cast<uint32_t>(k));
                    if (!valid(record, kRecordSize) || read16(record) == 0u)
                        continue;
                    Row row;
                    row.id = read16(record);
                    row.number = g_rdram[record + kNumber];
                    row.age = g_rdram[record + kAge];
                    row.pitcher = g_rdram[record + kPitcher] != 0u;
                    row.name = nameOf(record);
                    g_rows.push_back(std::move(row));
                }
            }
            if (team < 0 || !g_settings.enabled)
                return;
            for (int k = 0; k < kSlots; ++k)
            {
                const uint32_t record = read32(roster + kRosterPlayers + 4u * static_cast<uint32_t>(k));
                if (!valid(record, kRecordSize))
                    continue;
                const uint16_t id = read16(record);
                if (id == 0u)
                    continue; // an empty slot
                const auto pin = g_settings.pins.find(id);
                const bool pinned = pin != g_settings.pins.end();
                if (!(pinned ? pin->second.on : g_settings.all))
                    continue;
                const uint32_t mask = pinned ? pin->second.mask : g_settings.mask;
                for (size_t i = 0; i < g_items.size(); ++i)
                    if (mask & (1u << i))
                        for (const Run &run : g_items[i].runs)
                            std::memcpy(g_rdram + record + run.offset, run.bytes.data(), run.bytes.size());
            }
        }

#if defined(URBAN_PLAYER_MENU)
        uint16_t g_selected = 0u; // what the items show: 0 = the players without a pin, else a pinned player (GS thread)

        // The items of `mask`, a tab per section of player_cheat_items.txt, three to a row.
        void drawItems(const Pcsx2GsUi *ui, uint32_t &mask, bool &changed)
        {
            if (ui->small_button("모두 켜기"))
            {
                mask = allItems();
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("모두 끄기"))
            {
                mask = 0u;
                changed = true;
            }
            ui->same_line();
            ui->text_disabled((std::to_string(std::popcount(mask)) + " / " + std::to_string(g_items.size()) + " 항목").c_str());
            if (!ui->begin_tab_bar("sections"))
                return;
            for (size_t first = 0; first < g_items.size();)
            {
                size_t end = first;
                while (end < g_items.size() && g_items[end].section == g_items[first].section)
                    ++end;
                const std::string &section = g_items[first].section;
                if (ui->begin_tab(section.empty() ? "항목" : section.c_str()))
                {
                    for (size_t i = first; i < end; ++i)
                    {
                        ui->push_id(static_cast<int>(i));
                        if ((i - first) % 3u != 0u)
                            ui->same_line();
                        int on = (mask >> i) & 1u ? 1 : 0;
                        if (ui->checkbox(g_items[i].label.c_str(), &on))
                        {
                            mask = on ? mask | (1u << i) : mask & ~(1u << i);
                            changed = true;
                        }
                        ui->pop_id();
                    }
                    for (size_t i = first; i < end; ++i)
                        if (!g_items[i].note.empty())
                            ui->text_disabled((g_items[i].label + ": " + g_items[i].note).c_str());
                    ui->end_tab();
                }
                first = end;
            }
            ui->end_tab_bar();
        }

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end.
        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 선수 치트가 꺼져 있습니다.");
                return;
            }
            if (g_items.empty())
            {
                ui->text_wrapped(("항목 파일이 없습니다: " + utf8(g_itemsPath)).c_str());
                return;
            }
            Settings settings;
            int team;
            std::vector<Row> rows;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                team = g_team;
                rows = g_rows;
            }
            bool changed = false;

            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("선수 치트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->same_line();
            if (team < 0)
                ui->text_disabled("내 구단을 찾는 중: 세이브를 불러오거나 새 게임을 시작하면 자동으로 찾습니다");
            else
                ui->text_disabled((std::string("내 구단 ") + kTeamNames[team] + " (자동 감지) · 선수 " + std::to_string(rows.size()) +
                                   "명 · 고정 " + std::to_string(settings.pins.size()) + "명")
                                      .c_str());

            // What the items below are for: the players without a pin, or one pinned player.
            if (g_selected != 0u && settings.pins.find(g_selected) == settings.pins.end())
                g_selected = 0u;
            std::vector<uint16_t> targets{0u};
            std::vector<std::string> names{"기본: 고정하지 않은 선수 전원"};
            for (const auto &[id, pin] : settings.pins)
            {
                targets.push_back(id);
                names.push_back("고정: " + (pin.name.empty() ? "ID " + std::to_string(id) : pin.name));
            }
            std::vector<const char *> pointers;
            for (const std::string &name : names)
                pointers.push_back(name.c_str());
            int current = 0;
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i] == g_selected)
                    current = static_cast<int>(i);
            ui->separator_text("능력치 항목");
            ui->set_next_width(300.0f);
            if (ui->combo("편집 대상", &current, pointers.data(), static_cast<int>(pointers.size())))
                g_selected = targets[static_cast<size_t>(current)];
            ui->same_line();
            const auto selected = settings.pins.find(g_selected);
            bool &apply = selected == settings.pins.end() ? settings.all : selected->second.on;
            uint32_t &mask = selected == settings.pins.end() ? settings.mask : selected->second.mask;
            int on = apply ? 1 : 0;
            if (ui->checkbox("적용", &on))
            {
                apply = on != 0;
                changed = true;
            }
            ui->begin_disabled(apply ? 0 : 1);
            drawItems(ui, mask, changed);
            ui->end_disabled();

            ui->separator_text("선수: 고정하면 번호나 자리가 바뀌어도 그 선수만 따라가며 개별 항목을 씁니다");
            ui->begin_child("players", 0.0f, 200.0f);
            for (const Row &row : rows)
            {
                ui->push_id(row.id);
                auto pin = settings.pins.find(row.id);
                int pinned = pin != settings.pins.end() ? 1 : 0;
                if (ui->checkbox("고정", &pinned))
                {
                    if (pinned)
                    {
                        Pin added;
                        added.mask = settings.mask;
                        added.name = row.name;
                        settings.pins[row.id] = added;
                        g_selected = row.id;
                    }
                    else
                        settings.pins.erase(row.id);
                    changed = true;
                    pin = settings.pins.find(row.id);
                }
                ui->same_line();
                char line[160];
                std::snprintf(line, sizeof(line), "%2d번  %s  (%s, %d세)", row.number, row.name.empty() ? "?" : row.name.c_str(),
                              row.pitcher ? "투수" : "야수", row.age);
                ui->text(line);
                ui->same_line();
                if (pin == settings.pins.end())
                    ui->text_disabled(settings.all ? "기본 항목" : "끔");
                else
                {
                    const std::string state = pin->second.on ? "개별 " + std::to_string(std::popcount(pin->second.mask)) + "/" +
                                                                   std::to_string(g_items.size()) + "항목"
                                                             : std::string("개별: 끔");
                    ui->text_colored(0xFF66CCFFu, state.c_str());
                    ui->same_line();
                    if (ui->small_button(g_selected == row.id ? "설정 중" : "설정"))
                        g_selected = row.id;
                }
                ui->pop_id();
            }
            ui->end_child();

            // Pinned players not on the roster now (traded, released, another save): kept for when they are back.
            int away = 0;
            for (auto it = settings.pins.begin(); it != settings.pins.end();)
            {
                bool here = false;
                for (const Row &row : rows)
                    here = here || row.id == it->first;
                if (here || team < 0)
                {
                    ++it;
                    continue;
                }
                if (away++ == 0)
                    ui->separator_text("로스터에 없는 고정 선수 (돌아오면 다시 적용)");
                ui->push_id(100000 + it->first);
                const bool remove = ui->small_button("고정 해제") != 0;
                ui->same_line();
                ui->text_disabled((it->second.name.empty() ? "ID " + std::to_string(it->first) : it->second.name).c_str());
                ui->pop_id();
                if (remove)
                {
                    it = settings.pins.erase(it);
                    changed = true;
                }
                else
                    ++it;
            }

            if (changed)
            {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_settings = settings;
                }
                saveSettings(settings);
            }
        }
#endif
    }

    void install(uint8_t *rdram, const std::filesystem::path &userDir, const std::filesystem::path &exeDir)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        std::error_code error;
        g_itemsPath = userDir / "player_cheat_items.txt";
        if (!std::filesystem::exists(g_itemsPath, error) && userDir != exeDir)
            g_itemsPath = exeDir / "player_cheat_items.txt";
        g_items = loadItems(g_itemsPath);
        g_settingsPath = userDir / "player_cheats.ini";
        g_settings = loadSettings(g_settingsPath);
        std::fprintf(stderr, "[players] %zu items (%s); player cheats %s, %zu pinned%s\n", g_items.size(), utf8(g_itemsPath).c_str(),
                     g_settings.enabled ? "on" : "off", g_settings.pins.size(), g_safeMode ? "; safe start: off" : "");
#if defined(URBAN_PLAYER_MENU)
        ps2x::menu::addPanel("선수 치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode && !g_items.empty())
            ps2x::pnach::addFrameHook(frame);
    }
}
