#pragma once

#include "uobject.h"
#include "../hooks/process_event.h"
#include <string>
#include <vector>

namespace bs1sdk {

/// Information about a UFunction parameter
struct FunctionParamInfo {
    std::string Name;
    std::string TypeName;   // IntProperty, FloatProperty, etc.
    int32_t Offset;         // offset within the param buffer
    int32_t Size;           // element size
    bool IsReturnParam;     // CPF_ReturnParm flag
    bool IsOutParam;        // CPF_OutParm flag
};

/// Discovered UFunction with metadata
struct FunctionInfo {
    UFunction* Function;
    std::string Name;
    std::string FullName;      // Class.FunctionName
    int32_t ParamsSize;        // total parameter buffer size
    uint32_t FunctionFlags;
    std::vector<FunctionParamInfo> Params;
};

/// Find a UFunction on a class by name (searches inheritance chain)
UFunction* FindFunction(UStruct* cls, const std::string& funcName);

/// Get parameter information for a UFunction
std::vector<FunctionParamInfo> GetFunctionParams(UFunction* func);

/// Get all functions on a class (including inherited)
std::vector<FunctionInfo> GetClassFunctions(UStruct* cls);

/// Call a UFunction on an object with string arguments.
/// Parses arguments based on parameter types.
/// Returns a string description of the return value (if any).
std::string CallFunction(UObject* obj, UFunction* func, const std::vector<std::string>& args);

/// UFunction flags (UE2.5)
enum EFunctionFlags : uint32_t {
    FUNC_Final       = 0x00000001,
    FUNC_Defined     = 0x00000002,
    FUNC_Iterator    = 0x00000004,
    FUNC_Latent      = 0x00000008,
    FUNC_PreOperator = 0x00000010,
    FUNC_Singular    = 0x00000020,
    FUNC_Net         = 0x00000040,
    FUNC_NetReliable = 0x00000080,
    FUNC_Simulated   = 0x00000100,
    FUNC_Exec        = 0x00000200,
    FUNC_Native      = 0x00000400,
    FUNC_Event       = 0x00000800,
    FUNC_Operator    = 0x00001000,
    FUNC_Static      = 0x00002000,
    FUNC_NoExport    = 0x00004000,
    FUNC_Const       = 0x00008000,
    FUNC_Invariant   = 0x00010000,
};

/// UProperty flags relevant for function params
enum EPropertyFlags : uint32_t {
    CPF_Parm       = 0x00000080,
    CPF_OutParm    = 0x00000100,
    CPF_ReturnParm = 0x00000400,
    CPF_OptionalParm = 0x00000010,
};

} // namespace bs1sdk
