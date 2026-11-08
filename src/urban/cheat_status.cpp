#include "urban/cheat_status.h"
#include "urban/save_identity.h"

#include "runtime/ps2_game_menu.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_STATUS_MENU 1
#endif

#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace urban::status
{
    namespace
    {
        // The settings files the tabs write: per save (cheat_saves/<save>/) and config.ini for the 치트 tab (all saves).
        struct Watched
        {
            const char *file;
            const char *tab;
        };
        constexpr Watched kFiles[] = {
            {"player_cheats.ini", "선수 치트"},   {"staff_cheats.ini", "스태프 치트"}, {"ichinen_cheats.ini", "이치넨"},
            {"stadium_cheats.ini", "구장·시설"}, {"match_script.ini", "경기 스크립트"}, {"ability_limits.ini", "99 제한"},
            {"scout_cheats.ini", "스카우트"},       {"lineup_lock.ini", "라인업"},
        };
        constexpr auto kShown = std::chrono::seconds(4); // the message stands out this long, then stays grey

        // GS thread only (the menu draws there).
        std::filesystem::path g_config;
        std::string g_profile;
        uint32_t g_generation = ~0u;
        std::map<std::string, std::filesystem::file_time_type> g_stamps; // file -> the write time last seen
        std::string g_message, g_clock;
        std::chrono::steady_clock::time_point g_at{}, g_lastScan{};

        std::string clockNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[16];
            std::strftime(text, sizeof(text), "%H:%M:%S", &local);
            return text;
        }

        // A file whose write time moved since it was first seen this save: its tab saved, which applies too (the tabs
        // hand their settings to the game when they write them, or read the file again when it changes).
        void scan()
        {
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                g_profile = urban::saveid::current();
                g_stamps.clear(); // another save: its files are seen afresh (loading them is not a change)
            }
            std::vector<std::string> changed;
            auto check = [&changed](const std::filesystem::path &path, const char *tab) {
                std::error_code error;
                const auto time = std::filesystem::last_write_time(path, error);
                if (error)
                    return;
                const auto [it, first] = g_stamps.try_emplace(path.string(), time);
                if (!first && it->second != time)
                {
                    it->second = time;
                    changed.push_back(tab);
                }
            };
            if (!g_profile.empty())
                for (const Watched &watched : kFiles)
                    check(urban::saveid::folder(g_profile) / watched.file, watched.tab);
            if (!g_config.empty())
                check(g_config, "치트(기능별)");
            if (changed.empty())
                return;
            std::string tabs;
            for (const std::string &tab : changed)
                tabs += (tabs.empty() ? "" : ", ") + tab;
            g_message = tabs;
            g_clock = clockNow();
            g_at = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[status] applied: %s (%s)\n", tabs.c_str(), g_profile.empty() ? "no save" : g_profile.c_str());
        }

#if defined(URBAN_STATUS_MENU)
        void draw(const Pcsx2GsUi *ui)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastScan >= std::chrono::milliseconds(400))
            {
                g_lastScan = now;
                scan();
            }
            // Two columns: this save at the left, the last apply in a fixed column at the right edge.
            if (!ui->begin_table("##cheatstatus", 2, 0u, 0.0f, 0.0f))
                return;
            ui->table_column("##save", 0u, 0.0f);
            ui->table_column("##applied", 0u, 560.0f);
            ui->table_next_row();
            ui->table_next_column();
            ui->text_disabled(g_profile.empty() ? "세이브 없음: 게임을 불러오면 그 세이브의 치트 설정이 쓰입니다"
                                                : ("세이브: " + urban::saveid::describe(g_profile) + " · 바꾸는 즉시 이 세이브 설정 파일에 저장·적용").c_str());
            ui->table_next_column();
            if (g_message.empty())
                ui->text_disabled("이번 실행에서 바꾼 설정 없음");
            else if (now - g_at < kShown)
                ui->text_colored(0xFF66EE66u, ("[적용됨] " + g_message + " (" + g_clock + ")").c_str());
            else
                ui->text_disabled(("마지막 적용: " + g_message + " (" + g_clock + ")").c_str());
            ui->end_table();
        }
#endif
    }

    void install(const std::filesystem::path &userDir)
    {
        g_config = userDir / "config.ini";
#if defined(URBAN_STATUS_MENU)
        ps2x::menu::setHeader([](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
