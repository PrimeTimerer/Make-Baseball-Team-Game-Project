#include "urban/hle_bindings.h"
#include "urban/function_hooks.h"
#include "urban/log.h"

#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

namespace urban
{
    namespace
    {
        constexpr SdkFunction kSdkFunctions[] = {
#define X(address, name, method, score) {address, #name, method, score},
#include "urban/sdk_functions.inc"
#undef X
        };

        // Same set the SotC port binds by name (Port/recomp/sotc.toml there), plus the extra libcdvd entry points below. Everything else in the SDK - GS/DMA/VIF
        // packet libraries, libpad, libmc, newlib - stays original recompiled code.
        constexpr std::string_view kHleNames[] = {
            "sceSifStopDma", "sceSifDmaStat", "sceSifSetDma", "isceSifSetDma", "sceSifSetDChain", "isceSifSetDChain",
            "sceSifSetReg", "sceSifGetReg", "sceSifGetSreg", "sceSifSetSreg", "sceSifGetDataTable",
            "sceSifInitCmd", "sceSifExitCmd", "sceSifSetCmdBuffer", "sceSifSetSysCmdBuffer",
            "sceSifAddCmdHandler", "sceSifRemoveCmdHandler", "sceSifSendCmd", "sceSifWriteBackDCache",
            "sceSifInitRpc", "sceSifExitRpc", "sceSifGetOtherData", "sceSifBindRpc", "sceSifCallRpc", "sceSifCheckStatRpc",
            "sceSifSetRpcQueue", "sceSifRegisterRpc", "sceSifRemoveRpc", "sceSifRemoveRpcQueue",
            "sceSifGetNextRequest", "sceSifExecRequest", "sceSifRpcLoop",
            "sceSifInitIopHeap", "sceSifAllocIopHeap", "sceSifAllocSysMemory", "sceSifFreeSysMemory", "sceSifFreeIopHeap",
            "sceSifLoadIopHeap", "sceSifLoadFileReset", "sceSifLoadModuleBuffer", "sceSifLoadModule", "sceSifLoadElfPart",
            "sceSifLoadElf", "sceSifGetIopAddr", "sceSifSetIopAddr", "sceSifResetIop", "sceSifIsAliveIop", "sceSifSyncIop",
            "sceSifRebootIop",
            "sceFsInit", "sceFsReset", "sceOpen", "sceClose", "sceLseek", "sceRead", "sceWrite", "sceIoctl",
            "sceDeci2Open", "sceDeci2Close", "sceDeci2ReqSend", "sceDeci2Poll", "sceDeci2ExRecv", "sceDeci2ExSend",
            "sceDeci2ExReqSend", "sceDeci2ExLock", "sceDeci2ExUnLock",
            "sceTtyWrite", "sceTtyRead", "sceTtyInit", "sceResetttyinit", "scePrintf",
            "sceCdCallback", "sceCdInitEeCB", "sceCdSearchFile", "sceCdNcmdDiskReady", "sceCdSync", "sceCdSyncS", "sceCdInit",
            "sceCdDiskReady", "sceCdMmode", "sceCdRead", "sceCdReadIOPm", "sceCdGetError", "sceCdStatus", "sceCdReadClock",
            // Not in the SotC list (that game never calls them), but they talk to the same missing cdvdfsv server.
            "sceCdGetDiskType", "sceCdSeek", "sceCdBreak", "sceCdStandby", "sceCdStop", "sceCdPause", "sceCdTrayReq",
            "sceMpegCreate", "sceMpegInit", "sceMpegDelete", "sceMpegAddBs", "sceMpegGetPicture", "sceMpegGetPictureRAW8",
            "sceMpegReset", "sceMpegIsEnd", "sceMpegIsRefBuffEmpty", "sceMpegSetDecodeMode", "sceMpegGetDecodeMode",
            "sceMpegAddCallback", "sceMpegClearRefBuff", "sceMpegGetPictureRAW8xy", "sceMpegSetDefaultPtsGap",
            "sceMpegResetDefaultPtsGap", "sceMpegSetImageBuff", "sceMpegDispWidth", "sceMpegDispHeight",
            "sceMpegDispCenterOffX", "sceMpegDispCenterOffY", "sceMpegDemuxPssRing", "sceMpegDemuxPss", "sceMpegAddStrCallback",
            "sceIpuStopDMA", "sceIpuRestartDMA", "sceIpuSync", "sceIpuInit",
            "_sceSDC", "_sceIDC",
        };

        void traceCall(const R5900Context *ctx)
        {
            const SdkFunction *function = sdkFunctionAt(ctx->pc);
            URBAN_TRACE(Hook, "HLE " << (function ? function->name : std::string_view("?")) << std::hex << " a0=0x" << getRegU32(ctx, 4)
                                     << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7)
                                     << " ra=0x" << getRegU32(ctx, 31));
        }

        // SDK functions the runtime has no handler of their own for, bound to a compatible handler.
        struct HleAlias
        {
            std::string_view name;
            std::string_view handler;
            std::string_view why;
        };
        constexpr HleAlias kHleAliases[] = {
            // sceCdLayerSearchFile(fp, name, layer): same first two arguments; this disc is single-layer.
            {"sceCdLayerSearchFile", "sceCdSearchFile", "layer argument ignored (single-layer disc)"},
            // YKTK3 stops (then unloads) its network modules when no network adapter answers.
            {"sceSifStopModule", "SifStopModule", "same arguments (modid, args, argp, result)"},
        };

        // sceSifUnloadModule(modid): the IOP emulator does not unload modules (their memory stays allocated); success,
        // with the module ID as libkernl returns it.
        void unloadModule(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t moduleId = static_cast<int32_t>(getRegU32(ctx, 4));
            URBAN_INFO(Hook, "sceSifUnloadModule(" << moduleId << ") -> kept loaded");
            ctx->pc = getRegU32(ctx, 31);
            setReturnS32(ctx, moduleId > 0 ? moduleId : -1);
        }

        // libkernl's loadfile RPC client init in SLPM_658.53 (unnamed; ps2sdk's SifLoadFileInit), called by every
        // loadfile function: it binds sid 0x80000006 and retries until the IOP's loadfile server answers. The runtime
        // loads modules itself (the sceSifLoadModule family is HLE'd), so there is no such server and the retry loop
        // never ends. Failing makes the loadfile calls without a handler (sceSifSearchModuleByName/ByAddress...) return
        // their error at once.
        constexpr uint32_t kLoadFileInit = 0x00555FA8u;

        void noLoadFileServer(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            URBAN_INFO(Hook, "loadfile RPC init -> -1 (no IOP loadfile server) ra=0x" << std::hex << getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            setReturnS32(ctx, -1);
        }

        // fileio calls with no runtime handler. sceFsInit is HLE'd, so the game's fileio RPC client is never bound:
        // without a handler these send to no server and wait forever for the IOP's completion command. YKTK3 makes them
        // for its network settings on the HDD (sceMount "pfs1:"), which this PS2 does not have: they fail with -ENODEV,
        // as on a console without an HDD.
        constexpr std::string_view kNoDeviceFileIo[] = {
            "sceMount", "sceDevctl", "sceFormat", "sceSync", "sceMkdir", "sceDopen", "sceGetstat", "sceChstat", "sceRename",
            "sceSymlink", "sceReadlink", "sceDread", "sceDclose", "sceIoctl2", "sceLseek64",
        };

        void noDeviceFileIo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const SdkFunction *function = sdkFunctionAt(ctx->pc);
            const std::string_view name = function ? function->name : std::string_view("?");
            std::string path;
            // A path first, except in the descriptor calls.
            if (name != "sceDread" && name != "sceDclose" && name != "sceIoctl2" && name != "sceLseek64")
                if (const char *text = reinterpret_cast<const char *>(getMemPtr(rdram, getRegU32(ctx, 4))))
                    path.assign(text, strnlen(text, 64));
            URBAN_INFO(Hook, name << "(\"" << path << "\") -> -ENODEV (no HDD) ra=0x" << std::hex << getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            setReturnS32(ctx, -19);
        }

        // Equivalent of the recompiler's generated stub body: return to $ra unless the handler redirects.
        // The dispatcher has set ctx->pc to the called address, which names the SDK function for tracing.
        template <PS2Runtime::RecompiledFunction Handler>
        void hleTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (log::traceEnabled(log::Category::Hook))
                traceCall(ctx);
            ctx->pc = getRegU32(ctx, 31);
            Handler(rdram, ctx, runtime);
            if (log::traceEnabled(log::Category::Hook))
                URBAN_TRACE(Hook, "    -> v0=0x" << std::hex << getRegU32(ctx, 2));
        }

        const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> &syscallHandlers()
        {
            static const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> handlers = {
#define URBAN_SYSCALL_ENTRY(name) {#name, &hleTrampoline<&ps2_syscalls::name>},
                PS2_SYSCALL_LIST(URBAN_SYSCALL_ENTRY)
#undef URBAN_SYSCALL_ENTRY
            };
            return handlers;
        }

        const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> &stubHandlers()
        {
            static const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> handlers = {
#define URBAN_STUB_ENTRY(name) {#name, &hleTrampoline<&ps2_stubs::name>},
                PS2_STUB_LIST(URBAN_STUB_ENTRY)
#undef URBAN_STUB_ENTRY
            };
            return handlers;
        }

        // Same resolution order as the recompiler: syscall list first, then stubs, with the leading-underscore alias.
        PS2Runtime::RecompiledFunction resolveHandler(std::string_view name, std::string &resolvedName)
        {
            if (const std::string_view syscall = ps2_runtime_calls::resolveSyscallName(name); !syscall.empty())
            {
                resolvedName = "ps2_syscalls::" + std::string(syscall);
                return syscallHandlers().at(syscall);
            }
            if (const std::string_view stub = ps2_runtime_calls::resolveStubName(name); !stub.empty())
            {
                resolvedName = "ps2_stubs::" + std::string(stub);
                return stubHandlers().at(stub);
            }
            return nullptr;
        }

        bool mpegHleWanted()
        {
            const char *ipu = std::getenv("PS2X_IPU");
            const char *hle = std::getenv("URBAN_MPEG_HLE");
            return (hle && *hle == '1') || (ipu && *ipu == '0');
        }

        std::set<std::string, std::less<>> skippedFromEnvironment()
        {
            std::set<std::string, std::less<>> skipped;
            const char *value = std::getenv("URBAN_HLE_SKIP");
            std::string_view list = value ? value : "";
            while (!list.empty())
            {
                const size_t comma = list.find(',');
                if (const std::string_view token = list.substr(0, comma); !token.empty())
                    skipped.emplace(token);
                list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
            return skipped;
        }
    }

    const SdkFunction *findSdkFunction(std::string_view name)
    {
        for (const SdkFunction &function : kSdkFunctions)
        {
            if (function.name == name)
                return &function;
        }
        return nullptr;
    }

    const SdkFunction *sdkFunctionAt(uint32_t address)
    {
        for (const SdkFunction &function : kSdkFunctions)
        {
            if (function.address == address)
                return &function;
        }
        return nullptr;
    }

    void installHleBindings(PS2Runtime &runtime)
    {
        (void)runtime;
        const auto skipped = skippedFromEnvironment();
        size_t bound = 0;
        std::ostringstream missing;
        for (const std::string_view name : kHleNames)
        {
            const SdkFunction *function = findSdkFunction(name);
            if (!function)
            {
                missing << ' ' << name;
                continue;
            }
            if (skipped.count(name))
            {
                URBAN_INFO(Hook, "HLE binding skipped by URBAN_HLE_SKIP: " << name);
                continue;
            }
            // The game's own libmpeg/libipu drive the emulated IPU (the runtime's IPU, on unless PS2X_IPU=0): CRI
            // Sofdec's M2V decoder calls into libmpeg below the functions the HLE replaces, so with the HLE the FMVs
            // stayed black (docs/PROGRESS.md §89). URBAN_MPEG_HLE=1 binds the HLE again.
            if ((name.rfind("sceMpeg", 0) == 0 || name.rfind("sceIpu", 0) == 0) && !mpegHleWanted())
                continue;
            std::string resolvedName;
            const PS2Runtime::RecompiledFunction handler = resolveHandler(name, resolvedName);
            if (!handler)
            {
                URBAN_ERROR(Hook, "no runtime handler for " << name);
                continue;
            }
            std::ostringstream reason;
            reason << resolvedName << " (located: " << function->method;
            if (function->method == "near")
                reason << ' ' << std::fixed << std::setprecision(2) << function->score;
            reason << ')';
            if (FunctionHooks::instance().replace(function->address, std::string(name), handler, ReplacementStatus::Hle, reason.str()))
                ++bound;
        }
        for (const HleAlias &alias : kHleAliases)
        {
            const SdkFunction *function = findSdkFunction(alias.name);
            if (!function || skipped.count(alias.name))
                continue;
            std::string resolvedName;
            const PS2Runtime::RecompiledFunction handler = resolveHandler(alias.handler, resolvedName);
            if (handler && FunctionHooks::instance().replace(function->address, std::string(alias.name), handler, ReplacementStatus::Hle,
                                                             resolvedName + ": " + std::string(alias.why)))
                ++bound;
        }
        for (const std::string_view name : kNoDeviceFileIo)
        {
            const SdkFunction *function = findSdkFunction(name);
            if (!function || skipped.count(name))
                continue;
            if (FunctionHooks::instance().replace(function->address, std::string(name), &noDeviceFileIo, ReplacementStatus::Hle,
                                                  "-ENODEV: no HDD, and the fileio RPC client is not bound"))
                ++bound;
        }
        if (const SdkFunction *function = findSdkFunction("sceSifUnloadModule"); function && !skipped.count("sceSifUnloadModule") &&
            FunctionHooks::instance().replace(function->address, "sceSifUnloadModule", &unloadModule, ReplacementStatus::Hle,
                                              "module kept loaded, success"))
            ++bound;
        if (FunctionHooks::instance().replace(kLoadFileInit, "loadfile_init", &noLoadFileServer, ReplacementStatus::Hle,
                                              "no IOP loadfile server: fails instead of retrying forever"))
            ++bound;
        URBAN_INFO(Hook, "HLE: bound " << bound << " SDK functions; not located in this ELF:" << missing.str());
    }
}
