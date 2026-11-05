#include "urban/native_profile.h"

#include "runtime/ps2_display_options.h"
#include "urban/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace urban::nativeprofile
{
#if defined(_WIN32)
    namespace
    {
        // Log times of the last game frames (ring; noteGameFrame writes, the capture thread reads after stopping).
        constexpr size_t kFrameRing = 1u << 16;
        std::array<double, kFrameRing> g_frameTimes{};
        std::atomic<uint64_t> g_frameCount{0u};

        struct ThreadSamples
        {
            std::string name;
            uint64_t samples = 0;
            std::unordered_map<DWORD64, uint64_t> self;      // leaf pc -> count
            std::vector<std::vector<DWORD64>> stacks;        // walked stacks (GameThread only)
            std::vector<uint32_t> stackSecond;               // capture second of each walked stack (Ctrl+F6 captures)
            std::vector<double> stackTime;                   // log time of each walked stack (timeline output)
        };

        struct Session
        {
            std::map<DWORD, ThreadSamples> threads;
            std::map<DWORD, HANDLE> handles;
        };

        std::string threadName(HANDLE thread)
        {
            PWSTR description = nullptr;
            std::string name;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
            {
                for (const wchar_t *c = description; *c; ++c)
                    name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                LocalFree(description);
            }
            return name;
        }

        HANDLE symbolProcess()
        {
            static HANDLE process = []
            {
                HANDLE p = GetCurrentProcess();
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(p, nullptr, TRUE);
                return p;
            }();
            return process;
        }

        // Function name for a pc (symbol name without offset), cached by pc.
        std::string functionOf(HANDLE process, DWORD64 pc)
        {
            static std::unordered_map<DWORD64, std::string> cache;
            if (const auto it = cache.find(pc); it != cache.end())
                return it->second;
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            std::string name;
            if (SymFromAddr(process, pc, &displacement, symbol))
                name = symbol->Name;
            else
            {
                char text[32];
                std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(pc));
                name = text;
            }
            cache.emplace(pc, name);
            return name;
        }

        // Main per-frame consumers of the game thread, summed from the walked stacks (inclusive time).
        constexpr std::pair<const char *, const char *> kSubsystems[] = {
            {"VU1 (microprogramas de vertices)", "VU1Interpreter::run"},
            {"VIF1 (envio de dados ao VU1)", "PS2Memory::processVIF1Data"},
            {"VU0 (microprogramas)", "PS2Runtime::executeVU0Microprogram"},
            {"GS (front end: pacotes GIF)", "GS::processGIFPacket"},
            {"GS (envio a thread de video)", "GSThreadedBackend::Enqueue"},
            {"IOP (total pedido pelo EE)", "ps2x::iop::IopSubsystem::runEeCycles"},
            {"IOP (espera da thread do IOP)", "ps2x::iop::IopSubsystem::Impl::join"},
            {"EE esperando evento (thread ociosa)", "EeScheduler::waitForEvent"},
            {"Folga: esperando o tempo real (sobra de CPU)", "EeScheduler::processDueDeadlines"},
        };

        void report(HANDLE process, std::map<DWORD, ThreadSamples> &threads, std::FILE *out, const std::string &foldedPath,
                    const char *prefix)
        {
            for (auto &[id, t] : threads)
            {
                if (t.samples == 0)
                    continue;
                std::map<std::string, uint64_t> self;
                for (const auto &[pc, n] : t.self)
                    self[functionOf(process, pc)] += n;
                std::vector<std::pair<uint64_t, std::string>> top;
                for (auto &[name, n] : self)
                    top.emplace_back(n, name);
                std::sort(top.rbegin(), top.rend());
                std::fprintf(out, "%sthread %lu %s: %llu samples; self time:\n", prefix, id, t.name.c_str(),
                             static_cast<unsigned long long>(t.samples));
                const size_t selfLines = t.name == "GameThread" ? 45u : 12u;
                for (size_t i = 0; i < top.size() && i < selfLines; ++i)
                    std::fprintf(out, "%s  %5.1f%%  %s\n", prefix, 100.0 * top[i].first / t.samples, top[i].second.c_str());
                // URBAN_NATIVE_PROFILE_LINES=<n>: the n (1 = 60) hottest source lines (self time) of this thread, e.g.
                // which part of a generated VU1 block the time goes to.
                const char *lines = std::getenv("URBAN_NATIVE_PROFILE_LINES");
                const size_t lineCount = lines ? (std::atoi(lines) > 1 ? static_cast<size_t>(std::atoi(lines)) : 60u) : 0u;
                if (lineCount != 0u)
                {
                    std::map<std::string, uint64_t> byLine;
                    for (const auto &[pc, n] : t.self)
                    {
                        IMAGEHLP_LINE64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD displacement = 0;
                        std::string key;
                        if (SymGetLineFromAddr64(process, pc, &displacement, &line))
                        {
                            std::string file = line.FileName ? line.FileName : "?";
                            const size_t slash = file.find_last_of("\\/");
                            key = (slash == std::string::npos ? file : file.substr(slash + 1)) + ":" + std::to_string(line.LineNumber);
                        }
                        else
                            key = functionOf(process, pc);
                        byLine[key] += n;
                    }
                    std::vector<std::pair<uint64_t, std::string>> topLines;
                    for (auto &[key, n] : byLine)
                        topLines.emplace_back(n, key);
                    std::sort(topLines.rbegin(), topLines.rend());
                    for (size_t i = 0; i < topLines.size() && i < lineCount; ++i)
                        std::fprintf(out, "%sline %5.1f%%  %s\n", prefix, 100.0 * topLines[i].first / t.samples, topLines[i].second.c_str());
                }
                if (t.stacks.empty())
                    continue;
                // Folded stacks ("root;...;leaf count" per line) for offline caller/callee analysis (flame graphs).
                if (!foldedPath.empty())
                {
                    std::map<std::string, uint64_t> folded;
                    for (const auto &stack : t.stacks)
                    {
                        std::string line;
                        for (auto pc = stack.rbegin(); pc != stack.rend(); ++pc)
                        {
                            if (!line.empty())
                                line += ';';
                            line += functionOf(process, *pc);
                        }
                        ++folded[line];
                    }
                    if (std::FILE *file = std::fopen(foldedPath.c_str(), "w"))
                    {
                        for (const auto &[line, n] : folded)
                            std::fprintf(file, "%s %llu\n", line.c_str(), static_cast<unsigned long long>(n));
                        std::fclose(file);
                    }
                }
                std::map<std::string, uint64_t> inclusive;
                for (const auto &stack : t.stacks)
                {
                    std::set<std::string> seen;
                    for (const DWORD64 pc : stack)
                        seen.insert(functionOf(process, pc));
                    for (const std::string &name : seen)
                        ++inclusive[name];
                }
                const double total = static_cast<double>(t.stacks.size());
                std::fprintf(out, "%sthread %lu %s by subsystem (inclusive):\n", prefix, id, t.name.c_str());
                for (const auto &[label, function] : kSubsystems)
                {
                    const auto it = inclusive.find(function);
                    std::fprintf(out, "%s  %5.1f%%  %s\n", prefix, it == inclusive.end() ? 0.0 : 100.0 * it->second / total, label);
                }
                std::vector<std::pair<uint64_t, std::string>> topInc;
                for (auto &[name, n] : inclusive)
                    topInc.emplace_back(n, name);
                std::sort(topInc.rbegin(), topInc.rend());
                std::fprintf(out, "%sthread %lu %s inclusive time (stack depth <= 24):\n", prefix, id, t.name.c_str());
                for (size_t i = 0; i < topInc.size() && i < 80u; ++i)
                    std::fprintf(out, "%s  %5.1f%%  %s\n", prefix, 100.0 * topInc[i].first / total, topInc[i].second.c_str());
            }
            std::fflush(out);
        }

        // Named threads only (the runtime names its workers); the main/present thread is unnamed but cheap.
        void openThreads(Session &session)
        {
            const DWORD self = GetCurrentThreadId();
            const DWORD pid = GetCurrentProcessId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                           entry.th32ThreadID);
                if (!thread)
                    continue;
                const std::string name = threadName(thread);
                if (name.empty() || name == "ProfileCapture")
                {
                    CloseHandle(thread);
                    continue;
                }
                session.threads[entry.th32ThreadID].name = name;
                session.handles[entry.th32ThreadID] = thread;
            }
            CloseHandle(snapshot);
        }

        void closeThreads(Session &session)
        {
            for (auto &[id, thread] : session.handles)
                CloseHandle(thread);
            session.handles.clear();
        }

        // One round: every opened thread is suspended briefly and its stack walked.
        void sampleOnce(Session &session, uint32_t second = 0u)
        {
            for (auto &[id, thread] : session.handles)
            {
                ThreadSamples &t = session.threads[id];
                // URBAN_NATIVE_PROFILE_WALK=<thread name>: walk that thread's stacks instead of GameThread's (one
                // thread at a time; the reports label it as usual).
                static const std::string walkName = [] { const char *v = std::getenv("URBAN_NATIVE_PROFILE_WALK"); return std::string(v && *v ? v : "GameThread"); }();
                const bool walk = t.name == walkName;
                // Nothing may allocate or take a lock between Suspend and Resume: the suspended thread can hold the
                // heap lock (the old StackWalk64 + map insertions here deadlocked the game). The stack goes into a
                // fixed array, unwound with RtlVirtualUnwind, and is stored after the thread resumes.
                std::array<DWORD64, 24> frames{};
                size_t depth = 0u;
                bool sampled = false;
                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(thread, &context))
                {
                    sampled = true;
                    frames[depth++] = context.Rip;
                    while (walk && depth < frames.size())
                    {
                        DWORD64 imageBase = 0;
                        if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr))
                        {
                            PVOID handlerData = nullptr;
                            DWORD64 establisherFrame = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                             &establisherFrame, nullptr);
                        }
                        else
                        {
                            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp); // leaf function
                            context.Rsp += 8u;
                        }
                        if (context.Rip == 0u)
                            break;
                        frames[depth++] = context.Rip;
                    }
                }
                ResumeThread(thread);
                if (!sampled)
                    continue;
                ++t.samples;
                ++t.self[frames[0]];
                if (walk)
                {
                    t.stacks.emplace_back(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(depth));
                    t.stackSecond.push_back(second);
                    t.stackTime.push_back(urban::log::elapsedSeconds());
                }
            }
        }

        // Per-second table of a capture: the FPS line of that second, then the share of the game thread's samples
        // spent inside each subsystem, so the cost of each extra fighter shows up as a step.
        void secondTable(HANDLE process, const ThreadSamples &game, const std::map<uint32_t, std::string> &fpsBySecond,
                         const std::map<uint32_t, std::vector<std::string>> &marks, std::FILE *out)
        {
            constexpr size_t kCount = sizeof(kSubsystems) / sizeof(kSubsystems[0]);
            std::map<uint32_t, std::array<uint64_t, kCount + 1u>> perSecond; // [kCount] = samples in that second
            for (size_t i = 0; i < game.stacks.size(); ++i)
            {
                auto &row = perSecond[game.stackSecond[i]];
                ++row[kCount];
                std::set<std::string> seen;
                for (const DWORD64 pc : game.stacks[i])
                    seen.insert(functionOf(process, pc));
                for (size_t k = 0; k < kCount; ++k)
                    if (seen.count(kSubsystems[k].second))
                        ++row[k];
            }
            std::fprintf(out, "Por segundo: %% da thread principal (GameThread) em cada parte, e o FPS daquele segundo\n");
            std::fprintf(out, "  seg  |  VU1 VIF1  VU0   GS GSenv  IOP IOPesp ocioso folga | jogo / tela / velocidade / pior quadro\n");
            uint32_t last = 0u;
            for (const auto &[second, row] : perSecond)
                last = std::max(last, second);
            for (const auto &[second, line] : fpsBySecond)
                last = std::max(last, second);
            for (uint32_t second = 0; second <= last; ++second)
            {
                if (const auto m = marks.find(second); m != marks.end())
                    for (const std::string &mark : m->second)
                        std::fprintf(out, "  ---- %s ----\n", mark.c_str());
                std::fprintf(out, "  %4u |", second);
                const auto it = perSecond.find(second);
                for (size_t k = 0; k < kCount; ++k)
                {
                    const bool empty = it == perSecond.end() || it->second[kCount] == 0u;
                    std::fprintf(out, " %4.0f", empty ? 0.0 : 100.0 * static_cast<double>(it->second[k]) / static_cast<double>(it->second[kCount]));
                }
                const auto fps = fpsBySecond.find(second);
                std::fprintf(out, " | %s\n", fps == fpsBySecond.end() ? "" : fps->second.c_str());
            }
            std::fprintf(out, "\n");
        }

        void run(double startSeconds, double durationSeconds, double intervalMs)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
            HANDLE process = symbolProcess();
            Session session;
            openThreads(session);
            std::fprintf(stderr, "[native-profile] sampling %zu threads for %.0f s every %.2f ms\n", session.handles.size(), durationSeconds,
                         intervalMs);
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(durationSeconds);
            timeBeginPeriod(1);
            while (std::chrono::steady_clock::now() < end)
            {
                sampleOnce(session);
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(intervalMs));
            }
            timeEndPeriod(1);
            const char *stacks = std::getenv("URBAN_NATIVE_PROFILE_STACKS");
            report(process, session.threads, stderr, stacks ? stacks : "", "[native-profile] ");
            // URBAN_NATIVE_PROFILE_TIMELINE=<file>: every GameThread sample as "time stack" (log time, folded stack
            // root first), to line the samples up with per-frame events of the log (e.g. URBAN_CALL_TRACE=128dc0).
            if (const char *timeline = std::getenv("URBAN_NATIVE_PROFILE_TIMELINE"); timeline && *timeline)
                if (std::FILE *file = std::fopen(timeline, "w"))
                {
                    for (const auto &[id, t] : session.threads)
                        for (size_t i = 0; i < t.stacks.size() && i < t.stackTime.size(); ++i)
                        {
                            std::string line;
                            for (auto pc = t.stacks[i].rbegin(); pc != t.stacks[i].rend(); ++pc)
                            {
                                if (!line.empty())
                                    line += ';';
                                line += functionOf(process, *pc);
                            }
                            std::fprintf(file, "%.4f %s\n", t.stackTime[i], line.c_str());
                        }
                    std::fclose(file);
                }
            closeThreads(session);
        }

        // Ctrl+F6 capture: one press starts sampling (1 ms), the next stops it and writes perfil_<n>.txt (summary, the FPS
        // line of every second, per-thread hot spots, subsystem split) and perfil_<n>_pilhas.txt (folded stacks) next
        // to urban.exe. Made for comparing scenes, e.g. the character select with 2, 3 and 4 fighters on screen.
        void captureLoop(std::filesystem::path directory)
        {
            SetThreadDescription(GetCurrentThread(), L"ProfileCapture");
            bool wasDown = false;
            uint32_t captureIndex = 0u;
            // Continue the numbering after captures from earlier sessions.
            while (std::filesystem::exists(directory / ("perfil_" + std::to_string(captureIndex + 1u) + ".txt")))
                ++captureIndex;
            for (;;)
            {
                const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                DWORD foregroundPid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
                const bool pressed = down && !wasDown && foregroundPid == GetCurrentProcessId();
                wasDown = down;
                if (!pressed)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }

                ++captureIndex;
                const std::string name = "perfil_" + std::to_string(captureIndex);
                const std::string note = "CAPTURING PROFILE " + std::to_string(captureIndex) + " (Ctrl+F6 to stop)";
                ps2_host_display::setOverlayNote(note.c_str());
                HANDLE process = symbolProcess();
                Session session;
                openThreads(session);
                std::map<uint32_t, std::string> fpsBySecond;
                std::map<uint32_t, std::vector<std::string>> marks;
                uint32_t markCount = 0u;
                bool markWasDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                unsigned lastSerial = 0u;
                (void)ps2_host_display::fpsLine(lastSerial);
                const auto start = std::chrono::steady_clock::now();
                timeBeginPeriod(1);
                for (;;)
                {
                    const uint32_t second =
                        static_cast<uint32_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                    sampleOnce(session, second);
                    unsigned serial = 0u;
                    const char *line = ps2_host_display::fpsLine(serial);
                    if (serial != lastSerial)
                    {
                        lastSerial = serial;
                        fpsBySecond[second] = line;
                    }
                    // Ctrl+F7 (F7 alone is the 200% speed): a marker in the table (e.g. right after adding a fighter).
                    const bool markDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (markDown && !markWasDown)
                    {
                        ++markCount;
                        marks[second].push_back("marca " + std::to_string(markCount) + " (Ctrl+F7)");
                        const std::string markNote = note + "  - mark " + std::to_string(markCount);
                        ps2_host_display::setOverlayNote(markNote.c_str());
                    }
                    markWasDown = markDown;
                    const bool stopDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (stopDown && !wasDown)
                    {
                        wasDown = true;
                        break;
                    }
                    wasDown = stopDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                timeEndPeriod(1);
                const double duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                // Frame times and the game thread's samples, both on the log clock.
                {
                    const double endLog = urban::log::elapsedSeconds();
                    const double startLog = endLog - duration;
                    if (std::FILE *frames = _wfopen((directory / (name + "_quadros.txt")).wstring().c_str(), L"w"))
                    {
                        const uint64_t count = g_frameCount.load(std::memory_order_acquire);
                        const uint64_t first = count > kFrameRing ? count - kFrameRing : 0u;
                        for (uint64_t i = first; i < count; ++i)
                        {
                            const double t = g_frameTimes[i % kFrameRing];
                            if (t >= startLog - 0.1 && t <= endLog)
                                std::fprintf(frames, "%.4f\n", t);
                        }
                        for (const auto &[second, list] : marks)
                            for (const auto &mark : list)
                                std::fprintf(frames, "# %s at %.1f s into the capture (log %.1f)\n", mark.c_str(), static_cast<double>(second),
                                             startLog + second);
                        std::fclose(frames);
                    }
                    // Real presents of the capture, on the log clock (perfil_<n>_telas.txt: log time, emulated field,
                    // image sequence, game frames so far).
                    {
                        const double steadyNow =
                            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        const double steadyToLog = endLog - steadyNow;
                        std::vector<ps2_host_display::PresentRecord> presents;
                        ps2_host_display::copyPresents(steadyNow - duration, presents);
                        if (std::FILE *file = _wfopen((directory / (name + "_telas.txt")).wstring().c_str(), L"w"))
                        {
                            for (const auto &p : presents)
                                std::fprintf(file, "%.5f %llu %llu %llu\n", p.hostSeconds + steadyToLog, static_cast<unsigned long long>(p.field),
                                             static_cast<unsigned long long>(p.imageSequence), static_cast<unsigned long long>(p.gameFrames));
                            std::fclose(file);
                        }
                    }
                    if (std::FILE *timeline = _wfopen((directory / (name + "_linha.txt")).wstring().c_str(), L"w"))
                    {
                        for (const auto &[id, t] : session.threads)
                            for (size_t i = 0; i < t.stacks.size() && i < t.stackTime.size(); ++i)
                            {
                                std::string line;
                                for (auto pc = t.stacks[i].rbegin(); pc != t.stacks[i].rend(); ++pc)
                                {
                                    if (!line.empty())
                                        line += ';';
                                    line += functionOf(process, *pc);
                                }
                                std::fprintf(timeline, "%.4f %s\n", t.stackTime[i], line.c_str());
                            }
                        std::fclose(timeline);
                    }
                }
                ps2_host_display::setOverlayNote(("SAVING " + name + ".txt ...").c_str());
                const std::filesystem::path textPath = directory / (name + ".txt");
                const std::filesystem::path stacksPath = directory / (name + "_pilhas.txt");
                if (std::FILE *out = _wfopen(textPath.wstring().c_str(), L"w"))
                {
                    std::fprintf(out, "Captura %u: %.1f segundos, %u marca(s) com Ctrl+F7\n\n", captureIndex, duration, markCount);
                    for (const auto &[id, t] : session.threads)
                        if (t.name == "GameThread")
                            secondTable(process, t, fpsBySecond, marks, out);
                    std::fprintf(out, "Captura inteira:\n");
                    report(process, session.threads, out, stacksPath.string(), "");
                    std::fclose(out);
                }
                closeThreads(session);
                ps2_host_display::setOverlayNote(("PROFILE SAVED: " + name + ".txt").c_str());
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    ps2_host_display::setOverlayNote("");
                }).detach();
            }
        }
    }

    void startFromEnvironment()
    {
        const char *spec = std::getenv("URBAN_NATIVE_PROFILE");
        if (!spec || !*spec)
            return;
        double start = 60.0, duration = 20.0, interval = 1.0;
        std::sscanf(spec, "%lf:%lf:%lf", &start, &duration, &interval);
        std::thread([start, duration, interval]() { run(start, duration, interval); }).detach();
    }

    void noteGameFrame()
    {
        const uint64_t index = g_frameCount.load(std::memory_order_relaxed);
        g_frameTimes[index % kFrameRing] = urban::log::elapsedSeconds();
        g_frameCount.store(index + 1u, std::memory_order_release);
    }

    void startCaptureHotkey(const std::filesystem::path &directory)
    {
        std::thread([directory]() { captureLoop(directory); }).detach();
    }
#else
    void startFromEnvironment() {}
    void startCaptureHotkey(const std::filesystem::path &) {}
    void noteGameFrame() {}
#endif
}
