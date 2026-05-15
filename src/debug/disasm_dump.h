#pragma once

#include <string>
#include <cstdint>

namespace bs1sdk {

/// Dump ProcessEvent disassembly + dispatch analysis to debug_dumps/pe_disasm.txt
/// Shows raw bytes, basic instruction decode, and identifies:
///   - CALL instructions (what sub-functions PE calls)
///   - CMP/TEST on iNative offset (dispatch logic)
///   - Indirect calls through function pointers (NativeFunc dispatch)
///   - References to GNatives table
void DumpProcessEventDisasm();

/// Dump a native function's disassembly by index
/// Output: debug_dumps/native_<index>_disasm.txt
void DumpNativeDisasm(uint16_t nativeIndex, const std::string& name = "");

/// Dump UE2 struct layout validation against known offsets
/// Compares runtime-discovered offsets with expected UE2 source layouts
/// Output: debug_dumps/struct_validation.txt
void DumpStructValidation();

/// Dump all key function addresses for external analysis (IDA/Ghidra/x64dbg)
/// Output: debug_dumps/function_addresses.txt
void DumpFunctionAddresses();

} // namespace bs1sdk
