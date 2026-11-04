#include "Common.h"
#include "TTY.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    void scePrintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceResetttyinit", rdram, ctx, runtime);
    }

    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyHandler", rdram, ctx, runtime);
    }

    // The DECI2 "ttyp" channel of a development kit, which newlib's write()/read() use for stdout/stderr/stdin
    // (YKTK3's printf). A retail console has no host on it: init succeeds, writes go to the log, reads get nothing.
    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // sceTtyWrite(buf, len)
        const uint32_t buffer = getRegU32(ctx, 4);
        const int32_t length = static_cast<int32_t>(getRegU32(ctx, 5));
        if (length > 0)
        {
            std::string text = readPs2CStringBounded(rdram, runtime, buffer, std::min<size_t>(static_cast<size_t>(length), 1024u));
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs && !text.empty())
                std::cerr << "[EE tty] " << sanitizeForLog(text) << std::endl;
            else if (count == kMaxPrintfLogs + 1)
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
        }
        setReturnS32(ctx, length);
    }
}
