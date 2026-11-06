#include "urban/save_identity.h"
#include "urban/function_hooks.h"

#include "ps2_runtime.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <vector>

namespace urban::saveid
{
    namespace
    {
        // sub_002E8E18 builds a save's directory name (BISLPM-65853Y293_M%02d from the table at 0x600D10) on the memory
        // card object in a0 (+0x4 the kind: 0 _SYS, 1 _M, 2 _V; the name at +0x75F0) before a load or a save.
        constexpr uint32_t kMcName = 0x2E8E18u;
        constexpr uint32_t kMcKind = 0x4u, kMcMode = 0x20u, kMcSlot = 0x58u, kMcNameAt = 0x75F0u;
        // The directory entry the game read last (sceMcTblGetDir: created, modified, ..., the name at +0x20): static.
        constexpr uint32_t kEntry = 0x92C1C0u;
        constexpr uint32_t kTeamTable = 0x5D1240u, kTeamRoster = 0x120u, kRosterClass = 0x4u, kOwnTeamClass = 0x6659F0u;
        constexpr uint32_t kRamSize = 0x2000000u;

        struct Access
        {
            std::string id;
            uint32_t mode = 0u;
            bool inGame = false;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_root;
        std::mutex g_mutex; // everything below
        std::string g_current, g_pending, g_stateId;
        bool g_stateIdSet = false;
        std::vector<Access> g_accesses; // from the hook, for the frame hook
        std::atomic<uint32_t> g_generation{0};
        GuestFunction g_mcName = nullptr;
        bool g_debug = false;

        uint32_t read32(uint32_t address)
        {
            if (address >= kRamSize - 4u)
                return 0u;
            uint32_t value;
            std::memcpy(&value, g_rdram + address, 4);
            return value;
        }
        bool valid(uint32_t address, uint32_t size) { return address >= 0x100000u && address + size < kRamSize; }

        // The own team (by its roster's class) and its object, or -1.
        int ownTeam(uint32_t *object = nullptr)
        {
            for (int t = 0; t < 12; ++t)
            {
                const uint32_t team = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(team, kTeamRoster + 4u))
                    continue;
                const uint32_t roster = read32(team + kTeamRoster);
                if (valid(roster, 8u) && read32(roster + kRosterClass) == kOwnTeamClass)
                {
                    if (object)
                        *object = team;
                    return t;
                }
            }
            return -1;
        }

        // "BISLPM-65853Y293_M00" -> "M00"; "" for the system file or anything else.
        std::string idOf(const char *name)
        {
            static const char kPrefix[] = "BISLPM-65853Y293_";
            if (std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0)
                return std::string();
            const char *rest = name + sizeof(kPrefix) - 1;
            if ((rest[0] == 'M' || rest[0] == 'V') && rest[1] >= '0' && rest[1] <= '9' && rest[2] >= '0' && rest[2] <= '9' && rest[3] == '\0')
                return std::string(rest, 3);
            return std::string();
        }

        std::string entryName()
        {
            char name[33] = {};
            std::memcpy(name, g_rdram + kEntry + 0x20u, 32);
            return name;
        }
        std::string entryText()
        {
            const uint8_t *c = g_rdram + kEntry, *m = g_rdram + kEntry + 8u;
            char text[160];
            std::snprintf(text, sizeof(text), "%s created %04u-%02u-%02u %02u:%02u:%02u modified %04u-%02u-%02u %02u:%02u:%02u",
                          entryName().c_str(), c[6] | (c[7] << 8), c[5], c[4], c[3], c[2], c[1], m[6] | (m[7] << 8), m[5], m[4], m[3],
                          m[2], m[1]);
            return text;
        }

        // g_mutex held.
        void setCurrent(const std::string &id, const char *why)
        {
            if (id == g_current)
                return;
            std::fprintf(stderr, "[saveid] %s -> %s (%s)\n", g_current.empty() ? "(none)" : g_current.c_str(), id.empty() ? "(none)" : id.c_str(), why);
            g_current = id;
            g_generation.fetch_add(1u);
        }
        // A game saved under another name: its settings go with it (that slot's old ones were another game's).
        void copyProfile(const std::string &from, const std::string &to)
        {
            std::error_code error;
            const std::filesystem::path source = g_root / from, target = g_root / to;
            if (!std::filesystem::exists(source, error))
                return;
            std::filesystem::remove_all(target, error);
            std::filesystem::create_directories(target, error);
            std::filesystem::copy(source, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
            std::fprintf(stderr, "[saveid] settings of %s copied to %s%s\n", from.c_str(), to.c_str(), error ? " (failed)" : "");
        }

        void mcNameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t mc = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            g_mcName(rdram, ctx, runtime);
            if (!valid(mc, kMcNameAt + 32u))
                return;
            char name[33] = {};
            std::memcpy(name, rdram + mc + kMcNameAt, 32);
            uint32_t object = 0u;
            const int team = ownTeam(&object);
            if (g_debug)
                std::fprintf(stderr, "[saveid] mc name: %s (kind %u, mode %u, slot %u, from 0x%08x) team %d obj 0x%08x; entry %s\n", name,
                             read32(mc + kMcKind), read32(mc + kMcMode), rdram[mc + kMcSlot], ra, team, object, entryText().c_str());
            const std::string id = idOf(name);
            if (id.empty())
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_accesses.push_back(Access{id, read32(mc + kMcMode), team >= 0});
        }
        // Mode 1 on the name builder: a save from the game (hidden run 11-06: 設定・終了 → セーブ into a new copy, M01).
        constexpr uint32_t kModeSave = 1u;

        // The memory card file operations that format "/%s/%s" (dir in a1): which one a load and a save use.
        constexpr uint32_t kMcFileOps[4] = {0x2E5160u, 0x2E5288u, 0x2E53C0u, 0x2E5468u};
        GuestFunction g_fileOps[4] = {};
        template <int K>
        void fileOpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t dir = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            char name[33] = {};
            if (valid(dir, 32u))
                std::memcpy(name, rdram + dir, 32);
            uint32_t object = 0u;
            const int team = ownTeam(&object);
            if (g_debug)
                std::fprintf(stderr, "[saveid] file op %d (0x%08x) dir %s from 0x%08x team %d obj 0x%08x\n", K, kMcFileOps[K], name, ra, team, object);
            // A save's file opened while no game runs (the title's 続きから): the game about to start is that save.
            if (const std::string id = idOf(name); !id.empty() && team < 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_pending != id)
                    std::fprintf(stderr, "[saveid] loading %s\n", id.c_str());
                g_pending = id;
            }
            g_fileOps[K](rdram, ctx, runtime);
        }

        void frame()
        {
            static int lastTeam = -2;
            static uint32_t lastObject = 0u, lastLoads = 0u;
            static std::string lastEntry;
            uint32_t object = 0u;
            const int team = ownTeam(&object);
            const uint32_t loads = ps2x::savestate::loadCount();
            if (g_debug)
            {
                const std::string entry = entryText();
                if (team != lastTeam || object != lastObject || loads != lastLoads || entry != lastEntry)
                    std::fprintf(stderr, "[saveid] team %d obj 0x%08x loads %u entry %s\n", team, object, loads, entry.c_str());
                lastEntry = entry;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            if (loads != lastLoads)
            {
                // A save state was loaded: its own record, else (states made before this) the last save name in RAM.
                const std::string fromRam = idOf(entryName().c_str());
                setCurrent(g_stateIdSet ? g_stateId : team < 0 ? std::string() : fromRam.empty() ? std::string("new") : fromRam,
                           g_stateIdSet ? "save state" : "save state without a record: the last save name in RAM");
                g_stateIdSet = false;
                g_pending.clear();
                g_accesses.clear();
            }
            else
            {
                for (const Access &access : g_accesses)
                    if (access.mode == kModeSave && access.inGame)
                    {
                        // Saved from the game: this game is that save now (its settings copied there when it was another).
                        if (!g_current.empty() && g_current != access.id)
                            copyProfile(g_current, access.id);
                        setCurrent(access.id, "saved");
                    }
                g_accesses.clear();
                // A game starts (the own team appears, or another team object / team is there): the save just loaded, else new.
                if (team >= 0 && (team != lastTeam || object != lastObject) && lastLoads == loads)
                {
                    if (g_pending.empty())
                    {
                        std::error_code error;
                        std::filesystem::remove_all(g_root / "new", error); // a new game starts with no settings
                        setCurrent("new", "a new game");
                    }
                    else
                        setCurrent(g_pending, "loaded");
                    g_pending.clear();
                }
            }
            lastTeam = team;
            lastObject = object;
            lastLoads = loads;
        }
    }

    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram || !runtime.hasFunction(kMcName))
            return;
        g_rdram = rdram;
        g_root = userDir / "cheat_saves";
        const char *debug = std::getenv("URBAN_SAVEID_DEBUG");
        g_debug = debug && *debug == '1';
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kMcName, "save_name", &mcNameHook, ReplacementStatus::Native, "save identity: the save's directory name");
        g_mcName = hooks.original(kMcName);
        if (!g_mcName)
        {
            std::fprintf(stderr, "[saveid] hook not installed\n");
            return;
        }
        const GuestFunction fileHooks[4] = {&fileOpHook<0>, &fileOpHook<1>, &fileOpHook<2>, &fileOpHook<3>};
        for (int k = 0; k < 4; ++k)
            if (runtime.hasFunction(kMcFileOps[k]))
            {
                hooks.replace(kMcFileOps[k], "save_file_op", fileHooks[k], ReplacementStatus::Native, "save identity: memory card file operation");
                g_fileOps[k] = hooks.original(kMcFileOps[k]);
            }
        ps2x::savestate::registerExtension(
            "urban.saveid",
            [](ps2x::state::Writer &out) {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::string id = g_current;
                ps2x::state::field(out, id);
            },
            [](ps2x::state::Reader &in) {
                std::string id;
                ps2x::state::field(in, id);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stateId = id;
                g_stateIdSet = true;
            });
        ps2x::pnach::addFrameHook(frame);
        std::fprintf(stderr, "[saveid] watching the memory card saves (settings in %s)\n", g_root.string().c_str());
    }

    std::string current()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_current;
    }
    uint32_t generation() { return g_generation.load(); }
    std::filesystem::path folder(const std::string &id) { return g_root / id; }
    std::string describe(const std::string &id)
    {
        if (id.empty())
            return "게임 전 (세이브를 불러오거나 새 게임을 시작하면 정해짐)";
        if (id == "new")
            return "새 게임 (아직 저장 안 함: 저장하면 그 칸의 설정이 됨)";
        return "메모리카드 세이브 " + id;
    }
}
