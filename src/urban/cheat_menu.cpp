#include "urban/cheat_menu.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "urban/korean.h"
#include "urban/mods.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_CHEAT_MENU 1
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
#include <chrono>
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

namespace urban::cheatmenu
{
    namespace
    {
        struct Entry
        {
            std::string file, category, name, japanese, source, note; // UTF-8; file relative to mods, '/' separated
        };
        // A number a cheat lets the user pick (mods/기능별/값.tsv from tools/cheat_groups.py).
        struct Param
        {
            // kind: number; memo (an idea memo by the game's names); plan (a management plan by number); planlist (the
            // loop's plans: stored as u16 IDs 0x0B00 + number, "2823,2820")
            std::string file, address, label, kind, defaultText;
            long long low = 0, high = 0, fallback = 0;
            bool lockable = false; // a data write: held at every frame (고정) or written once
        };

        std::filesystem::path g_configPath;
        uint8_t *g_rdram = nullptr;
        std::vector<Entry> g_entries;
        std::vector<Param> g_params;
        std::vector<std::string> g_categories; // in the order of the list
        bool g_safeMode = false;

        std::mutex g_mutex;
        std::vector<std::string> g_enabled; // UTF-8, as config.ini lists them
        std::map<std::string, std::string> g_values; // [CheatValues]: same(file) + "|" + key (upper case) -> value
        // Korean names (mods/기능별/한국어 이름.tsv, from tools/cheat_names_ko.tsv by the game's numbers): plan -> (name,
        // effect), memo -> (name, stars). Read once at install.
        std::map<int, std::pair<std::string, std::string>> g_planKorean, g_memoKorean;
        std::chrono::steady_clock::time_point g_readAt{};

        bool validUtf8(const std::string &text)
        {
            for (size_t i = 0; i < text.size();)
            {
                const auto c = static_cast<unsigned char>(text[i]);
                const size_t length = c < 0x80u ? 1u : (c >> 5) == 0x6u ? 2u : (c >> 4) == 0xEu ? 3u : (c >> 3) == 0x1Eu ? 4u : 0u;
                if (length == 0u || i + length > text.size())
                    return false;
                for (size_t k = 1; k < length; ++k)
                    if ((static_cast<unsigned char>(text[i + k]) & 0xC0u) != 0x80u)
                        return false;
                i += length;
            }
            return true;
        }

#if defined(_WIN32)
        std::string convert(const std::string &text, UINT from, UINT to)
        {
            const int wide = MultiByteToWideChar(from, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring buffer(static_cast<size_t>(wide), L'\0');
            MultiByteToWideChar(from, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
            const int narrow = WideCharToMultiByte(to, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(narrow), '\0');
            WideCharToMultiByte(to, 0, buffer.data(), wide, result.data(), narrow, nullptr, nullptr);
            return result;
        }
#endif

        // config.ini's names (the launcher writes them in the ANSI code page) as UTF-8, and back
        std::string toUtf8(const std::string &text)
        {
#if defined(_WIN32)
            if (!validUtf8(text))
                return convert(text, CP_ACP, CP_UTF8);
#endif
            return text;
        }

        std::string toConfig(const std::string &utf8)
        {
#if defined(_WIN32)
            return convert(utf8, CP_UTF8, CP_ACP);
#else
            return utf8;
#endif
        }

        std::string same(std::string name) // compare names case-blind with '/' separators
        {
            std::replace(name.begin(), name.end(), '\\', '/');
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return name;
        }

        std::vector<std::string> splitList(const std::string &list)
        {
            std::vector<std::string> names;
            std::istringstream stream(list);
            std::string name;
            while (std::getline(stream, name, ';'))
            {
                name.erase(0, name.find_first_not_of(" \t"));
                name.erase(name.find_last_not_of(" \t\r") + 1);
                if (!name.empty())
                    names.push_back(toUtf8(name));
            }
            return names;
        }

        std::string upper(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            return text;
        }

        // config.ini [CheatValues] "<file>|<key>=<value>" (file as Enabled lists it, in the ANSI code page).
        std::map<std::string, std::string> readValues()
        {
            std::map<std::string, std::string> values;
            std::ifstream in(g_configPath, std::ios::binary);
            std::string line, section;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos)
                    continue;
                if (line[start] == '[')
                {
                    section = same(line.substr(start + 1, line.find(']') - start - 1));
                    continue;
                }
                const size_t equals = line.find('='), bar = line.rfind('|', equals);
                if (section != "cheatvalues" || line[start] == '#' || line[start] == ';' || equals == std::string::npos || bar == std::string::npos)
                    continue;
                std::string value = line.substr(equals + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                std::string key = line.substr(bar + 1, equals - bar - 1);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                values[same(toUtf8(line.substr(start, bar - start))) + "|" + upper(key)] = value;
            }
            return values;
        }

        void refreshEnabled()
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_mutex);
            if (now - g_readAt < std::chrono::milliseconds(500))
                return;
            g_readAt = now;
            g_enabled = splitList(urban::mods::readEnabled(g_configPath));
            g_values = readValues();
        }

        bool isEnabled(const std::string &file)
        {
            const std::string key = same(file);
            std::lock_guard<std::mutex> lock(g_mutex);
            return std::any_of(g_enabled.begin(), g_enabled.end(), [&](const std::string &n) { return same(n) == key; });
        }

        // Rewrites the Enabled line of config.ini's [Mods] section (added when missing); the other lines stay as they are.
        bool writeEnabled(const std::vector<std::string> &names)
        {
            std::string joined;
            for (const std::string &name : names)
                joined += (joined.empty() ? "" : ";") + toConfig(name);
            std::vector<std::string> lines;
            {
                std::ifstream in(g_configPath, std::ios::binary);
                std::string line;
                while (std::getline(in, line))
                    lines.push_back(line);
            }
            const bool crlf = !lines.empty() && !lines.front().empty() && lines.front().back() == '\r';
            std::string section;
            bool written = false;
            int modsEnd = -1;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string text = lines[i];
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                const size_t start = text.find_first_not_of(" \t");
                if (start != std::string::npos && text[start] == '[')
                {
                    section = same(text.substr(start + 1, text.find(']') - start - 1));
                    continue;
                }
                if (section != "mods")
                    continue;
                modsEnd = static_cast<int>(i);
                const size_t equals = text.find('=');
                if (start == std::string::npos || text[start] == '#' || text[start] == ';' || equals == std::string::npos)
                    continue;
                std::string key = text.substr(start, equals - start);
                key.erase(key.find_last_not_of(" \t") + 1);
                if (same(key) == "enabled" && !written)
                {
                    lines[i] = "Enabled=" + joined + (crlf ? "\r" : "");
                    written = true;
                }
            }
            if (!written)
            {
                if (modsEnd < 0)
                {
                    lines.push_back(crlf ? "\r" : "");
                    lines.push_back(std::string("[Mods]") + (crlf ? "\r" : ""));
                    modsEnd = static_cast<int>(lines.size()) - 1;
                }
                lines.insert(lines.begin() + modsEnd + 1, "Enabled=" + joined + (crlf ? "\r" : ""));
            }
            // A whole new file, then one rename: the watcher never reads half of it
            const std::filesystem::path temporary = g_configPath.string() + ".cheats.tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                for (const std::string &line : lines)
                    out << line << '\n';
                if (!out)
                    return false;
            }
            std::error_code error;
            std::filesystem::rename(temporary, g_configPath, error);
            return !error;
        }

        void setEnabled(const std::string &file, bool on)
        {
            std::vector<std::string> names;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                names = g_enabled;
            }
            const std::string key = same(file);
            names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string &n) { return same(n) == key; }), names.end());
            if (on)
                names.push_back(file);
            if (writeEnabled(names))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_enabled = names;
                g_readAt = std::chrono::steady_clock::now();
            }
            std::fprintf(stderr, "[cheats] %s %s\n", on ? "on:" : "off:", file.c_str());
        }

        // Sets "<file>|<key>=<value>" in config.ini [CheatValues] (the section is added when missing); the other lines stay.
        bool writeValue(const std::string &file, const std::string &key, const std::string &value)
        {
            std::vector<std::string> lines;
            {
                std::ifstream in(g_configPath, std::ios::binary);
                std::string line;
                while (std::getline(in, line))
                    lines.push_back(line);
            }
            const bool crlf = !lines.empty() && !lines.front().empty() && lines.front().back() == '\r';
            const std::string cr = crlf ? "\r" : "";
            const std::string wanted = same(file) + "|" + upper(key);
            const std::string written = toConfig(file) + "|" + key + "=" + value + cr;
            std::string section;
            int sectionEnd = -1;
            bool done = false;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string text = lines[i];
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                const size_t start = text.find_first_not_of(" \t");
                if (start != std::string::npos && text[start] == '[')
                {
                    section = same(text.substr(start + 1, text.find(']') - start - 1));
                    if (section == "cheatvalues")
                        sectionEnd = static_cast<int>(i);
                    continue;
                }
                if (section != "cheatvalues")
                    continue;
                sectionEnd = static_cast<int>(i);
                const size_t equals = text.find('='), bar = text.rfind('|', equals);
                if (start == std::string::npos || equals == std::string::npos || bar == std::string::npos)
                    continue;
                std::string name = text.substr(start, bar - start), field = text.substr(bar + 1, equals - bar - 1);
                field.erase(0, field.find_first_not_of(" \t"));
                field.erase(field.find_last_not_of(" \t") + 1);
                if (!done && same(toUtf8(name)) + "|" + upper(field) == wanted)
                {
                    lines[i] = written;
                    done = true;
                }
            }
            if (!done)
            {
                if (sectionEnd < 0)
                {
                    lines.push_back(cr);
                    lines.push_back("[CheatValues]" + cr);
                    sectionEnd = static_cast<int>(lines.size()) - 1;
                }
                lines.insert(lines.begin() + sectionEnd + 1, written);
            }
            const std::filesystem::path temporary = g_configPath.string() + ".cheats.tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                for (const std::string &line : lines)
                    out << line << '\n';
                if (!out)
                    return false;
            }
            std::error_code error;
            std::filesystem::rename(temporary, g_configPath, error);
            return !error;
        }

        void setValue(const std::string &file, const std::string &key, const std::string &value)
        {
            if (!writeValue(file, key, value))
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_values[same(file) + "|" + upper(key)] = value;
            g_readAt = std::chrono::steady_clock::now();
        }

        // The game's idea memo names (SLPM-65853 ELF data, the same address in guest RAM: 83 pointers to Shift-JIS).
        const std::vector<std::string> &memoNames()
        {
            static std::vector<std::string> names;
            if (!names.empty() || !g_rdram)
                return names;
            std::vector<std::string> found;
            for (uint32_t i = 0; i < 83u; ++i)
            {
                uint32_t pointer;
                std::memcpy(&pointer, g_rdram + 0x5CD520u + 4u * i, sizeof(pointer));
                if (pointer < 0x100000u || pointer >= 0x2000000u - 64u)
                    return names; // not loaded yet
                const char *text = urban::korean::japanese(reinterpret_cast<const char *>(g_rdram + pointer));
#if defined(_WIN32)
                const std::string japanese = convert(std::string(text, strnlen(text, 48)), 932, CP_UTF8);
#else
                const std::string japanese;
#endif
                const auto korean = g_memoKorean.find(static_cast<int>(i));
                found.push_back(std::to_string(i) + "  " +
                                (korean != g_memoKorean.end() ? korean->second.first + (korean->second.second.empty() ? "" : " " + korean->second.second) +
                                                                     (japanese.empty() ? "" : "  (" + japanese + ")")
                                                              : japanese));
            }
            names = std::move(found);
            return names;
        }

        // The management plans the game has loaded (plan.tbb: [0x5CD51C] -> "TBL1" with the data size at +8, records of
        // 0x88 bytes from +0x10, the Shift-JIS name first), by plan number; the loop list holds 0x0B00 + number. Also
        // written to mods/기능별/경영기획 이름.txt for the launcher, which cannot read the game's memory.
        std::mutex g_planMutex;
        std::vector<std::string> g_plans; // found once (EE thread), read by the menu (GS thread)

        std::vector<std::string> planNames()
        {
            std::lock_guard<std::mutex> lock(g_planMutex);
            return g_plans;
        }

        // EE thread, at every vblank: every 2 s until the game has loaded the plan table.
        void findPlans()
        {
            static auto lastTry = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(g_planMutex);
                if (!g_plans.empty())
                    return;
            }
            if (!g_rdram || now - lastTry < std::chrono::seconds(2))
                return;
            lastTry = now;
            uint32_t table, size;
            std::memcpy(&table, g_rdram + 0x5CD51Cu, sizeof(table));
            if (table < 0x100000u || table >= 0x2000000u - 0x10u || std::memcmp(g_rdram + table, "TBL1", 4) != 0)
                return;
            std::memcpy(&size, g_rdram + table + 8u, sizeof(size));
            const uint32_t count = std::min<uint32_t>(size / 0x88u, 400u);
            if (count == 0u || table + 0x10u + count * 0x88u > 0x2000000u)
                return;
            std::vector<std::string> found;
            std::string file;
            for (uint32_t i = 0; i < count; ++i)
            {
                const char *text = reinterpret_cast<const char *>(g_rdram + table + 0x10u + i * 0x88u);
                // With the Korean translation on, the table may already hold Korean names (as kanji codes): the launcher
                // list keeps the game's Japanese names.
                std::string sjis = urban::korean::original(table + 0x10u + i * 0x88u);
                if (sjis.empty())
                    sjis.assign(text, strnlen(text, 0x28));
#if defined(_WIN32)
                const std::string name = convert(sjis, 932, CP_UTF8);
#else
                const std::string name = std::to_string(i);
#endif
                found.push_back(name);
                file += name + "\n";
            }
            {
                std::lock_guard<std::mutex> lock(g_planMutex);
                g_plans = std::move(found);
            }
            std::fprintf(stderr, "[cheats] %u management plans found\n", count);
            const std::filesystem::path list =
                g_configPath.parent_path() / "mods" / std::filesystem::path(u8"기능별") / std::filesystem::path(u8"경영기획 이름.txt");
            std::ifstream before(list, std::ios::binary);
            std::ostringstream old;
            old << before.rdbuf();
            before.close();
            if (old.str() != file)
                std::ofstream(list, std::ios::binary | std::ios::trunc) << file;
        }

        void loadKorean(const std::filesystem::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::vector<std::string> fields;
                std::istringstream stream(line);
                std::string field;
                while (std::getline(stream, field, '\t'))
                    fields.push_back(field);
                if (fields.size() < 3u || line[0] == '#')
                    continue;
                fields.resize(4u);
                const int number = std::atoi(fields[1].c_str());
                if (fields[0] == "plan")
                    g_planKorean[number] = {fields[2], fields[3]};
                else if (fields[0] == "memo")
                    g_memoKorean[number] = {fields[2], fields[3]};
            }
        }

        // "7  플래그 타운 계획 (フラッグタウン計画) · 교통·생활·주목도 ↑": number, Korean, the game's own name, effect.
        std::string planLabel(int number, const std::vector<std::string> &names)
        {
            const std::string japanese = number >= 0 && number < static_cast<int>(names.size()) ? names[static_cast<size_t>(number)] : "";
            const auto korean = g_planKorean.find(number);
            std::string label = std::to_string(number) + "  ";
            if (korean != g_planKorean.end())
                label += korean->second.first + (japanese.empty() ? "" : " (" + japanese + ")") +
                         (korean->second.second.empty() ? "" : " · " + korean->second.second);
            else
                label += japanese.empty() ? "기획 " + std::to_string(number) : japanese;
            return label;
        }

        std::vector<int> numberList(const std::string &text)
        {
            std::vector<int> numbers;
            std::istringstream items(text);
            std::string item;
            while (std::getline(items, item, ','))
                if (item.find_first_of("0123456789") != std::string::npos)
                    numbers.push_back(std::atoi(item.c_str()));
            return numbers;
        }

        std::vector<Param> loadParams(const std::filesystem::path &path)
        {
            std::vector<Param> params;
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::vector<std::string> fields;
                std::istringstream stream(line);
                std::string field;
                while (std::getline(stream, field, '\t'))
                    fields.push_back(field);
                if (fields.size() < 7u)
                    continue;
                Param param;
                param.file = fields[0];
                param.address = fields[1];
                param.label = fields[2];
                param.low = std::atoll(fields[3].c_str());
                param.high = std::atoll(fields[4].c_str());
                param.fallback = std::atoll(fields[5].c_str());
                param.defaultText = fields[5];
                param.kind = fields[6];
                param.lockable = fields.size() > 7u && fields[7] == "1";
                params.push_back(std::move(param));
            }
            return params;
        }

        std::vector<Entry> loadList(const std::filesystem::path &path)
        {
            std::vector<Entry> entries;
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::vector<std::string> fields;
                std::istringstream stream(line);
                std::string field;
                while (std::getline(stream, field, '\t'))
                    fields.push_back(field);
                if (fields.size() < 3u)
                    continue;
                fields.resize(6u);
                entries.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]});
            }
            return entries;
        }

#if defined(URBAN_CHEAT_MENU)
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

        // The loop's plans, top to bottom (the game runs the next one each time a plan is carried out, then starts over):
        // remove, move up, add by name. At least one stays: an empty list would keep the game's loop from ever ending.
        void drawPlanList(const Pcsx2GsUi *ui, const std::string &file, const Param &param, const std::string &stored)
        {
            std::vector<int> plans;
            if (!stored.empty())
                for (int id : numberList(stored))
                    plans.push_back(id - 0x0B00);
            else
                plans = numberList(param.defaultText);
            const std::vector<std::string> names = planNames();
            const auto nameOf = [&](int n) { return planLabel(n, names); };
            ui->text((param.label + " · " + std::to_string(plans.size()) + "개").c_str());
            bool changed = false;
            for (size_t k = 0; k < plans.size(); ++k)
            {
                ui->push_id(static_cast<int>(k));
                ui->begin_disabled(plans.size() <= 1u ? 1 : 0);
                const bool remove = ui->small_button("빼기") != 0;
                ui->end_disabled();
                ui->same_line();
                ui->begin_disabled(k == 0u ? 1 : 0);
                const bool up = ui->small_button("위로") != 0;
                ui->end_disabled();
                ui->same_line();
                ui->text(nameOf(plans[k]).c_str());
                ui->pop_id();
                if (remove || up)
                {
                    if (remove)
                        plans.erase(plans.begin() + static_cast<std::ptrdiff_t>(k));
                    else
                        std::swap(plans[k], plans[k - 1u]);
                    changed = true;
                    break;
                }
            }
            const size_t known = names.empty() ? g_planKorean.size() : names.size(); // the game's, else the Korean table's
            if (known > 0u && plans.size() < 30u)
            {
                std::vector<std::string> labels{"+ 추가할 기획 고르기"};
                for (size_t i = 0; i < known; ++i)
                    labels.push_back(nameOf(static_cast<int>(i)));
                std::vector<const char *> pointers;
                for (const std::string &label : labels)
                    pointers.push_back(label.c_str());
                int pick = 0;
                ui->set_next_width(320.0f);
                if (ui->combo("##addplan", &pick, pointers.data(), static_cast<int>(pointers.size())) && pick > 0)
                {
                    plans.push_back(pick - 1);
                    changed = true;
                }
                ui->same_line();
            }
            else if (known == 0u)
                ui->text_disabled("기획 이름은 게임이 기획 표를 읽은 뒤에 나옵니다 (그 전에는 번호만).");
            if (ui->small_button("기본 목록"))
            {
                plans = numberList(param.defaultText);
                changed = true;
            }
            if (changed && !plans.empty())
            {
                std::string text;
                for (int n : plans)
                    text += (text.empty() ? "" : ",") + std::to_string(0x0B00 + n);
                setValue(file, param.address, text);
            }
        }

        // The numbers of one cheat: buttons by steps (the menu has no text input; the launcher has number boxes), the
        // idea memo by name, and 고정 for data writes. Saved in config.ini [CheatValues]; the game picks them up within a
        // second, also while the cheat is off (they apply when it is switched on).
        void drawParams(const Pcsx2GsUi *ui, const Entry &entry, const std::map<std::string, std::string> &values)
        {
            bool lockable = false;
            for (size_t p = 0; p < g_params.size(); ++p)
            {
                const Param &param = g_params[p];
                if (same(param.file) != same(entry.file))
                    continue;
                lockable = lockable || param.lockable;
                const auto stored = values.find(same(entry.file) + "|" + upper(param.address));
                long long value = stored != values.end() ? std::atoll(stored->second.c_str()) : param.fallback;
                value = std::clamp(value, param.low, param.high);
                ui->push_id(2000 + static_cast<int>(p));
                ui->indent();
                if (param.kind == "planlist")
                    drawPlanList(ui, entry.file, param, stored != values.end() ? stored->second : std::string());
                else if ((param.kind == "memo" && !memoNames().empty()) || (param.kind == "plan" && (!planNames().empty() || !g_planKorean.empty())))
                {
                    std::vector<std::string> labels;
                    if (param.kind == "memo")
                        labels = memoNames();
                    else
                    {
                        const std::vector<std::string> plans = planNames();
                        for (size_t i = 0; i < (plans.empty() ? g_planKorean.size() : plans.size()); ++i)
                            labels.push_back(planLabel(static_cast<int>(i), plans));
                    }
                    std::vector<const char *> pointers;
                    for (const std::string &label : labels)
                        pointers.push_back(label.c_str());
                    int current = static_cast<int>(std::min<long long>(value, static_cast<long long>(labels.size()) - 1));
                    ui->set_next_width(300.0f);
                    if (ui->combo(param.label.c_str(), &current, pointers.data(), static_cast<int>(pointers.size())))
                        setValue(entry.file, param.address, std::to_string(current));
                }
                else
                {
                    ui->text((param.label + ": " + grouped(value)).c_str());
                    const long long range = param.high - param.low;
                    const std::vector<long long> steps = range > 999999 ? std::vector<long long>{10000000, 1000000, 100000, 10000}
                                                         : range > 999   ? std::vector<long long>{1000, 100, 10, 1}
                                                                         : std::vector<long long>{10, 1};
                    long long next = value;
                    for (long long step : steps)
                    {
                        ui->same_line();
                        if (ui->small_button(("-" + grouped(step)).c_str()))
                            next = value - step;
                    }
                    for (auto it = steps.rbegin(); it != steps.rend(); ++it)
                    {
                        ui->same_line();
                        if (ui->small_button(("+" + grouped(*it)).c_str()))
                            next = value + *it;
                    }
                    ui->same_line();
                    if (ui->small_button("최소"))
                        next = param.low;
                    ui->same_line();
                    if (ui->small_button("최대"))
                        next = param.high;
                    ui->same_line();
                    if (ui->small_button("기본"))
                        next = param.fallback;
                    next = std::clamp(next, param.low, param.high);
                    if (next != value)
                        setValue(entry.file, param.address, std::to_string(next));
                }
                ui->unindent();
                ui->pop_id();
            }
            if (lockable)
            {
                const auto lock = values.find(same(entry.file) + "|LOCK");
                int held = lock == values.end() || lock->second != "0" ? 1 : 0;
                ui->indent();
                if (ui->checkbox("고정 (매 프레임 다시 씀; 끄면 켤 때·바꿀 때 한 번만)", &held))
                    setValue(entry.file, "lock", held ? "1" : "0");
                ui->unindent();
            }
        }

        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 치트가 꺼져 있습니다.");
                return;
            }
            if (g_entries.empty())
            {
                ui->text_wrapped("기능별 치트 목록(mods/기능별/목록.tsv)이 없습니다. python tools/cheat_groups.py 가 만듭니다.");
                return;
            }
            refreshEnabled();
            std::map<std::string, std::string> values;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                values = g_values;
            }
            size_t on = 0;
            for (const Entry &entry : g_entries)
                on += isEnabled(entry.file) ? 1u : 0u;
            ui->text_wrapped("치트를 기능별로 켜고 끕니다. 게임 중에도 1초 안에 적용되고, 끄면 게임 코드가 원래대로 돌아갑니다.");
            ui->text_disabled(("켜진 기능 " + std::to_string(on) + " / " + std::to_string(g_entries.size())).c_str());
            ui->same_line();
            if (ui->small_button("기능별 모두 끄기"))
                for (const Entry &entry : g_entries)
                    if (isEnabled(entry.file))
                        setEnabled(entry.file, false);
            // Whole files still enabled (the launcher's list): their functions would run twice
            std::vector<std::string> wholeFiles;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const std::string &name : g_enabled)
                    if (std::none_of(g_entries.begin(), g_entries.end(), [&](const Entry &e) { return same(e.file) == same(name); }))
                        wholeFiles.push_back(name);
            }
            if (!ui->begin_tab_bar("cheat categories"))
                return;
            for (size_t c = 0; c < g_categories.size(); ++c)
            {
                if (!ui->begin_tab(g_categories[c].c_str()))
                    continue;
                ui->begin_child("cheat list", 0.0f, 0.0f);
                for (size_t i = 0; i < g_entries.size(); ++i)
                {
                    const Entry &entry = g_entries[i];
                    if (entry.category != g_categories[c])
                        continue;
                    ui->push_id(static_cast<int>(i));
                    int value = isEnabled(entry.file) ? 1 : 0;
                    if (ui->checkbox(entry.name.c_str(), &value))
                        setEnabled(entry.file, value != 0);
                    std::string detail = entry.japanese;
                    if (!entry.note.empty())
                        detail += (detail.empty() ? "" : " · ") + entry.note;
                    if (!detail.empty())
                    {
                        ui->same_line();
                        ui->text_disabled(detail.c_str());
                    }
                    drawParams(ui, entry, values);
                    ui->pop_id();
                }
                ui->end_child();
                ui->end_tab();
            }
            if (!wholeFiles.empty() && ui->begin_tab("파일째 켠 것"))
            {
                ui->text_wrapped("런처에서 파일째 켠 치트입니다. 같은 기능을 위 목록에서 켜면 두 번 돌므로, 기능별로 쓰려면 여기서 끄세요.");
                for (size_t i = 0; i < wholeFiles.size(); ++i)
                {
                    ui->push_id(100000 + static_cast<int>(i));
                    int value = 1;
                    if (ui->checkbox(wholeFiles[i].c_str(), &value) && !value)
                        setEnabled(wholeFiles[i], false);
                    ui->pop_id();
                }
                ui->end_tab();
            }
            ui->end_tab_bar();
        }
#endif
    }

    void install(const std::filesystem::path &configPath, uint8_t *rdram)
    {
        g_configPath = configPath;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start (as the player cheats tab)
        const std::filesystem::path list =
            configPath.parent_path() / "mods" / std::filesystem::path(u8"기능별") / std::filesystem::path(u8"목록.tsv");
        g_entries = loadList(list);
        g_params = loadParams(list.parent_path() / std::filesystem::path(u8"값.tsv"));
        loadKorean(list.parent_path() / std::filesystem::path(u8"한국어 이름.tsv"));
        for (const Entry &entry : g_entries)
            if (std::find(g_categories.begin(), g_categories.end(), entry.category) == g_categories.end())
                g_categories.push_back(entry.category);
        std::fprintf(stderr, "[cheats] %zu cheat functions in %zu categories, %zu values\n", g_entries.size(), g_categories.size(),
                     g_params.size());
#if defined(URBAN_CHEAT_MENU)
        ps2x::menu::addPanel("치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode && !g_params.empty())
            ps2x::pnach::addFrameHook(findPlans); // the plan names for the menu and the launcher
    }
}
