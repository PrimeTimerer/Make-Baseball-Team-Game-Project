#include "urban/cheat_menu.h"

#include "runtime/ps2_game_menu.h"
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

        std::filesystem::path g_configPath;
        std::vector<Entry> g_entries;
        std::vector<std::string> g_categories; // in the order of the list
        bool g_safeMode = false;

        std::mutex g_mutex;
        std::vector<std::string> g_enabled; // UTF-8, as config.ini lists them
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

        void refreshEnabled()
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_mutex);
            if (now - g_readAt < std::chrono::milliseconds(500))
                return;
            g_readAt = now;
            g_enabled = splitList(urban::mods::readEnabled(g_configPath));
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

    void install(const std::filesystem::path &configPath)
    {
        g_configPath = configPath;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start (as the player cheats tab)
        const std::filesystem::path list =
            configPath.parent_path() / "mods" / std::filesystem::path(u8"기능별") / std::filesystem::path(u8"목록.tsv");
        g_entries = loadList(list);
        for (const Entry &entry : g_entries)
            if (std::find(g_categories.begin(), g_categories.end(), entry.category) == g_categories.end())
                g_categories.push_back(entry.category);
        std::fprintf(stderr, "[cheats] %zu cheat functions in %zu categories\n", g_entries.size(), g_categories.size());
#if defined(URBAN_CHEAT_MENU)
        ps2x::menu::addPanel("치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
