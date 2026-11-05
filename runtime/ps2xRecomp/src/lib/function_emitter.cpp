#include "ps2recomp/Emitters/function_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/gif_dma_kick_analyzer.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace ps2recomp
{
    namespace
    {
        Instruction makeSyntheticDelaySlot(uint32_t address)
        {
            Instruction inst{};
            inst.address = address;
            inst.raw = 0;
            inst.opcode = OPCODE_SPECIAL;
            inst.function = SPECIAL_SLL;
            return inst;
        }
    }

    FunctionEmitter::FunctionEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FunctionEmitter::emit(
        const Function &function,
        const std::vector<Instruction> &instructions,
        bool useHeaders)
    {
        CodeGenerator &cg = m_codeGenerator;
        std::stringstream ss;
        cg.m_currentFunctionName = function.name;

        if (useHeaders)
        {
            // Not ps2_recompiled_functions.h: the functions this file calls directly are declared below (see the end),
            // so a change to the function map rebuilds the files that changed instead of every generated file.
            ss << "#include <stdexcept>\n";
            ss << "#include \"ps2_runtime_macros.h\"\n";
            ss << "#include \"ps2_runtime.h\"\n";
            ss << "#include <ps2_recompiled_stubs.h>\n\n";
            ss << "#include \"ps2_syscalls.h\"\n";
            ss << "#include \"ps2_stubs.h\"\n\n";
            ss << "#ifdef PS2_FUNCTION_LOG_TRACKER\n";
            ss << "#include \"ps2_log.h\"\n";
            ss << "#endif\n\n";
        }
        const size_t bodyStart = static_cast<size_t>(ss.tellp());

        CodeGenerator::AnalysisResult analysisResult = cg.collectInternalBranchTargets(function, instructions);
        std::vector<uint32_t> resumeTargets(analysisResult.resumeEntryPoints.begin(),
                                            analysisResult.resumeEntryPoints.end());
        auto resumeIt = cg.m_resumeEntryTargetsByOwner.find(function.start);
        if (resumeIt != cg.m_resumeEntryTargetsByOwner.end())
        {
            resumeTargets.insert(resumeTargets.end(), resumeIt->second.begin(), resumeIt->second.end());
        }
        std::sort(resumeTargets.begin(), resumeTargets.end());
        resumeTargets.erase(std::unique(resumeTargets.begin(), resumeTargets.end()), resumeTargets.end());
        for (uint32_t target : resumeTargets)
        {
            analysisResult.entryPoints.insert(target);
        }

        // Cheat regions in this function (tools/cheat_code.py): where every version continues and the targets of the
        // cheats' branches are labels.
        std::vector<const CheatRegion *> cheatRegions;
        for (auto it = cg.m_cheatRegions.lower_bound(function.start); it != cg.m_cheatRegions.end() && it->first < function.end; ++it)
        {
            const CheatRegion &region = it->second;
            cheatRegions.push_back(&region);
            if (region.end < function.end)
                analysisResult.entryPoints.insert(region.end);
            for (const auto &version : region.versions)
            {
                for (const Instruction &alt : version)
                {
                    const uint32_t op = alt.raw >> 26;
                    uint32_t target = 0;
                    if (op == 2u || op == 3u)
                        target = ((alt.address + 4u) & 0xF0000000u) | ((alt.raw & 0x03FFFFFFu) << 2);
                    else if (alt.hasDelaySlot && op != 0u)
                        target = alt.address + 4u + (static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(alt.raw & 0xFFFFu))) << 2);
                    if (target >= function.start && target < function.end)
                        analysisResult.entryPoints.insert(target);
                }
            }
        }
        size_t nextCheatRegion = 0;

        // A cheat region: the code each version the cheats can leave there compiles to, chosen by the words in memory
        // (as PCSX2 runs what is in memory); the original code below runs when none matches.
        auto emitCheatRegion = [&](const CheatRegion &region)
        {
            std::ostringstream out;
            const size_t count = (region.end - region.start) / 4u;
            out << "    // cheat region 0x" << std::hex << region.start << "-0x" << region.end
                << ": the versions cheats leave here, picked by the words in memory (tools/cheat_code.py)\n    {\n";
            for (size_t k = 0; k < count; ++k)
                out << "        const uint32_t cheat_" << std::hex << region.start << "_" << std::dec << k
                    << " = FAST_READ32(0x" << std::hex << (region.start + 4u * static_cast<uint32_t>(k)) << "u);\n";
            bool first = true;
            for (size_t v = 0; v < region.versions.size(); ++v)
            {
                std::ostringstream body;
                try
                {
                    const std::vector<Instruction> &version = region.versions[v];
                    for (size_t k = 0; k < version.size(); ++k)
                    {
                        const Instruction &alt = version[k];
                        if (alt.hasDelaySlot && k + 1 < version.size())
                        {
                            body << cg.handleBranchDelaySlots(alt, version[k + 1], function, analysisResult);
                            ++k;
                        }
                        else
                        {
                            body << "    ctx->pc = 0x" << std::hex << alt.address << "u;\n";
                            body << "    " << cg.translateInstruction(alt, MemoryAccessHint{}) << "\n";
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    if (cg.m_reporter)
                        cg.m_reporter->warningAt("cheat", function.name, region.start, std::string("cheat version left out: ") + e.what());
                    continue;
                }
                out << "        " << (first ? "if (" : "else if (");
                first = false;
                for (size_t k = 0; k < count; ++k)
                    out << (k ? " && " : "") << "cheat_" << std::hex << region.start << "_" << std::dec << k << " == 0x" << std::hex
                        << region.words[v][k] << "u";
                out << ")\n        {\n" << body.str();
                if (region.end < function.end)
                    out << "    ctx->pc = 0x" << std::hex << region.end << "u;\n    goto label_" << region.end << ";\n";
                else
                    out << "    ctx->pc = 0x" << std::hex << region.end << "u;\n    return;\n";
                out << "        }\n";
            }
            out << "    }\n" << std::dec;
            return out.str();
        };

        const std::unordered_set<uint32_t> &internalTargets = analysisResult.entryPoints;
        ConstantRegisterState constantRegisters;
        GifDmaKickPlan gifDmaKickPlan{};
        ss << "// Function: " << function.name << "\n";
        ss << "// Address: 0x" << std::hex << function.start << " - 0x" << function.end << std::dec << "\n";

        std::string sanitizedName = cg.getFunctionName(function.start);
        if (sanitizedName.empty())
        {
            std::stringstream nameBuilder;
            nameBuilder << "Errorfunc_" << std::hex << function.start;
            sanitizedName = nameBuilder.str();
        }

        ss << "void " << sanitizedName << "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {\n";
        ss << "#ifdef PS2_FUNCTION_LOG_TRACKER\n";
        ss << "    PS_LOG_ENTRY(\"" << sanitizedName << "\");\n";
        ss << "#endif\n";
        ss << "\n";
        if (!resumeTargets.empty())
        {
            ss << "    switch (ctx->pc) {\n";
            for (uint32_t target : resumeTargets)
            {
                ss << "        case 0x" << std::hex << target << "u: goto label_" << target << ";\n"
                   << std::dec;
            }
            ss << "        default: break;\n";
            ss << "    }\n\n";
        }
        ss << "    ctx->pc = 0x" << std::hex << function.start << "u;\n"
           << std::dec;
        ss << "\n";

        bool lastInstructionWasControlFlow = false;

        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const Instruction &inst = instructions[i];
            lastInstructionWasControlFlow = inst.hasDelaySlot;

            if (internalTargets.contains(inst.address))
            {
                constantRegisters.clear();
                ss << "label_" << std::hex << inst.address << std::dec << ":\n";
            }

            if (cg.m_emitInstructionComments)
            {
                ss << "    // 0x" << std::hex << inst.address << ": 0x" << inst.raw << std::dec;
                std::string disassembly = R5900Decoder::disassembleInstruction(inst);
                if (!disassembly.empty())
                {
                    ss << "  " << disassembly;
                }
                ss << "\n";
            }

            while (nextCheatRegion < cheatRegions.size() && cheatRegions[nextCheatRegion]->start < inst.address)
            {
                if (cg.m_reporter)
                    cg.m_reporter->warningAt("cheat", function.name, cheatRegions[nextCheatRegion]->start, "cheat region does not start at an instruction: left out");
                ++nextCheatRegion;
            }
            if (nextCheatRegion < cheatRegions.size() && cheatRegions[nextCheatRegion]->start == inst.address)
            {
                ss << emitCheatRegion(*cheatRegions[nextCheatRegion++]);
                constantRegisters.clear();
                gifDmaKickPlan = {};
            }

            try
            {
                if (inst.hasDelaySlot)
                {
                    const bool hasDecodedDelaySlot =
                        i + 1 < instructions.size() &&
                        instructions[i + 1].address == inst.address + 4u;

                    Instruction syntheticDelaySlot{};
                    const Instruction *delaySlot = nullptr;
                    if (hasDecodedDelaySlot)
                    {
                        delaySlot = &instructions[i + 1];
                    }
                    else
                    {
                        syntheticDelaySlot = makeSyntheticDelaySlot(inst.address + 4u);
                        delaySlot = &syntheticDelaySlot;
                    }

                    if (hasDecodedDelaySlot && internalTargets.contains(delaySlot->address))
                    {
                        ss << "label_" << std::hex << delaySlot->address << std::dec << ":\n";
                    }

                    if (gifDmaKickPlan.valid &&
                        gifDmaKickPlan.completesInDelaySlot &&
                        gifDmaKickPlan.branchIndex == i &&
                        hasDecodedDelaySlot)
                    {
                        ss << cg.handleBranchDelaySlots(
                            inst,
                            *delaySlot,
                            function,
                            analysisResult,
                            gifDmaDelaySlotOverride(*delaySlot, gifDmaKickPlan, cg.m_emitInstructionComments));
                        gifDmaKickPlan = {};
                    }
                    else
                    {
                        ss << cg.handleBranchDelaySlots(inst, *delaySlot, function, analysisResult);
                    }

                    if (hasDecodedDelaySlot)
                    {
                        ++i; // Skip delay slot instruction (handled inside branch logic)
                    }
                    constantRegisters.clear();
                }
                else
                {
                    if (!gifDmaKickPlan.valid)
                    {
                        gifDmaKickPlan = tryBuildGifDmaKickPlan(instructions, i, constantRegisters, internalTargets);
                    }

                    if (gifDmaKickPlan.suppresses(i))
                    {
                        const size_t slot = gifDmaKickPlan.slotFor(i);
                        emitGifDmaCapture(ss, gifDmaKickPlan, slot, "    ");

                        if (gifDmaKickPlan.completesAt(i))
                        {
                            ss << "    ctx->pc = 0x" << std::hex << inst.address << "u;\n"
                               << std::dec;
                            ss << "    " << gifDmaKickCall(gifDmaKickPlan) << "\n";
                            gifDmaKickPlan = {};
                        }

                        updateConstantRegisters(inst, constantRegisters);
                        continue;
                    }

                    ss << "    ctx->pc = 0x" << std::hex << inst.address << "u;\n"
                       << std::dec;
                    const MemoryAccessHint memoryHint = resolveMemoryAccessHint(inst, constantRegisters);
                    ss << "    " << cg.translateInstruction(inst, memoryHint);
                    if (inst.isMmio)
                    {
                        ss << " // MMIO: 0x" << std::hex << inst.mmioAddress << std::dec;
                    }
                    ss << "\n";

                    updateConstantRegisters(inst, constantRegisters);
                }
            }
            catch (const std::exception &e)
            {
                if (cg.m_reporter)
                {
                    std::ostringstream msg;
                    msg << "translation failed: " << e.what() << " raw=0x" << std::hex << inst.raw;
                    cg.m_reporter->errorAt("codegen", function.name, inst.address, msg.str());
                }

                throw;
            }
        }

        // Fallthrough with no terminating branch: publish the next PC so the EE dispatcher does not re-enter this function.
        if (!instructions.empty() && !lastInstructionWasControlFlow)
        {
            ss << "    ctx->pc = 0x" << std::hex << function.end << "u;\n"
               << std::dec;
        }

        ss << "}\n";
        std::string text = ss.str();
        if (!useHeaders)
            return text;

        // Declarations of the generated functions called directly (jump-table switches): unqualified names followed
        // by the guest-call arguments. ps2_syscalls::/ps2_stubs:: handlers come from their own headers.
        static constexpr std::string_view kCallArguments = "(rdram, ctx, runtime)";
        std::set<std::string> called;
        for (size_t at = text.find(kCallArguments, bodyStart); at != std::string::npos;
             at = text.find(kCallArguments, at + kCallArguments.size()))
        {
            size_t begin = at;
            while (begin > bodyStart && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_'))
                --begin;
            if (begin == at || text[begin - 1] == ':' || text[begin - 1] == '>' || text[begin - 1] == '.')
                continue;
            called.insert(text.substr(begin, at - begin));
        }
        std::string declarations;
        for (const std::string &name : called)
            declarations += "void " + name + "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);\n";
        if (!declarations.empty())
            text.insert(bodyStart, declarations + "\n");
        return text;
    }
}
