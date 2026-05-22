"""Dump BSP serialization functions from UT2004 Engine.dll using dumpbin."""
import struct
import subprocess
import re

dll_path = r'Z:\UnrealTournament\UnrealTournament2004\System\Engine.dll'
data = open(dll_path, 'rb').read()

# Key function file offsets (from export scan):
functions = {
    'FBspNode_serialize':  (0x001C0A40, 0x200, '??6@YAAEAVFArchive@@AEAV0@AEAVFBspNode@@@Z'),
    'FBspSurf_serialize':  (0x001C0C50, 0x400, '??6@YAAEAVFArchive@@AEAV0@AEAVFBspSurf@@@Z'),
    'FBspVertex_serialize':(0x000A2D40, 0x100, '??6@YAAEAVFArchive@@AEAV0@AEAUFBspVertex@@@Z'),
    'PointRegion':         (0x002CF400, 0x200, '?PointRegion@UModel@@...'),
    'NumVerts_func':       (0x0017A340, 0x300, 'Function with NumVertices>=3 assertion'),
}

# PE sections for RVA mapping
pe_sig = struct.unpack_from('<I', data, 0x3C)[0]
num_sections = struct.unpack_from('<H', data, pe_sig + 6)[0]
opt_hdr_size = struct.unpack_from('<H', data, pe_sig + 20)[0]
section_start = pe_sig + 24 + opt_hdr_size
sections = []
for i in range(num_sections):
    off = section_start + i * 40
    name = data[off:off+8].rstrip(b'\x00').decode('ascii', errors='replace')
    vaddr = struct.unpack_from('<I', data, off+12)[0]
    rawoff = struct.unpack_from('<I', data, off+20)[0]
    rawsize = struct.unpack_from('<I', data, off+16)[0]
    sections.append((name, vaddr, rawoff, rawsize))

def file_to_rva(foff):
    for name, va, rawoff, rawsize in sections:
        if rawoff <= foff < rawoff + rawsize:
            return va + (foff - rawoff)
    return 0

print("=== Raw hex dump of BSP serialization functions ===\n")
print("These are x64 functions. Key x64 patterns to look for:")
print("  48 89 xx xx  = MOV [reg+offset], reg  (stores to struct fields)")
print("  48 8B xx xx  = MOV reg, [reg+offset]   (loads from struct fields)")
print("  8B xx xx     = MOV r32, [reg+offset]   (32-bit field access)")
print("  0F B6 xx xx  = MOVZX r32, BYTE [reg+offset] (byte field like NumVertices)")
print("  88 xx xx     = MOV BYTE [reg+offset], r8 (byte store)")
print("  E8 xx xx xx xx = CALL (Ar << field)")
print()

for name, (foff, size, desc) in functions.items():
    rva = file_to_rva(foff)
    print(f"\n{'='*60}")
    print(f"{name} at RVA 0x{rva:08X} (file 0x{foff:08X})")
    print(f"  {desc}")
    print(f"{'='*60}")
    
    # Find function end (look for CC CC padding or RET)
    actual_end = foff + size
    for i in range(foff, min(foff + size, len(data) - 1)):
        if data[i] == 0xCC and data[i+1] == 0xCC:
            actual_end = i
            break
    
    func_size = actual_end - foff
    print(f"  Estimated size: {func_size} bytes")
    
    # Dump with annotations
    for off in range(0, func_size, 16):
        addr = foff + off
        r = file_to_rva(addr)
        raw = data[addr:min(addr+16, actual_end)]
        hexstr = ' '.join(f'{b:02X}' for b in raw)
        
        # Annotate interesting patterns
        annotations = []
        for j in range(len(raw)):
            b = raw[j]
            if j+1 < len(raw):
                # Look for struct offsets in ModRM/SIB displacement bytes
                if raw[j] in (0x8B, 0x89, 0x0F) and j+2 < len(raw):
                    pass  # Complex to decode without full disassembler
        
        print(f"  0x{r:08X}: {hexstr:<48s}")

    # Also show which byte values appear (potential field offsets)
    func_bytes = data[foff:actual_end]
    print(f"\n  Unique byte values that could be struct offsets:")
    # Look for common offset patterns in x64: [reg+disp8] where disp8 is the offset
    offsets_found = set()
    for i in range(len(func_bytes) - 2):
        # MOV patterns: 8B 4x/5x/6x/7x disp8 or 89 4x/5x/6x/7x disp8
        # Also: 48 8B 4x/5x disp8 (REX.W + MOV)
        if func_bytes[i] in (0x48, 0x49, 0x4C) and func_bytes[i+1] in (0x8B, 0x89, 0x8D, 0x03, 0x2B):
            modrm = func_bytes[i+2]
            mod = (modrm >> 6) & 3
            if mod == 1 and i+3 < len(func_bytes):  # [reg+disp8]
                disp = func_bytes[i+3]
                if 0x04 <= disp <= 0x60:
                    offsets_found.add(disp)
            elif mod == 2 and i+6 < len(func_bytes):  # [reg+disp32]
                disp = struct.unpack_from('<i', func_bytes, i+3)[0]
                if 0 < disp < 0x200:
                    offsets_found.add(disp)
        # 32-bit MOV without REX
        elif func_bytes[i] in (0x8B, 0x89) and i+1 < len(func_bytes):
            modrm = func_bytes[i+1]
            mod = (modrm >> 6) & 3
            if mod == 1 and i+2 < len(func_bytes):
                disp = func_bytes[i+2]
                if 0x04 <= disp <= 0x60:
                    offsets_found.add(disp)
        # MOVZX (0F B6) - byte access, important for NumVertices
        elif func_bytes[i] == 0x0F and i+1 < len(func_bytes) and func_bytes[i+1] == 0xB6:
            if i+2 < len(func_bytes):
                modrm = func_bytes[i+2]
                mod = (modrm >> 6) & 3
                if mod == 1 and i+3 < len(func_bytes):
                    disp = func_bytes[i+3]
                    if 0x04 <= disp <= 0x60:
                        offsets_found.add(disp)
                        print(f"  ** MOVZX (byte read) at offset +0x{disp:02X} **")
    
    if offsets_found:
        sorted_offs = sorted(offsets_found)
        print(f"  Struct offsets accessed: {', '.join(f'+0x{o:02X}' for o in sorted_offs)}")

# ─── Also dump the PointRegion function (BSP tree traversal with zone detection) ───
print(f"\n\n{'='*60}")
print("=== PointRegion — BSP tree traversal for zone detection ===")
print(f"{'='*60}")

pr_off = 0x002CF400
pr_rva = file_to_rva(pr_off)
# Find function end
pr_end = pr_off + 0x300
for i in range(pr_off, min(pr_off + 0x300, len(data) - 1)):
    if data[i] == 0xCC and data[i+1] == 0xCC:
        pr_end = i
        break

func_bytes = data[pr_off:pr_end]
print(f"RVA 0x{pr_rva:08X}, size {pr_end - pr_off} bytes")

# Extract field offsets used
offsets_found = set()
for i in range(len(func_bytes) - 3):
    if func_bytes[i] in (0x48, 0x49, 0x4C) and func_bytes[i+1] in (0x8B, 0x89, 0x8D, 0x03):
        modrm = func_bytes[i+2]
        mod = (modrm >> 6) & 3
        if mod == 1 and i+3 < len(func_bytes):
            disp = func_bytes[i+3]
            if 0x04 <= disp <= 0x60:
                offsets_found.add(disp)
    elif func_bytes[i] in (0x8B, 0x89, 0x3B, 0x39):
        modrm = func_bytes[i+1]
        mod = (modrm >> 6) & 3
        if mod == 1 and i+2 < len(func_bytes):
            disp = func_bytes[i+2]
            if 0x04 <= disp <= 0x60:
                offsets_found.add(disp)
    # MOVZX byte reads
    if func_bytes[i] == 0x0F and func_bytes[i+1] == 0xB6:
        modrm = func_bytes[i+2]
        mod = (modrm >> 6) & 3
        if mod == 1 and i+3 < len(func_bytes):
            disp = func_bytes[i+3]
            offsets_found.add(disp)
            print(f"  ** MOVZX (byte read) at offset +0x{disp:02X} **")

if offsets_found:
    sorted_offs = sorted(offsets_found)
    print(f"  Struct offsets accessed: {', '.join(f'+0x{o:02X}' for o in sorted_offs)}")

# Map to known UE2 FBspNode layout
print("\n  Expected UE2 FBspNode field mapping:")
field_map = {
    0x00: "Plane.X", 0x04: "Plane.Y", 0x08: "Plane.Z", 0x0C: "Plane.W",
    0x10: "ZoneMask (low)", 0x14: "ZoneMask (low, might not exist in x64)",
    0x18: "iVertPool", 0x1C: "iSurf",
    0x20: "iBack", 0x24: "iFront", 0x28: "iPlane",
    0x2C: "iCollisionBound", 0x30: "iRenderBound",
    0x34: "iZone[0]+iZone[1]+NumVertices+NodeFlags (packed bytes)",
    0x36: "NumVertices (byte)", 0x37: "NodeFlags (byte)",
    0x38: "iLeaf[0]", 0x3C: "iLeaf[1]",
}
for o in sorted(offsets_found):
    if o in field_map:
        print(f"    +0x{o:02X} = {field_map[o]}")
    else:
        print(f"    +0x{o:02X} = (unknown)")
