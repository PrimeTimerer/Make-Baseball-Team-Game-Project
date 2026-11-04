#include "urban/opening.h"

#include "urban/function_hooks.h"
#include "urban/log.h"

#include <cstring>

namespace urban::opening
{
    namespace
    {
        // OVL/OPMV.BIN is loaded at 0x01C7F000 (sub_00119550) and called through the scene's function pointer by
        // sub_003AB770 (start), sub_003AB7C8 (update) and sub_003AB6E8 (end), with a0 = the scene's +0x14: the command
        // word, then the movie object at +4. Its entry 0x01C7F008 calls 0x01C7F028, which calls the dispatcher
        // 0x01C7F048: 0 start (sets the movie mode, opens and plays the movie, keeps the object at a0+4; -1 on failure),
        // 1 end (frees the object if any, clears the movie mode, returns 1), 2 update (1 once START is pressed or the
        // movie has ended), 3 draw (returns 0), anything else -1. DNAS.BIN (online play only) loads at the same address.
        constexpr uint32_t kEntries[] = {0x01C7F008u, 0x01C7F028u, 0x01C7F048u};
        // The movie mode word: sub_00119530 sets it, sub_00119540 reads it.
        constexpr uint32_t kMovieMode = 0x01931228u;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        void write32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & kRamMask), &value, sizeof(value));
        }

        void skippedOpening(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t scene = getRegU32(ctx, 4) & kRamMask;
            uint32_t command = 0;
            std::memcpy(&command, rdram + scene, sizeof(command));
            int32_t result = -1;
            switch (command)
            {
            case 0: // start: the movie mode as the original sets it, without a movie object
                write32(rdram, kMovieMode, 1u);
                write32(rdram, scene + 4u, 0u);
                result = 0;
                URBAN_INFO(Boot, "opening movie (OVL/OPMV.BIN) skipped");
                break;
            case 1: // end: no object to free
                write32(rdram, kMovieMode, 0u);
                result = 1;
                break;
            case 2: // update: the movie has ended
                result = 1;
                break;
            case 3: // draw
                result = 0;
                break;
            default:
                break;
            }
            ctx->pc = getRegU32(ctx, 31);
            setReturnS32(ctx, result);
        }
    }

    void install()
    {
        for (const uint32_t entry : kEntries)
            FunctionHooks::instance().replace(entry, "opening_movie", &skippedOpening, ReplacementStatus::Native,
                                              "OVL/OPMV.BIN is not recompiled: the opening movie is skipped");
    }
}
