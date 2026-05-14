#include "function_caller.h"
#include "../core/log.h"

#include <cstring>
#include <cstdlib>

namespace bs1sdk {

UFunction* FindFunction(UStruct* cls, const std::string& funcName)
{
    if (!cls) return nullptr;

    UStruct* current = cls;
    int safetyLimit = 64;

    while (current && safetyLimit-- > 0) {
        UField* child = current->GetChildren();
        int childLimit = 2000;

        while (child && childLimit-- > 0) {
            std::string childClass = child->GetObjClassName();
            if (childClass == "Function") {
                if (child->GetName() == funcName) {
                    return reinterpret_cast<UFunction*>(child);
                }
            }
            child = child->GetNext();
        }

        UField* super = current->GetSuperField();
        current = super ? reinterpret_cast<UStruct*>(super) : nullptr;
    }

    return nullptr;
}

std::vector<FunctionParamInfo> GetFunctionParams(UFunction* func)
{
    std::vector<FunctionParamInfo> params;
    if (!func) return params;

    // Walk the function's children - parameters are UProperty objects with CPF_Parm flag
    UField* child = func->GetChildren();
    int limit = 200;

    while (child && limit-- > 0) {
        std::string childClass = child->GetObjClassName();
        if (childClass.find("Property") != std::string::npos) {
            UProperty* prop = reinterpret_cast<UProperty*>(child);
            uint32_t flags = prop->GetPropertyFlags();

            if (flags & CPF_Parm) {
                FunctionParamInfo pi;
                pi.Name = prop->GetName();
                pi.TypeName = childClass;
                pi.Offset = prop->GetPropertyOffset();
                pi.Size = prop->GetElementSize();
                pi.IsReturnParam = (flags & CPF_ReturnParm) != 0;
                pi.IsOutParam = (flags & CPF_OutParm) != 0;
                params.push_back(pi);
            }
        }
        child = child->GetNext();
    }

    return params;
}

std::vector<FunctionInfo> GetClassFunctions(UStruct* cls)
{
    std::vector<FunctionInfo> funcs;
    if (!cls) return funcs;

    UStruct* current = cls;
    int safetyLimit = 64;

    while (current && safetyLimit-- > 0) {
        UField* child = current->GetChildren();
        int childLimit = 2000;

        while (child && childLimit-- > 0) {
            std::string childClass = child->GetObjClassName();
            if (childClass == "Function") {
                UFunction* func = reinterpret_cast<UFunction*>(child);
                FunctionInfo fi;
                fi.Function = func;
                fi.Name = func->GetName();
                fi.FullName = current->GetName() + "." + fi.Name;
                fi.ParamsSize = func->GetPropertiesSize();
                fi.FunctionFlags = func->GetFunctionFlags();
                fi.Params = GetFunctionParams(func);
                funcs.push_back(fi);
            }
            child = child->GetNext();
        }

        UField* super = current->GetSuperField();
        current = super ? reinterpret_cast<UStruct*>(super) : nullptr;
    }

    return funcs;
}

std::string CallFunction(UObject* obj, UFunction* func, const std::vector<std::string>& args)
{
    if (!obj || !func) return "ERROR: null object or function";

    ProcessEventFn originalPE = GetOriginalProcessEvent();
    if (!originalPE) {
        // If ProcessEvent isn't hooked, call through vtable directly
        // ProcessEvent is a virtual function - call it via the object's vtable
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(obj);
        // We know the index from discovery (stored during hook init)
        // For now, use a simple approach: just call through our hook which passes through
    }

    // Get function parameters
    auto params = GetFunctionParams(func);
    int32_t paramsSize = func->GetPropertiesSize();

    // Allocate and zero the parameter buffer
    std::vector<uint8_t> parmsBuf(paramsSize > 0 ? paramsSize : 1, 0);

    // Fill in parameters from string arguments
    int argIdx = 0;
    std::string returnValue;

    for (auto& p : params) {
        if (p.IsReturnParam) continue; // skip return param during filling

        if (argIdx < (int)args.size()) {
            const std::string& argStr = args[argIdx];

            if (p.TypeName == "IntProperty") {
                int32_t val = std::atoi(argStr.c_str());
                std::memcpy(parmsBuf.data() + p.Offset, &val, sizeof(val));
            } else if (p.TypeName == "FloatProperty") {
                float val = std::strtof(argStr.c_str(), nullptr);
                std::memcpy(parmsBuf.data() + p.Offset, &val, sizeof(val));
            } else if (p.TypeName == "BoolProperty") {
                uint32_t val = (argStr == "true" || argStr == "1") ? 1 : 0;
                std::memcpy(parmsBuf.data() + p.Offset, &val, sizeof(val));
            } else if (p.TypeName == "ByteProperty") {
                uint8_t val = (uint8_t)std::atoi(argStr.c_str());
                std::memcpy(parmsBuf.data() + p.Offset, &val, sizeof(val));
            } else if (p.TypeName == "StrProperty") {
                // FString = TArray<wchar_t> - complex, skip for now
            }
            argIdx++;
        }
    }

    // Call ProcessEvent
    if (originalPE) {
        originalPE(obj, func, parmsBuf.data(), nullptr);
    } else {
        // Call through vtable - need the vtable index
        // For safety, try calling via a function pointer with __thiscall convention
        typedef void (__thiscall* DirectPEFn)(UObject*, UFunction*, void*, void*);
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(obj);
        // Use index 66 as discovered - but this is fragile
        // Better to just require the hook to be active
        return "ERROR: ProcessEvent not hooked. Run 'hookpe' first.";
    }

    // Read return value if present
    for (auto& p : params) {
        if (!p.IsReturnParam) continue;

        if (p.TypeName == "IntProperty") {
            int32_t val;
            std::memcpy(&val, parmsBuf.data() + p.Offset, sizeof(val));
            returnValue = "return: " + std::to_string(val);
        } else if (p.TypeName == "FloatProperty") {
            float val;
            std::memcpy(&val, parmsBuf.data() + p.Offset, sizeof(val));
            char buf[64]; std::snprintf(buf, sizeof(buf), "return: %.2f", val);
            returnValue = buf;
        } else if (p.TypeName == "BoolProperty") {
            uint32_t val;
            std::memcpy(&val, parmsBuf.data() + p.Offset, sizeof(val));
            returnValue = std::string("return: ") + (val ? "true" : "false");
        } else {
            returnValue = "return: (" + p.TypeName + ")";
        }
    }

    if (returnValue.empty()) returnValue = "OK (void)";
    return returnValue;
}

} // namespace bs1sdk
