#include "urban/mods.h"


#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
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

    std::vector<std::string> pnachFiles(const std::filesystem::path &configPath, const std::string &enabled, bool log)
    {
        std::vector<std::string> files;
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
                files.push_back(narrow);
            else if (log)
                std::cerr << "[mods] " << name << (relative.empty() || narrow.empty() ? ": the name cannot be read (config.ini encoding)"
                                                                                          : " is not in the mods folder") << std::endl;
        }
        return files;
    }
}
