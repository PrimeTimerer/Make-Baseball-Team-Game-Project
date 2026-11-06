#pragma once
// The F2 cheat tabs' refresh line and change log: when the game's data was read last (with the game date), a button to
// read it now, and what changed between reads (players joining or leaving, scouting deals, finds, one-time edits), so a
// change can be checked in the game.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_CHANGE_LOG_MENU 1
#endif

namespace urban
{
    // YKTK3's club object ([0x62F190], the same after a new game and a load): year +0x13D, week +0x140; +0x13F the month
    // in the season (+0x13E = 0xFF: 4 = 4月), but in the off-season (+0x13E = 1) the months since October (1 = 11月,
    // 2 = 12月, 3 = 1月). Checked on saves at 4月 4週, 11月 2-3週 and 12月 1週.
    inline bool gameDate(const uint8_t *rdram, int &year, int &month, int &week)
    {
        constexpr uint32_t kClubPointer = 0x62F190u, kRamSize = 0x2000000u;
        if (!rdram)
            return false;
        uint32_t club = 0u;
        std::memcpy(&club, rdram + kClubPointer, 4);
        if (club < 0x100000u || club >= kRamSize - 0x200u || (club & 3u) != 0u)
            return false;
        year = rdram[club + 0x13Du];
        month = rdram[club + 0x13Fu];
        week = rdram[club + 0x140u];
        if (rdram[club + 0x13Eu] == 1u && month >= 1 && month <= 12)
            month = (month + 9) % 12 + 1;
        return year >= 1 && year <= 99 && month >= 1 && month <= 12 && week >= 1 && week <= 5;
    }
    inline std::string gameDateText(const uint8_t *rdram)
    {
        int year = 0, month = 0, week = 0;
        if (!gameDate(rdram, year, month, week))
            return std::string();
        char text[40];
        std::snprintf(text, sizeof(text), "%d년차 %d월 %d주", year, month, week);
        return text;
    }

    // "a, b, c 외 N명": the first few names of a list.
    inline std::string joinNames(const std::vector<std::string> &names, size_t shown = 6u)
    {
        std::string text;
        for (size_t k = 0; k < names.size() && k < shown; ++k)
            text += (k ? ", " : "") + names[k];
        if (names.size() > shown)
            text += " 외 " + std::to_string(names.size() - shown) + "명";
        return text;
    }

    // Used under its owner's mutex (the frame hook writes, the menu copies).
    struct ChangeLog
    {
        struct Line
        {
            std::string date, clock, text;
        };
        std::deque<Line> lines; // newest first
        std::chrono::steady_clock::time_point readAt{};
        std::string readDate;
        unsigned reads = 0u;

        void read(const uint8_t *rdram)
        {
            readAt = std::chrono::steady_clock::now();
            readDate = gameDateText(rdram);
            ++reads;
        }
        void add(const std::string &text, const char *tag)
        {
            char clock[16] = "";
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
            lines.push_front(Line{readDate, clock, text});
            while (lines.size() > 80u)
                lines.pop_back();
            std::fprintf(stderr, "[%s] change (%s): %s\n", tag, readDate.c_str(), text.c_str());
        }
    };

#if defined(URBAN_CHANGE_LOG_MENU)
    // "새로고침" and "<what> 갱신: 0.3초 전 · 게임 날짜 1년차 12월 1주". Returns true when the button was pressed.
    inline bool drawRefresh(const Pcsx2GsUi *ui, const ChangeLog &log, const char *what)
    {
        const bool pressed = ui->small_button("새로고침") != 0;
        ui->tooltip("게임 메모리를 지금 다시 읽습니다. 평소에도 0.5초마다 저절로 읽습니다.");
        ui->same_line();
        if (log.reads == 0u)
        {
            ui->text_disabled((std::string(what) + ": 아직 읽지 않음 (세이브를 불러오거나 새 게임을 시작하면 읽습니다)").c_str());
            return pressed;
        }
        const double ago = std::chrono::duration<double>(std::chrono::steady_clock::now() - log.readAt).count();
        char text[200];
        std::snprintf(text, sizeof(text), "%s 갱신: %.1f초 전%s%s", what, ago, log.readDate.empty() ? "" : " · 게임 날짜 ",
                      log.readDate.c_str());
        if (ago > 3.0)
            ui->text_colored(0xFF5050FFu, text); // not read for a while (no save loaded, or the game is stopped)
        else
            ui->text_disabled(text);
        return pressed;
    }

    // The change log, newest first, in a fold. Returns true when 지우기 was pressed.
    inline bool drawChanges(const Pcsx2GsUi *ui, const ChangeLog &log, const char *id, float height)
    {
        const std::string label = "최근 변화 (" + std::to_string(log.lines.size()) + ")##" + id;
        if (!ui->collapsing_header(label.c_str(), 0))
            return false;
        bool clear = false;
        if (log.lines.empty())
            ui->text_disabled("아직 바뀐 것이 없습니다. 게임을 진행하거나 치트를 적용하면 여기에 쌓입니다.");
        else
        {
            clear = ui->small_button("지우기") != 0;
            ui->same_line();
            ui->text_disabled("새것이 위. 게임 날짜 · 실제 시각 · 내용 (0.5초마다 비교)");
            if (ui->begin_child(id, 0.0f, height))
            {
                for (const ChangeLog::Line &line : log.lines)
                    ui->text(((line.date.empty() ? std::string() : line.date + " · ") + line.clock + "  " + line.text).c_str());
            }
            ui->end_child();
        }
        return clear;
    }
#endif
}
