#include "urban/mods.h"


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace urban::mods
{
    namespace
    {
        std::string trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // "[name]" -> "name" (lower case), else "".
        std::string sectionOf(const std::string &line)
        {
            const std::string text = trim(line);
            return text.size() >= 2u && text.front() == '[' && text.back() == ']' ? lower(trim(text.substr(1, text.size() - 2))) : "";
        }

        // "key=value" with key = Enabled (any case, not commented out).
        bool isEnabledLine(const std::string &line, std::string *value)
        {
            const std::string text = trim(line);
            const size_t equals = text.find('=');
            if (text.empty() || text[0] == '#' || text[0] == ';' || equals == std::string::npos ||
                lower(trim(text.substr(0, equals))) != "enabled")
                return false;
            if (value)
                *value = trim(text.substr(equals + 1));
            return true;
        }

        std::vector<std::string> split(const std::string &list)
        {
            std::vector<std::string> names;
            std::istringstream stream(list);
            std::string name;
            while (std::getline(stream, name, ';'))
                if (!(name = trim(name)).empty())
                    names.push_back(name);
            return names;
        }

    }

    std::string readEnabled(const std::filesystem::path &configPath)
    {
        std::ifstream file(configPath);
        std::string line, section, enabled, value;
        while (std::getline(file, line))
        {
            if (const std::string name = sectionOf(line); !name.empty())
                section = name;
            else if (section == "mods" && isEnabledLine(line, &value))
                enabled = value;
        }
        return enabled;
    }

    std::filesystem::path pathFromConfig(const std::string &text)
    {
        // Valid UTF-8 with non-ASCII bytes is UTF-8 (Korean in CP949 never is: its lead bytes are UTF-8 continuation
        // bytes); anything else is the ANSI code page. A character neither can map gives an empty path, not an
        // exception (std::filesystem throws on one, and at startup that ended the game).
        bool ascii = true, utf8 = true;
        for (size_t i = 0; i < text.size() && utf8;)
        {
            const auto c = static_cast<unsigned char>(text[i]);
            const size_t length = c < 0x80u ? 1u : (c >> 5) == 0x6u ? 2u : (c >> 4) == 0xEu ? 3u : (c >> 3) == 0x1Eu ? 4u : 0u;
            if (length == 0u || i + length > text.size())
                utf8 = false;
            for (size_t k = 1; utf8 && k < length; ++k)
                utf8 = (static_cast<unsigned char>(text[i + k]) & 0xC0u) == 0x80u;
            ascii = ascii && length == 1u;
            i += length ? length : 1u;
        }
        try
        {
            if (!ascii && utf8)
                return std::filesystem::path(std::u8string(text.begin(), text.end()));
            return std::filesystem::path(text);
        }
        catch (const std::exception &)
        {
            return {};
        }
    }

    namespace
    {
        // A name as config.ini has it, for comparing: the path it means, lower case, '/' separated.
        std::wstring nameKey(const std::string &name)
        {
            std::wstring key = pathFromConfig(name).generic_wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return key;
        }

        // config.ini [CheatValues]: "<name as in Enabled>|<address as the .pnach line has it>=<number>" picks a number of a
        // cheat (the F2 치트 tab and the launcher write them), "<name>|lock=0" writes a data cheat once instead of at
        // every frame. By name key, then key upper case.
        std::map<std::wstring, std::map<std::string, std::string>> readValues(const std::filesystem::path &configPath)
        {
            std::map<std::wstring, std::map<std::string, std::string>> values;
            std::ifstream file(configPath);
            std::string line, section;
            while (std::getline(file, line))
            {
                if (const std::string name = sectionOf(line); !name.empty())
                {
                    section = name;
                    continue;
                }
                if (section != "cheatvalues")
                    continue;
                const std::string text = trim(line);
                const size_t equals = text.find('='), bar = text.rfind('|', equals);
                if (text.empty() || text[0] == '#' || text[0] == ';' || equals == std::string::npos || bar == std::string::npos)
                    continue;
                std::string key = trim(text.substr(bar + 1, equals - bar - 1));
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                values[nameKey(trim(text.substr(0, bar)))][key] = trim(text.substr(equals + 1));
            }
            return values;
        }

        // The .pnach text with the picked numbers: a word that loads a number (addiu/ori/lui) keeps its instruction and
        // takes the number's low half, any other line the whole number; once, the lines are written once (place 0).
        std::string withValues(const std::string &text, const std::map<std::string, std::string> &values)
        {
            const auto lock = values.find("LOCK");
            const bool once = lock != values.end() && lock->second == "0";
            // A list ("2823,2820,..."): u16 values from that address, then 0 (a loop's end), in place of the file's lines
            // within 0x100 bytes of it (the 지정 경영기획 무한 루프 plan list).
            struct List
            {
                unsigned long long address = 0;
                std::vector<unsigned> items;
                bool written = false;
            };
            std::vector<List> lists;
            for (const auto &[key, value] : values)
                if (value.find(',') != std::string::npos)
                {
                    List list;
                    list.address = std::strtoull(key.c_str(), nullptr, 16);
                    std::istringstream items(value);
                    std::string item;
                    while (std::getline(items, item, ',') && list.items.size() < 64u)
                        if (!trim(item).empty())
                            list.items.push_back(static_cast<unsigned>(std::strtoul(trim(item).c_str(), nullptr, 10)) & 0xFFFFu);
                    lists.push_back(std::move(list));
                }
            const auto listLines = [&](List &list)
            {
                std::string lines;
                std::vector<unsigned> halves = list.items;
                halves.push_back(0u);
                for (size_t i = 0; i < halves.size(); i += 2u)
                {
                    const unsigned word = halves[i] | ((i + 1u < halves.size() ? halves[i + 1u] : 0u) << 16);
                    char text[64];
                    std::snprintf(text, sizeof(text), "patch=%s,EE,%08llX,word,%08X\n", once ? "0" : "1", list.address + 2u * i, word);
                    lines += text;
                }
                list.written = true;
                return lines;
            };
            std::istringstream in(text);
            std::ostringstream out;
            std::string line;
            while (std::getline(in, line))
            {
                const std::string code = trim(line.substr(0, line.find("//")));
                const size_t equals = code.find('=');
                if (code.rfind("patch", 0) == 0u && equals != std::string::npos)
                {
                    std::vector<std::string> fields;
                    std::istringstream rest(code.substr(equals + 1));
                    std::string field;
                    while (std::getline(rest, field, ','))
                        fields.push_back(trim(field));
                    const unsigned long long at = fields.size() >= 5u ? std::strtoull(fields[2].c_str(), nullptr, 16) : 0u;
                    const auto inList = std::find_if(lists.begin(), lists.end(), [&](const List &list)
                                                     { return at >= list.address && at < list.address + 0x100u; });
                    if (inList != lists.end())
                    {
                        if (!inList->written)
                            out << listLines(*inList);
                        continue; // the file's own list
                    }
                    if (fields.size() >= 5u)
                    {
                        std::string address = fields[2];
                        std::transform(address.begin(), address.end(), address.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        if (const auto value = values.find(address); value != values.end())
                        {
                            const unsigned long long original = std::strtoull(fields[4].c_str(), nullptr, 16);
                            const long long number = std::strtoll(value->second.c_str(), nullptr, 10);
                            const unsigned opcode = static_cast<unsigned>(original >> 26) & 0x3Fu;
                            unsigned long long word = static_cast<unsigned long long>(number) & 0xFFFFFFFFull;
                            if (lower(fields[3]) == "word" && (opcode == 0x09u || opcode == 0x0Du || opcode == 0x0Fu))
                                word = (original & 0xFFFF0000ull) | (static_cast<unsigned long long>(number) & 0xFFFFull);
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "%08llX", word);
                            fields[4] = hex;
                        }
                        if (once && fields[0] == "1")
                            fields[0] = "0";
                        line = code.substr(0, equals + 1);
                        for (size_t i = 0; i < fields.size(); ++i)
                            line += (i ? "," : "") + fields[i];
                    }
                }
                out << line << "\n";
            }
            for (List &list : lists)
                if (!list.written)
                    out << listLines(list);
            return out.str();
        }
    }

    std::vector<std::string> pnachFiles(const std::filesystem::path &configPath, const std::string &enabled, bool log)
    {
        std::vector<std::string> files;
        const auto values = readValues(configPath);
        for (const std::string &name : split(enabled))
        {
            const std::filesystem::path relative = pathFromConfig(name);
            std::error_code error;
            const std::filesystem::path file = configPath.parent_path() / "mods" / relative;
            std::string narrow;
            try
            {
                narrow = file.string(); // the pnach loader opens narrow (ANSI) paths
            }
            catch (const std::exception &)
            {
            }
            if (!relative.empty() && !narrow.empty() && std::filesystem::exists(file, error))
            {
                // Picked numbers: the loader reads a copy with them (user folder cheat_values/), rewritten only when it
                // changes, so the reload watcher sees a change of a number as a change of that file.
                const auto picked = values.find(nameKey(name));
                if (picked != values.end() && !picked->second.empty())
                {
                    std::ifstream in(file, std::ios::binary);
                    std::ostringstream original;
                    original << in.rdbuf();
                    const std::string text = withValues(original.str(), picked->second);
                    std::wstring flat = relative.generic_wstring();
                    std::replace(flat.begin(), flat.end(), L'/', L'_');
                    const std::filesystem::path copy = configPath.parent_path() / "cheat_values" / flat;
                    std::ifstream before(copy, std::ios::binary);
                    std::ostringstream old;
                    old << before.rdbuf();
                    before.close();
                    if (old.str() != text)
                    {
                        std::filesystem::create_directories(copy.parent_path(), error);
                        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
                        out << text;
                    }
                    try
                    {
                        files.push_back(copy.string());
                        continue;
                    }
                    catch (const std::exception &)
                    {
                    }
                }
                files.push_back(narrow);
            }
            else if (log)
                std::cerr << "[mods] " << name << (relative.empty() || narrow.empty() ? ": the name cannot be read (config.ini encoding)"
                                                                                          : " is not in the mods folder") << std::endl;
        }
        return files;
    }
}
