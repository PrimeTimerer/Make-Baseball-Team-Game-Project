#include "Common.h"
#include "Deci2.h"

namespace ps2_stubs
{
    namespace
    {
        // DECI2 is the debug protocol to a development kit's host. A retail console has no DECI2 manager: every call
        // fails, as its kernel's Deci2Call does.
        void noDeci2Host(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)rdram;
            (void)runtime;
            setReturnS32(ctx, -1);
        }
    }

    void sceDeci2Close(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ExLock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ExRecv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ExReqSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ExSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ExUnLock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2Open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2Poll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }

    void sceDeci2ReqSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        noDeci2Host(rdram, ctx, runtime);
    }
}
