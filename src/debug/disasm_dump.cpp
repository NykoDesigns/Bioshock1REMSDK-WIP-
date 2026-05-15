#include "disasm_dump.h"
#include "coop_debug.h"
#include "crash_handler.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../engine/uobject.h"
#include "../engine/world.h"
#include "../hooks/process_event.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <vector>
#include <map>

namespace bs1sdk {

// ─── Minimal x86 Instruction Decoder ────────────────────────────────────
// Not a full disassembler — just enough to identify key patterns:
//   CALL rel32, CALL [reg], JMP, CMP, TEST, MOV, PUSH, RET, NOP
// and extract operands for CALL targets.

struct DecodedInsn {
    uintptr_t addr;
    int length;
    std::string mnemonic;
    std::string operands;
    std::string comment;
    uint8_t bytes[16];
};

// Read a ModR/M byte and return the reg field
static int ModRM_Reg(uint8_t modrm) { return (modrm >> 3) & 7; }
static int ModRM_Mod(uint8_t modrm) { return (modrm >> 6) & 3; }
static int ModRM_RM(uint8_t modrm) { return modrm & 7; }

static const char* RegName32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};

// Get ModR/M instruction length (mod=0,1,2,3 variants)
static int ModRM_ExtraBytes(uint8_t modrm)
{
    int mod = ModRM_Mod(modrm);
    int rm  = ModRM_RM(modrm);
    if (mod == 3) return 0;                   // register direct
    if (mod == 0 && rm == 5) return 4;        // disp32
    if (mod == 0) return (rm == 4) ? 1 : 0;  // SIB byte
    if (mod == 1) return (rm == 4) ? 2 : 1;  // SIB + disp8
    if (mod == 2) return (rm == 4) ? 5 : 4;  // SIB + disp32
    return 0;
}

static bool TryDecode(const uint8_t* code, uintptr_t addr, DecodedInsn& out,
                       uintptr_t modBase, size_t modSize, uintptr_t gnativesAddr)
{
    out.addr = addr;
    memset(out.bytes, 0, sizeof(out.bytes));

    uint8_t b0 = code[0];

    // NOP
    if (b0 == 0x90) {
        out.length = 1; out.mnemonic = "nop";
        out.bytes[0] = b0;
        return true;
    }
    // INT3
    if (b0 == 0xCC) {
        out.length = 1; out.mnemonic = "int3";
        out.bytes[0] = b0;
        return true;
    }
    // RET
    if (b0 == 0xC3) {
        out.length = 1; out.mnemonic = "ret";
        out.bytes[0] = b0;
        return true;
    }
    // RET imm16
    if (b0 == 0xC2) {
        out.length = 3; out.mnemonic = "ret";
        memcpy(out.bytes, code, 3);
        uint16_t imm = *(uint16_t*)(code+1);
        char buf[16]; snprintf(buf, sizeof(buf), "0x%X", imm);
        out.operands = buf;
        return true;
    }
    // PUSH reg
    if (b0 >= 0x50 && b0 <= 0x57) {
        out.length = 1; out.mnemonic = "push";
        out.operands = RegName32[b0 - 0x50];
        out.bytes[0] = b0;
        return true;
    }
    // POP reg
    if (b0 >= 0x58 && b0 <= 0x5F) {
        out.length = 1; out.mnemonic = "pop";
        out.operands = RegName32[b0 - 0x58];
        out.bytes[0] = b0;
        return true;
    }
    // PUSH imm32
    if (b0 == 0x68) {
        out.length = 5; out.mnemonic = "push";
        memcpy(out.bytes, code, 5);
        uint32_t imm = *(uint32_t*)(code+1);
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", imm);
        out.operands = buf;
        // Check if it's a known address
        if (gnativesAddr && imm >= (uint32_t)gnativesAddr && imm < (uint32_t)gnativesAddr + 16384)
            out.comment = "<-- GNatives table region!";
        return true;
    }
    // PUSH imm8
    if (b0 == 0x6A) {
        out.length = 2; out.mnemonic = "push";
        memcpy(out.bytes, code, 2);
        char buf[16]; snprintf(buf, sizeof(buf), "0x%02X", code[1]);
        out.operands = buf;
        return true;
    }
    // CALL rel32
    if (b0 == 0xE8) {
        out.length = 5; out.mnemonic = "call";
        memcpy(out.bytes, code, 5);
        int32_t rel = *(int32_t*)(code+1);
        uintptr_t target = addr + 5 + rel;
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)target);
        out.operands = buf;
        out.comment = "DIRECT CALL";
        return true;
    }
    // JMP rel32
    if (b0 == 0xE9) {
        out.length = 5; out.mnemonic = "jmp";
        memcpy(out.bytes, code, 5);
        int32_t rel = *(int32_t*)(code+1);
        uintptr_t target = addr + 5 + rel;
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)target);
        out.operands = buf;
        return true;
    }
    // JMP rel8
    if (b0 == 0xEB) {
        out.length = 2; out.mnemonic = "jmp short";
        memcpy(out.bytes, code, 2);
        int8_t rel = (int8_t)code[1];
        uintptr_t target = addr + 2 + rel;
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)target);
        out.operands = buf;
        return true;
    }
    // Jcc rel8 (short conditional jumps)
    if (b0 >= 0x70 && b0 <= 0x7F) {
        static const char* jccNames[] = {
            "jo","jno","jb","jnb","jz","jnz","jbe","ja",
            "js","jns","jp","jnp","jl","jge","jle","jg"
        };
        out.length = 2; out.mnemonic = jccNames[b0 - 0x70];
        memcpy(out.bytes, code, 2);
        int8_t rel = (int8_t)code[1];
        uintptr_t target = addr + 2 + rel;
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)target);
        out.operands = buf;
        return true;
    }
    // Two-byte Jcc rel32 (0F 8x)
    if (b0 == 0x0F && code[1] >= 0x80 && code[1] <= 0x8F) {
        static const char* jccNames[] = {
            "jo","jno","jb","jnb","jz","jnz","jbe","ja",
            "js","jns","jp","jnp","jl","jge","jle","jg"
        };
        out.length = 6; out.mnemonic = jccNames[code[1] - 0x80];
        memcpy(out.bytes, code, 6);
        int32_t rel = *(int32_t*)(code+2);
        uintptr_t target = addr + 6 + rel;
        char buf[32]; snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)target);
        out.operands = buf;
        return true;
    }
    // CALL r/m32 (FF /2)
    if (b0 == 0xFF) {
        uint8_t modrm = code[1];
        int reg = ModRM_Reg(modrm);
        if (reg == 2) { // CALL
            int extra = ModRM_ExtraBytes(modrm);
            out.length = 2 + extra;
            out.mnemonic = "call";
            memcpy(out.bytes, code, out.length);
            int mod = ModRM_Mod(modrm);
            int rm = ModRM_RM(modrm);
            if (mod == 3) {
                out.operands = RegName32[rm];
            } else if (mod == 0 && rm == 5) {
                uint32_t disp = *(uint32_t*)(code+2);
                char buf[32]; snprintf(buf, sizeof(buf), "[0x%08X]", disp);
                out.operands = buf;
            } else {
                out.operands = "[indirect]";
            }
            out.comment = "INDIRECT CALL <-- check dispatch target";
            return true;
        }
        if (reg == 4) { // JMP r/m32
            int extra = ModRM_ExtraBytes(modrm);
            out.length = 2 + extra;
            out.mnemonic = "jmp";
            memcpy(out.bytes, code, out.length);
            out.operands = "[indirect]";
            return true;
        }
        if (reg == 6) { // PUSH r/m32
            int extra = ModRM_ExtraBytes(modrm);
            out.length = 2 + extra;
            out.mnemonic = "push";
            memcpy(out.bytes, code, out.length);
            out.operands = "[mem]";
            return true;
        }
    }
    // MOV reg, imm32
    if (b0 >= 0xB8 && b0 <= 0xBF) {
        out.length = 5; out.mnemonic = "mov";
        memcpy(out.bytes, code, 5);
        uint32_t imm = *(uint32_t*)(code+1);
        char buf[64]; snprintf(buf, sizeof(buf), "%s, 0x%08X", RegName32[b0-0xB8], imm);
        out.operands = buf;
        if (gnativesAddr && imm >= (uint32_t)gnativesAddr && imm < (uint32_t)gnativesAddr + 16384)
            out.comment = "<-- GNatives table reference!";
        return true;
    }
    // CMP [reg+disp8], imm  (common for checking iNative offset)
    if (b0 == 0x80 || b0 == 0x83) {
        uint8_t modrm = code[1];
        int reg = ModRM_Reg(modrm);
        if (reg == 7) { // CMP
            int extra = ModRM_ExtraBytes(modrm);
            int immSize = (b0 == 0x80) ? 1 : 1; // sign-extended
            out.length = 2 + extra + immSize;
            out.mnemonic = "cmp";
            memcpy(out.bytes, code, out.length);
            int mod = ModRM_Mod(modrm);
            int rm = ModRM_RM(modrm);
            if (mod == 1) { // [reg+disp8]
                int8_t disp = (int8_t)code[2];
                uint8_t imm = code[3];
                char buf[64]; snprintf(buf, sizeof(buf), "[%s+0x%02X], 0x%02X",
                    RegName32[rm], (uint8_t)disp, imm);
                out.operands = buf;
                // Check if comparing at iNative offset (0x68)
                if ((uint8_t)disp == 0x68 || (uint8_t)disp == 0x6A || (uint8_t)disp == 0x6B)
                    out.comment = "<-- comparing UFunction field near iNative(0x68)/OperPrec(0x6A)/NumParms(0x6B)!";
            } else {
                out.operands = "[mem], imm";
            }
            return true;
        }
    }
    // CMP reg, imm32 (3D = cmp eax, imm32; 81 /7 = cmp r/m32, imm32)
    if (b0 == 0x3D) {
        out.length = 5; out.mnemonic = "cmp";
        memcpy(out.bytes, code, 5);
        uint32_t imm = *(uint32_t*)(code+1);
        char buf[32]; snprintf(buf, sizeof(buf), "eax, 0x%08X", imm);
        out.operands = buf;
        return true;
    }
    // TEST al, imm8
    if (b0 == 0xA8) {
        out.length = 2; out.mnemonic = "test";
        memcpy(out.bytes, code, 2);
        char buf[32]; snprintf(buf, sizeof(buf), "al, 0x%02X", code[1]);
        out.operands = buf;
        return true;
    }
    // TEST reg, reg (85)
    if (b0 == 0x85) {
        out.length = 2; out.mnemonic = "test";
        memcpy(out.bytes, code, 2);
        int reg = ModRM_Reg(code[1]);
        int rm = ModRM_RM(code[1]);
        char buf[32]; snprintf(buf, sizeof(buf), "%s, %s", RegName32[rm], RegName32[reg]);
        out.operands = buf;
        return true;
    }
    // MOVZX (0F B6 / 0F B7)
    if (b0 == 0x0F && (code[1] == 0xB6 || code[1] == 0xB7)) {
        uint8_t modrm = code[2];
        int extra = ModRM_ExtraBytes(modrm);
        out.length = 3 + extra;
        out.mnemonic = (code[1] == 0xB6) ? "movzx" : "movzx";
        memcpy(out.bytes, code, out.length);
        int reg = ModRM_Reg(modrm);
        int mod = ModRM_Mod(modrm);
        int rm = ModRM_RM(modrm);
        if (mod == 1) {
            int8_t disp = (int8_t)code[3];
            char buf[64]; snprintf(buf, sizeof(buf), "%s, %s [%s+0x%02X]",
                RegName32[reg], (code[1]==0xB6)?"byte":"word", RegName32[rm], (uint8_t)disp);
            out.operands = buf;
            if ((uint8_t)disp == 0x68)
                out.comment = "<-- reading iNative from UFunction+0x68!";
            else if ((uint8_t)disp == 0x6B)
                out.comment = "<-- reading NumParms from UFunction+0x6B!";
        } else {
            out.operands = "reg, [mem]";
        }
        return true;
    }
    // MOV reg, [reg+disp8/32] (8B)
    if (b0 == 0x8B) {
        uint8_t modrm = code[1];
        int extra = ModRM_ExtraBytes(modrm);
        out.length = 2 + extra;
        out.mnemonic = "mov";
        memcpy(out.bytes, code, out.length);
        int reg = ModRM_Reg(modrm);
        int mod = ModRM_Mod(modrm);
        int rm = ModRM_RM(modrm);
        if (mod == 1) {
            int8_t disp = (int8_t)code[2];
            char buf[64]; snprintf(buf, sizeof(buf), "%s, [%s+0x%02X]",
                RegName32[reg], RegName32[rm], (uint8_t)disp);
            out.operands = buf;
            if ((uint8_t)disp == 0x70)
                out.comment = "<-- reading NativeFunc from UFunction+0x70!";
            else if ((uint8_t)disp == 0x68)
                out.comment = "<-- reading iNative from UFunction+0x68!";
            else if ((uint8_t)disp == 0x6C)
                out.comment = "<-- reading ParmsSize from UFunction+0x6C!";
        } else if (mod == 2) {
            int32_t disp = *(int32_t*)(code+2);
            char buf[64]; snprintf(buf, sizeof(buf), "%s, [%s+0x%X]",
                RegName32[reg], RegName32[rm], disp);
            out.operands = buf;
        } else if (mod == 3) {
            char buf[32]; snprintf(buf, sizeof(buf), "%s, %s", RegName32[reg], RegName32[rm]);
            out.operands = buf;
        } else {
            out.operands = "reg, [mem]";
        }
        return true;
    }

    // Fallback: unknown byte
    out.length = 1; out.mnemonic = "db";
    out.bytes[0] = b0;
    char buf[8]; snprintf(buf, sizeof(buf), "0x%02X", b0);
    out.operands = buf;
    return true;
}

// ─── Dump Helpers ────────────────────────────────────────────────────────

static void DumpFunction(std::ofstream& out, const char* label, uintptr_t addr, int maxBytes,
                          uintptr_t modBase, size_t modSize, uintptr_t gnativesAddr)
{
    if (!addr || !IsSafeToRead(reinterpret_cast<const void*>(addr), maxBytes)) {
        out << label << ": INVALID or unreadable at 0x" << std::hex << addr << "\n\n";
        return;
    }

    out << "═══════════════════════════════════════════════════════════════\n";
    out << label << " @ 0x" << std::hex << std::setfill('0') << std::setw(8) << addr;
    out << "  (RVA: 0x" << std::setw(8) << (addr - modBase) << ")\n";
    out << "═══════════════════════════════════════════════════════════════\n\n";

    const uint8_t* code = reinterpret_cast<const uint8_t*>(addr);
    int offset = 0;
    int insnCount = 0;
    int maxInsns = 500; // safety limit

    while (offset < maxBytes && insnCount < maxInsns) {
        DecodedInsn insn{};
        if (!TryDecode(code + offset, addr + offset, insn, modBase, modSize, gnativesAddr))
            break;

        // Format: ADDR  BYTES          MNEMONIC OPERANDS    ; COMMENT
        out << "  " << std::hex << std::setfill('0') << std::setw(8) << insn.addr << "  ";

        // Bytes
        std::ostringstream byteStr;
        for (int i = 0; i < insn.length && i < 8; i++)
            byteStr << std::hex << std::setfill('0') << std::setw(2) << (int)code[offset+i] << " ";
        std::string bs = byteStr.str();
        out << std::left << std::setfill(' ') << std::setw(25) << bs;

        // Mnemonic + operands
        std::string insnStr = insn.mnemonic;
        if (!insn.operands.empty()) insnStr += " " + insn.operands;
        out << std::setw(40) << insnStr;

        // Comment
        if (!insn.comment.empty()) out << " ; " << insn.comment;
        out << "\n";

        offset += insn.length;
        insnCount++;

        // Stop at RET or INT3 (end of function)
        if (insn.mnemonic == "ret" || insn.mnemonic == "int3") {
            // Check if next byte is also INT3 (padding)
            if (offset < maxBytes && code[offset] == 0xCC) {
                out << "  --- function end ---\n";
                break;
            }
        }
    }
    out << "\n";
}

// ─── Public API ──────────────────────────────────────────────────────────

void DumpProcessEventDisasm()
{
    std::string dir = GetDebugDir();
    std::string path = dir + "pe_disasm.txt";
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_ERROR("[DisasmDump] Cannot write to {}", path);
        return;
    }

    uintptr_t modBase = Memory::GetModuleBase(nullptr);
    size_t modSize = Memory::GetModuleSize(nullptr);
    uintptr_t gnativesAddr = GetNativesTableAddress();

    out << "╔═══════════════════════════════════════════════════════════════╗\n";
    out << "║  ProcessEvent Disassembly Dump                                ║\n";
    out << "║  BioshockHD.exe base: 0x" << std::hex << std::setfill('0') << std::setw(8) << modBase << "                        ║\n";
    out << "║  GNatives table:      0x" << std::setw(8) << gnativesAddr << "                        ║\n";
    out << "╚═══════════════════════════════════════════════════════════════╝\n\n";

    // 1. ProcessEvent itself
    ProcessEventFn origPE = GetOriginalProcessEvent();
    uintptr_t peAddr = reinterpret_cast<uintptr_t>(origPE);
    DumpFunction(out, "UObject::ProcessEvent", peAddr, 2048, modBase, modSize, gnativesAddr);

    // 2. Dump the first 16 bytes of GNatives entries for key indices
    out << "═══════════════════════════════════════════════════════════════\n";
    out << "Key GNatives Entries\n";
    out << "═══════════════════════════════════════════════════════════════\n\n";

    struct { uint16_t idx; const char* name; } keyNatives[] = {
        {4014, "Actor::SetLocation"},
        {4343, "Actor::SetRotation"},
        {4338, "Actor::Move"},
        {4336, "Actor::SetCastShadowMapShadow"},
        {4012, "Projector::AbandonProjector"},
    };

    for (auto& kn : keyNatives) {
        NativeFunc nf = GetNative(kn.idx);
        uintptr_t nfAddr = reinterpret_cast<uintptr_t>(nf);
        out << "  GNatives[" << std::dec << kn.idx << "] = 0x" << std::hex << std::setfill('0')
            << std::setw(8) << nfAddr << "  " << kn.name;
        if (!nf) out << "  (NULL!)";
        out << "\n";
    }
    out << "\n";

    // 3. Dump SetLocation native function
    NativeFunc setLocNative = GetNative(4014);
    if (setLocNative) {
        DumpFunction(out, "Actor::SetLocation (native 4014)", reinterpret_cast<uintptr_t>(setLocNative),
                     512, modBase, modSize, gnativesAddr);
    }

    // 4. Dump SetRotation native function
    NativeFunc setRotNative = GetNative(4343);
    if (setRotNative) {
        DumpFunction(out, "Actor::SetRotation (native 4343)", reinterpret_cast<uintptr_t>(setRotNative),
                     512, modBase, modSize, gnativesAddr);
    }

    // 5. Dump Actor::Move native function
    NativeFunc moveNative = GetNative(4338);
    if (moveNative) {
        DumpFunction(out, "Actor::Move (native 4338)", reinterpret_cast<uintptr_t>(moveNative),
                     512, modBase, modSize, gnativesAddr);
    }

    out << "\n═══════════════════════════════════════════════════════════════\n";
    out << "ANALYSIS GUIDE:\n";
    out << "═══════════════════════════════════════════════════════════════\n";
    out << "Look for in ProcessEvent:\n";
    out << "  1. movzx reg, [ecx+0x68]  — reads iNative index from UFunction\n";
    out << "  2. test/cmp on iNative     — checks if function is native\n";
    out << "  3. mov reg, [table+idx*4]  — GNatives lookup\n";
    out << "  4. call reg                — dispatch to native\n";
    out << "  5. mov reg, [ecx+0x70]     — reads NativeFunc pointer\n";
    out << "  6. call [ecx+0x70]         — direct NativeFunc dispatch\n";
    out << "\nIf PE checks NativeFunc BEFORE iNative, that explains the crash.\n";
    out << "If PE checks iNative first, the GNatives patch should work.\n";

    out.close();
    LOG_INFO("[DisasmDump] ProcessEvent disasm dumped to {}", path);
}

void DumpNativeDisasm(uint16_t nativeIndex, const std::string& name)
{
    NativeFunc nf = GetNative(nativeIndex);
    if (!nf) {
        LOG_WARN("[DisasmDump] Native {} is NULL", nativeIndex);
        return;
    }

    std::string dir = GetDebugDir();
    char filename[128];
    snprintf(filename, sizeof(filename), "native_%d_disasm.txt", nativeIndex);
    std::string path = dir + filename;
    std::ofstream out(path);

    uintptr_t modBase = Memory::GetModuleBase(nullptr);
    size_t modSize = Memory::GetModuleSize(nullptr);
    uintptr_t gnativesAddr = GetNativesTableAddress();

    std::string label = name.empty()
        ? ("Native " + std::to_string(nativeIndex))
        : (name + " (native " + std::to_string(nativeIndex) + ")");

    DumpFunction(out, label.c_str(), reinterpret_cast<uintptr_t>(nf), 512, modBase, modSize, gnativesAddr);
    out.close();
    LOG_INFO("[DisasmDump] {} dumped to {}", label, path);
}

void DumpStructValidation()
{
    std::string dir = GetDebugDir();
    std::string path = dir + "struct_validation.txt";
    std::ofstream out(path);

    out << "╔═══════════════════════════════════════════════════════════════╗\n";
    out << "║  UE2.5 Struct Layout Validation                               ║\n";
    out << "║  Comparing runtime offsets vs known UE2 source layouts        ║\n";
    out << "╚═══════════════════════════════════════════════════════════════╝\n\n";

    // Known UE2 UFunction layout (from UT2004 / UE2 source)
    out << "─── UFunction Layout (expected vs actual) ───\n\n";
    out << "  Field            Expected    Actual      Status\n";
    out << "  ─────────────    ────────    ──────      ──────\n";

    // Find a known UFunction to validate against
    UObject* player = FindObjectByClassName("ShockPlayer");
    UFunction* testFunc = nullptr;
    if (player) {
        UStruct* cls = reinterpret_cast<UStruct*>(player->GetClass());
        if (cls) {
            UField* child = cls->GetChildren();
            int limit = 500;
            while (child && limit-- > 0) {
                if (child->GetObjClassName() == "Function") {
                    testFunc = reinterpret_cast<UFunction*>(child);
                    break;
                }
                child = child->GetNext();
            }
        }
    }

    if (testFunc) {
        out << "  Test function: " << testFunc->GetName() << "\n\n";

        // Dump raw bytes of UFunction for comparison
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(testFunc);

        // UE2 known offsets for UFunction
        struct FieldCheck {
            int offset; const char* name; const char* expectedType; int size;
        };
        FieldCheck checks[] = {
            {0x00, "VfTable",         "ptr",     4},
            {0x04, "ObjectIndex",     "int32",   4},
            {0x28, "Name (FName)",    "int32",   4},
            {0x30, "Outer",           "ptr",     4},
            {0x34, "ObjectFlags",     "uint32",  4},
            {0x40, "SuperField",      "ptr",     4},
            {0x44, "Next",            "ptr",     4},
            {0x48, "Children",        "ptr",     4},
            {0x64, "FunctionFlags",   "uint32",  4},
            {0x68, "iNative",         "uint16",  2},
            {0x6A, "OperPrecedence",  "uint8",   1},
            {0x6B, "NumParms",        "uint8",   1},
            {0x6C, "ParmsSize",       "uint16",  2},
            {0x6E, "ReturnValueOff",  "uint16",  2},
            {0x70, "NativeFunc",      "ptr",     4},
        };

        for (auto& c : checks) {
            out << "  " << std::left << std::setw(18) << c.name
                << "0x" << std::hex << std::setfill('0') << std::setw(2) << c.offset
                << "        ";

            // Read actual value
            if (c.size == 4) {
                uint32_t val = *(uint32_t*)(raw + c.offset);
                out << "0x" << std::setw(8) << val;
            } else if (c.size == 2) {
                uint16_t val = *(uint16_t*)(raw + c.offset);
                out << "0x" << std::setw(4) << val << "    ";
            } else if (c.size == 1) {
                uint8_t val = *(uint8_t*)(raw + c.offset);
                out << "0x" << std::setw(2) << (int)val << "      ";
            }

            // Validate
            if (std::string(c.name) == "iNative") {
                uint16_t val = *(uint16_t*)(raw + c.offset);
                if (val > 0 && val < 4096) out << "  OK (valid index)";
                else if (val == 0) out << "  OK (not native)";
                else out << "  SUSPECT (>4096)";
            } else if (std::string(c.name) == "NativeFunc") {
                uintptr_t val = *(uintptr_t*)(raw + c.offset);
                if (val == 0) out << "  NULL";
                else if (IsSafeToRead(reinterpret_cast<const void*>(val), 4)) out << "  OK (readable)";
                else out << "  BAD (not readable!)";
            } else if (std::string(c.name) == "NumParms") {
                uint8_t val = *(uint8_t*)(raw + c.offset);
                if (val < 20) out << "  OK";
                else out << "  SUSPECT";
            }
            out << "\n";
        }

        // Hex dump of UFunction raw bytes (first 0x80 bytes)
        out << "\n  Raw hex dump (first 0x80 bytes):\n";
        for (int row = 0; row < 8; row++) {
            out << "  " << std::hex << std::setfill('0') << std::setw(4) << (row*16) << ": ";
            for (int col = 0; col < 16; col++) {
                out << std::setw(2) << (int)raw[row*16+col] << " ";
            }
            out << " | ";
            for (int col = 0; col < 16; col++) {
                char ch = raw[row*16+col];
                out << ((ch >= 32 && ch < 127) ? ch : '.');
            }
            out << "\n";
        }

        // Also dump SetLocation UFunction specifically
        out << "\n─── SetLocation UFunction ───\n\n";
        UFunction* setLocFunc = nullptr;
        // Walk up class hierarchy to find it
        UStruct* cls = reinterpret_cast<UStruct*>(player->GetClass());
        UField* walk = reinterpret_cast<UField*>(cls);
        int depth = 0;
        while (walk && depth < 64 && !setLocFunc) {
            UStruct* cur = reinterpret_cast<UStruct*>(walk);
            UField* child = cur->GetChildren();
            int limit = 2000;
            while (child && limit-- > 0) {
                if (child->GetObjClassName() == "Function" && child->GetName() == "SetLocation") {
                    setLocFunc = reinterpret_cast<UFunction*>(child);
                    break;
                }
                child = child->GetNext();
            }
            walk = cur->GetSuperField();
            depth++;
        }

        if (setLocFunc) {
            const uint8_t* slRaw = reinterpret_cast<const uint8_t*>(setLocFunc);
            uint16_t iNative = *(uint16_t*)(slRaw + 0x68);
            uintptr_t nativeFunc = *(uintptr_t*)(slRaw + 0x70);
            uint32_t funcFlags = *(uint32_t*)(slRaw + 0x64);
            uint8_t numParms = *(uint8_t*)(slRaw + 0x6B);
            uint16_t parmsSize = *(uint16_t*)(slRaw + 0x6C);

            out << "  Name:        " << setLocFunc->GetName() << "\n";
            out << "  Address:     0x" << std::hex << std::setfill('0') << std::setw(8) << (uintptr_t)setLocFunc << "\n";
            out << "  iNative:     " << std::dec << iNative << " (0x" << std::hex << iNative << ")\n";
            out << "  NativeFunc:  0x" << std::setw(8) << nativeFunc;
            if (nativeFunc == 0) out << "  (NULL!)";
            else if (IsSafeToRead(reinterpret_cast<const void*>(nativeFunc), 4)) out << "  (readable)";
            else out << "  (NOT READABLE!)";
            out << "\n";
            out << "  FuncFlags:   0x" << std::setw(8) << funcFlags << "\n";
            out << "  NumParms:    " << std::dec << (int)numParms << "\n";
            out << "  ParmsSize:   " << parmsSize << "\n";

            // Compare NativeFunc with GNatives entry
            if (iNative > 0 && iNative < 4096) {
                NativeFunc gnEntry = GetNative(iNative);
                uintptr_t gnAddr = reinterpret_cast<uintptr_t>(gnEntry);
                out << "\n  GNatives[" << iNative << "] = 0x" << std::hex << std::setw(8) << gnAddr << "\n";
                if (gnAddr == nativeFunc) out << "  STATUS: NativeFunc MATCHES GNatives — dispatch should work!\n";
                else if (nativeFunc == 0) out << "  STATUS: NativeFunc is NULL but GNatives has it — PE may skip to script execution!\n";
                else out << "  STATUS: MISMATCH — NativeFunc=0x" << std::setw(8) << nativeFunc << " vs GNatives=0x" << std::setw(8) << gnAddr << "\n";
            }

            out << "\n  Raw hex dump (first 0x80 bytes):\n";
            for (int row = 0; row < 8; row++) {
                out << "  " << std::hex << std::setfill('0') << std::setw(4) << (row*16) << ": ";
                for (int col = 0; col < 16; col++) {
                    out << std::setw(2) << (int)slRaw[row*16+col] << " ";
                }
                out << " | ";
                for (int col = 0; col < 16; col++) {
                    char ch = slRaw[row*16+col];
                    out << ((ch >= 32 && ch < 127) ? ch : '.');
                }
                out << "\n";
            }
        } else {
            out << "  SetLocation UFunction NOT FOUND in class hierarchy!\n";
        }
    } else {
        out << "  Could not find a test UFunction (ShockPlayer not loaded?)\n";
    }

    out.close();
    LOG_INFO("[DisasmDump] Struct validation dumped to {}", path);
}

void DumpFunctionAddresses()
{
    std::string dir = GetDebugDir();
    std::string path = dir + "function_addresses.txt";
    std::ofstream out(path);

    uintptr_t modBase = Memory::GetModuleBase(nullptr);
    ProcessEventFn origPE = GetOriginalProcessEvent();
    uintptr_t gnativesAddr = GetNativesTableAddress();

    out << "╔═══════════════════════════════════════════════════════════════╗\n";
    out << "║  Key Function Addresses (for IDA/Ghidra/x64dbg)              ║\n";
    out << "╚═══════════════════════════════════════════════════════════════╝\n\n";

    out << "Module base:     0x" << std::hex << std::setfill('0') << std::setw(8) << modBase << "\n";
    out << "ProcessEvent:    0x" << std::setw(8) << (uintptr_t)origPE
        << "  (RVA: 0x" << std::setw(8) << ((uintptr_t)origPE - modBase) << ")\n";
    out << "GNatives table:  0x" << std::setw(8) << gnativesAddr
        << "  (RVA: 0x" << std::setw(8) << (gnativesAddr - modBase) << ")\n\n";

    // Dump all populated GNatives
    out << "─── All populated GNatives entries ───\n\n";
    int count = 0;
    for (int i = 0; i < 4096; i++) {
        NativeFunc nf = GetNative((uint16_t)i);
        if (nf) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(nf);
            out << "  [" << std::dec << std::setw(4) << i << "] 0x" << std::hex << std::setfill('0')
                << std::setw(8) << addr << "  (RVA: 0x" << std::setw(8) << (addr - modBase) << ")\n";
            count++;
        }
    }
    out << "\nTotal: " << std::dec << count << " native functions\n";

    out.close();
    LOG_INFO("[DisasmDump] Function addresses dumped to {}", path);
}

} // namespace bs1sdk
