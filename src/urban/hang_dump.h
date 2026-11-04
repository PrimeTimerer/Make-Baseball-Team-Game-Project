#pragma once

class PS2Runtime;

namespace urban::hangdump
{
    // URBAN_HANG_DUMP=<seconds>: after that delay, print the native stack of every thread in the process to stderr.
    // URBAN_HANG_DUMP_REPEAT=<seconds>: keep dumping at that interval afterwards.
    // URBAN_HANG_DUMP_NATIVE=0: skip the native stacks (guest kernel state and call profile only).
    void startFromEnvironment();

    // Once set, dumps also include the guest EE kernel state (threads, PCs, wait reasons, semaphores).
    void setRuntime(PS2Runtime *runtime);

    // On a crash (an exception nothing handles), print the faulting address, the native stack of the crashing thread
    // and the guest EE state to stderr before the process ends. Windows only.
    void installCrashReport();
}
