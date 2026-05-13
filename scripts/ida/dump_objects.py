"""
IDA Pro script: Locate GObjects and GNames in BioShock Remastered.

Strategy:
1. Search for RTTI type descriptors containing "UObject"
2. Find cross-references to the GObjects global array
3. Identify the GNames global name table
4. Dump findings to a JSON file for use by the SDK

Usage in IDA:
    File -> Script File -> dump_objects.py
    
Expected results:
    - GObjects address
    - GNames address  
    - UObject vtable address
    - Key function addresses (ProcessEvent, Tick, etc.)
"""

import idautils
import idaapi
import idc
import json
import os

OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 
                           "../../docs/reverse-engineering/ida_findings.json")

findings = {
    "gobjects": None,
    "gnames": None,
    "uobject_vtable": None,
    "functions": {},
    "rtti_classes": [],
    "patterns_validated": []
}

def find_rtti_descriptors():
    """Search for MSVC RTTI type descriptors"""
    print("[*] Searching for RTTI type descriptors...")
    
    # Search for ".?AV" prefix (MSVC mangled type names)
    pattern = b".?AV"
    
    addr = idc.get_inf_attr(idc.INF_MIN_EA)
    end = idc.get_inf_attr(idc.INF_MAX_EA)
    
    classes_found = []
    
    while addr < end:
        addr = ida_bytes.bin_search(addr, end, pattern, None, 
                                     ida_bytes.BIN_SEARCH_FORWARD, 0)
        if addr == idaapi.BADADDR:
            break
            
        # Read the full type name
        name = idc.get_strlit_contents(addr, -1, idc.STRTYPE_C)
        if name:
            name = name.decode('utf-8', errors='ignore')
            classes_found.append({"address": hex(addr), "name": name})
            
            # Flag important UE classes
            if "UObject" in name or "AActor" in name or "UEngine" in name:
                print(f"  [+] Found: {name} at {hex(addr)}")
        
        addr += 1
    
    findings["rtti_classes"] = classes_found
    print(f"[*] Found {len(classes_found)} RTTI descriptors")
    return classes_found


def find_gobjects_pattern():
    """
    Search for the GObjects access pattern:
    mov ecx, [GObjects]     ; 8B 0D xx xx xx xx
    mov eax, [ecx+reg*4]    ; 8B 04 81 / 8B 04 91 / etc.
    """
    print("[*] Searching for GObjects access pattern...")
    
    # Pattern: mov ecx, [imm32]; mov eax, [ecx+eax*4]; test eax, eax
    patterns = [
        "8B 0D ?? ?? ?? ?? 8B 04 81 85 C0",
        "8B 0D ?? ?? ?? ?? 8B 04 81",
        "A1 ?? ?? ?? ?? 8B 0C 88",  # mov eax, [GObjects]; mov ecx, [eax+ecx*4]
    ]
    
    text_seg = idaapi.get_segm_by_name(".text")
    if not text_seg:
        print("  [!] .text segment not found")
        return
    
    for pat_str in patterns:
        pat = pat_str.replace("??", "?").replace(" ", " ")
        addr = ida_search.find_binary(text_seg.start_ea, text_seg.end_ea, 
                                       pat_str, 16, ida_search.SEARCH_DOWN)
        if addr != idaapi.BADADDR:
            # Extract the address operand
            if pat_str.startswith("8B 0D"):
                gobjects_addr = idc.get_wide_dword(addr + 2)
            else:
                gobjects_addr = idc.get_wide_dword(addr + 1)
            
            print(f"  [+] Potential GObjects at 0x{gobjects_addr:08X} (found via pattern at 0x{addr:08X})")
            findings["gobjects"] = hex(gobjects_addr)
            findings["patterns_validated"].append({
                "name": "GObjects",
                "pattern": pat_str,
                "found_at": hex(addr),
                "resolved_address": hex(gobjects_addr)
            })
            return


def find_gnames_pattern():
    """
    Search for GNames access pattern:
    mov ecx, [GNames]       ; 8B 0D xx xx xx xx  
    cmp dword [ecx+reg*4],0 ; 83 3C 81 00
    """
    print("[*] Searching for GNames access pattern...")
    
    patterns = [
        "8B 0D ?? ?? ?? ?? 83 3C 81 00",
        "8B 0D ?? ?? ?? ?? 8B 34 81",  # mov ecx,[GNames]; mov esi,[ecx+eax*4]
        "A1 ?? ?? ?? ?? 8B 0C 88",
    ]
    
    text_seg = idaapi.get_segm_by_name(".text")
    if not text_seg:
        return
    
    for pat_str in patterns:
        addr = ida_search.find_binary(text_seg.start_ea, text_seg.end_ea,
                                       pat_str, 16, ida_search.SEARCH_DOWN)
        if addr != idaapi.BADADDR:
            if pat_str.startswith("8B 0D"):
                gnames_addr = idc.get_wide_dword(addr + 2)
            else:
                gnames_addr = idc.get_wide_dword(addr + 1)
            
            print(f"  [+] Potential GNames at 0x{gnames_addr:08X} (found at 0x{addr:08X})")
            findings["gnames"] = hex(gnames_addr)
            findings["patterns_validated"].append({
                "name": "GNames",
                "pattern": pat_str,
                "found_at": hex(addr),
                "resolved_address": hex(gnames_addr)
            })
            return


def find_processevent():
    """
    Find UObject::ProcessEvent by searching for its typical signature.
    In UE2.5, ProcessEvent is a virtual function that dispatches script events.
    """
    print("[*] Searching for ProcessEvent...")
    
    # ProcessEvent typically has a distinctive prologue and references
    # to the script stack frame. Look for functions that reference
    # both "ScriptWarning" and "Accessed None" strings.
    
    for s in idautils.Strings():
        if "Accessed None" in str(s):
            print(f"  [+] Found 'Accessed None' string at {hex(s.ea)}")
            # Find xrefs to this string - they're likely in ProcessEvent or 
            # the script execution engine
            for xref in idautils.XrefsTo(s.ea):
                func = idaapi.get_func(xref.frm)
                if func:
                    print(f"      Referenced from function at {hex(func.start_ea)}")
                    findings["functions"]["possible_script_engine"] = hex(func.start_ea)
            break


def save_findings():
    """Save all findings to JSON"""
    os.makedirs(os.path.dirname(OUTPUT_FILE), exist_ok=True)
    
    with open(OUTPUT_FILE, 'w') as f:
        json.dump(findings, f, indent=2)
    
    print(f"\n[*] Findings saved to: {OUTPUT_FILE}")


def main():
    print("=" * 60)
    print(" BS1SDK - IDA Object Dumper")
    print(" Target: BioShock Remastered")
    print("=" * 60)
    print()
    
    find_rtti_descriptors()
    find_gobjects_pattern()
    find_gnames_pattern()
    find_processevent()
    save_findings()
    
    print("\n[*] Analysis complete!")
    print(f"    GObjects: {findings['gobjects'] or 'NOT FOUND'}")
    print(f"    GNames:   {findings['gnames'] or 'NOT FOUND'}")
    print(f"    RTTI classes: {len(findings['rtti_classes'])}")


if __name__ == "__main__":
    main()
