#include "urban/perf_log.h"
#include "urban/change_log.h"

#include "runtime/ps2_perf_stats.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>

int ps2xSpeedPercent() noexcept; // Kernel/EeScheduler.cpp: 100, 200 (F7) or 0 (turbo, F10)

namespace urban::perflog
{
    namespace
    {
        // YKTK3: the club object, the same after a new game and a loaded save. Money +0x124 (万円), year +0x13D, month
        // +0x13F, week +0x140.
        constexpr uint32_t kClubPointer = 0x62F190u;
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr double kFieldsPerSecond = 59.94;
        constexpr int kMaxSnapshots = 10; // a BMP of the upscaled image is ~8 MB
        constexpr double kSnapshotGapSeconds = 60.0;

        uint8_t *g_rdram = nullptr;
        std::ofstream g_log;
        std::filesystem::path g_folder; // this session's screenshots and VU1 images

        // A path as the narrow (ANSI) string the GS snapshot and the log take; empty when it cannot be one (no throw).
        std::string narrow(const std::filesystem::path &path)
        {
            try
            {
                return path.string();
            }
            catch (const std::exception &)
            {
                return std::string();
            }
        }

        std::string clockText(const char *format)
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), format, &local);
            return text;
        }

        std::string dateText()
        {
            int year = 0, month = 0, week = 0; // the off-season's months counted right (urban/change_log.h)
            if (!urban::gameDate(g_rdram, year, month, week))
                return "날짜 -";
            char text[40];
            std::snprintf(text, sizeof(text), "%d년차 %2d월 %d주", year, month, week);
            return text;
        }

        // EE thread, at every vblank: count fields, and once a second write the line.
        void frame()
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point secondStart = clock::now(), last = clock::now(), lastSnapshot{};
            static unsigned fields = 0u, slowSeconds = 0u, lines = 0u;
            static double worstMs = 0.0;
            static int snapshots = 0;
            const clock::time_point now = clock::now();
            worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
            ++fields;
            const double elapsed = std::chrono::duration<double>(now - secondStart).count();
            if (elapsed < 1.0)
                return;
            const int target = ps2xSpeedPercent();
            const double speed = fields / elapsed / kFieldsPerSecond * 100.0;
            const ps2x::perf::Vu1Times vu1 = ps2x::perf::takeVu1Times();
            const double interpreted = vu1.interpretedNs / (elapsed * 1e9) * 100.0;
            const double compiled = vu1.compiledNs / (elapsed * 1e9) * 100.0;
            const bool slow = target > 0 && speed < 0.9 * target;

            char line[256];
            std::snprintf(line, sizeof(line), "%s  %s  속도 %3.0f%% (목표 %s)  최악 %4.0fms  VU1 해석 %3.0f%% 컴파일 %3.0f%%",
                          clockText("%H:%M:%S").c_str(), dateText().c_str(), speed,
                          target > 0 ? (std::to_string(target) + "%").c_str() : "터보", worstMs, interpreted, compiled);
            std::string text = line;
            if (slow)
            {
                text += "  ▼느림";
                ++slowSeconds;
                // The second slow second of a stretch: one screenshot, at most one a minute and 10 a session.
                if (slowSeconds == 2u && snapshots < kMaxSnapshots &&
                    (lastSnapshot.time_since_epoch().count() == 0 ||
                     std::chrono::duration<double>(now - lastSnapshot).count() >= kSnapshotGapSeconds))
                {
                    char name[32];
                    std::snprintf(name, sizeof(name), "slow-%03d.bmp", ++snapshots);
                    std::error_code error;
                    std::filesystem::create_directories(g_folder, error);
                    const std::string path = narrow(g_folder / name);
                    if (!path.empty())
                    {
                        ps2x::perf::requestSnapshot(path);
                        text += std::string("  사진 ") + name;
                    }
                    lastSnapshot = now;
                }
            }
            else if (slowSeconds > 0u)
            {
                text += "  (느림 " + std::to_string(slowSeconds) + "초 끝)";
                slowSeconds = 0u;
            }
            // The first second includes the start-up: not written.
            if (lines++ > 0u)
            {
                g_log << text << "\n";
                for (const uint64_t hash : ps2x::perf::takeNewMissingVu1())
                {
                    char missing[192];
                    std::snprintf(missing, sizeof(missing), "          VU1 프로그램 %016llx 해석으로 실행: vu1\\vu1_%016llx.bin 저장",
                                  static_cast<unsigned long long>(hash), static_cast<unsigned long long>(hash));
                    g_log << missing << "\n";
                }
                g_log.flush();
            }
            secondStart = now;
            fields = 0u;
            worstMs = 0.0;
        }
    }

    void install(uint8_t *rdram, const std::filesystem::path &userDir)
    {
        const char *value = std::getenv("PS2X_PERF_LOG");
        if (!rdram || (value && std::strcmp(value, "0") == 0))
            return;
        g_rdram = rdram;
        const std::string stamp = clockText("%Y%m%d-%H%M%S");
        const std::filesystem::path logs = userDir / "logs";
        std::error_code error;
        std::filesystem::create_directories(logs, error);
        g_folder = logs / ("perf-" + stamp);
        g_log.open(logs / ("perf-" + stamp + ".txt"), std::ios::binary | std::ios::trunc);
        if (!g_log)
            return;
        g_log << "# YKTK3 성능 기록 " << clockText("%Y-%m-%d %H:%M:%S") << "\n"
              << "# 1초에 한 줄: 시각, 게임 날짜, 실제 속도(목표 대비), 가장 긴 프레임, VU1이 그 1초 중 해석기/컴파일 코드로 돈 비율.\n"
              << "# 목표의 90% 아래면 ▼느림, 느림이 2초 이어지면 화면 사진(perf-" << stamp << "\\slow-N.bmp, 1분에 한 장, 최대 10장).\n"
              << "# 해석으로 실행된 VU1 프로그램은 perf-" << stamp << "\\vu1에 저장됩니다 (컴파일해서 넣으면 3D가 빨라짐).\n";
        g_log.flush();
        ps2x::perf::setMissingVu1Directory(g_folder / "vu1");
        ps2x::pnach::addFrameHook(frame);
        std::fprintf(stderr, "[perf] logging to %s\n", narrow(logs / ("perf-" + stamp + ".txt")).c_str());
    }
}
