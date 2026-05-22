"""Deep scan of UT2004 Engine.dll for BSP struct access patterns."""
import struct

dll_path = r'Z:\UnrealTournament\UnrealTournament2004\System\Engine.dll'
data = open(dll_path, 'rb').read()

text_off = 0x400
text_size = 0x58D000
rdata_off = 0x58D400
rdata_size = 0x215400

# PE info for RVA calculation
pe_sig = struct.unpack_from('<I', data, 0x3C)[0]
num_sections = struct.unpack_from('<H', data, pe_sig + 6)[0]
opt_hdr_size = struct.unpack_from('<H', data, pe_sig + 20)[0]
section_start = pe_sig + 24 + opt_hdr_size

sections = []
for i in range(num_sections):
    off = section_start + i * 40
    name = data[off:off+8].rstrip(b'\x00').decode('ascii', errors='replace')
    vaddr = struct.unpack_from('<I', data, off+12)[0]
    rawsize = struct.unpack_from('<I', data, off+16)[0]
    rawoff = struct.unpack_from('<I', data, off+20)[0]
    sections.append((name, vaddr, rawoff, rawsize))

def rva_to_file(rva):
    for name, va, rawoff, rawsize in sections:
        if va <= rva < va + rawsize:
            return rawoff + (rva - va)
    return None

def file_to_rva(foff):
    for name, va, rawoff, rawsize in sections:
        if rawoff <= foff < rawoff + rawsize:
            return va + (foff - rawoff)
    return None

# ─── 1. Find xrefs to "NumVertices >= 3" string ───
print("=== Finding code that references 'NumVertices >= 3' ===")
str_offset = 0x005DC1C8  # "NumVertices >= 3"
str_rva = file_to_rva(str_offset)
print(f"String at file:0x{str_offset:08X}, RVA:0x{str_rva:08X}")

# In x64, LEA instructions reference strings via RIP-relative addressing:
# LEA reg, [RIP + disp32]
# The displacement is relative to the instruction AFTER the LEA
# LEA encoding: 48 8D 0D xx xx xx xx (LEA RCX, [RIP+disp]) - for first arg
# Or: 48 8D 15 xx xx xx xx (LEA RDX, [RIP+disp]) - for second arg
# Or: 4C 8D 05 xx xx xx xx (LEA R8, [RIP+disp]) - for third arg

xrefs = []
for i in range(text_off, text_off + text_size - 7):
    # Check for LEA with RIP-relative
    if data[i] in (0x48, 0x4C) and data[i+1] == 0x8D:
        modrm = data[i+2]
        if (modrm & 0xC7) == 0x05:  # ModRM = xx 000 101 = [RIP+disp32]
            disp = struct.unpack_from('<i', data, i+3)[0]
            # RIP points to next instruction (7 bytes for REX+8D+ModRM+disp32)
            target_rva = file_to_rva(i) + 7 + disp
            if target_rva == str_rva:
                func_rva = file_to_rva(i)
                xrefs.append(i)
                print(f"  XREF at file:0x{i:08X} RVA:0x{func_rva:08X}")

# For each xref, dump surrounding code bytes
for xref in xrefs[:3]:
    # Back up to find function start (look for CC CC CC pattern)
    func_start = xref
    for j in range(xref - 1, max(xref - 500, text_off), -1):
        if data[j] == 0xCC and data[j-1] == 0xCC:
            func_start = j + 1
            break
    
    func_rva = file_to_rva(func_start)
    print(f"\n  Function near RVA 0x{func_rva:08X}:")
    # Dump 200 bytes of code
    for off in range(0, min(300, xref - func_start + 100), 16):
        addr = func_start + off
        rva = file_to_rva(addr)
        hexbytes = ' '.join(f'{data[addr+b]:02X}' for b in range(min(16, len(data)-addr)))
        marker = " <-- NumVertices assertion" if func_start + off <= xref < func_start + off + 16 else ""
        print(f"    0x{rva:08X}: {hexbytes}{marker}")

# ─── 2. Find the FBspNode serialization function ───
print("\n\n=== Finding FBspNode serialization (operator<<) ===")
# Look for mangled name of operator<<(FArchive&, FBspNode&)
node_ser = data.find(b'FBspNode@@@Z')
if node_ser != -1:
    # Back up to find the full mangled name
    start = node_ser
    while start > 0 and data[start-1] != 0:
        start -= 1
    mangled = data[start:node_ser+12].decode('ascii', errors='replace')
    print(f"Mangled name at 0x{start:08X}: {mangled}")
    
    # Find xrefs to this name string (it's in the export/import table or .rdata)
    name_rva = file_to_rva(start)
    print(f"Name RVA: 0x{name_rva:08X}" if name_rva else "Name RVA: not in mapped section")

# ─── 3. Find operator<<(FArchive&, FBspSurf&) ───
print("\n=== Finding FBspSurf serialization ===")
surf_ser = data.find(b'FBspSurf@@@Z')
if surf_ser != -1:
    start = surf_ser
    while start > 0 and data[start-1] != 0:
        start -= 1
    mangled = data[start:surf_ser+12].decode('ascii', errors='replace')
    print(f"Mangled name at 0x{start:08X}: {mangled}")

# ─── 4. Find export table entries for these functions ───
print("\n=== Checking PE Export Table ===")
export_dir_rva = struct.unpack_from('<I', data, pe_sig + 24 + 112)[0]
export_dir_size = struct.unpack_from('<I', data, pe_sig + 24 + 116)[0]
if export_dir_rva > 0:
    export_off = rva_to_file(export_dir_rva)
    if export_off:
        num_funcs = struct.unpack_from('<I', data, export_off + 20)[0]
        num_names = struct.unpack_from('<I', data, export_off + 24)[0]
        addr_table_rva = struct.unpack_from('<I', data, export_off + 28)[0]
        name_table_rva = struct.unpack_from('<I', data, export_off + 32)[0]
        print(f"Exports: {num_funcs} functions, {num_names} named")
        
        # Search for BSP-related exports
        name_table_off = rva_to_file(name_table_rva)
        addr_table_off = rva_to_file(addr_table_rva)
        
        bsp_exports = []
        for i in range(min(num_names, 10000)):
            name_ptr_rva = struct.unpack_from('<I', data, name_table_off + i*4)[0]
            name_off = rva_to_file(name_ptr_rva)
            if name_off is None:
                continue
            name = b''
            while data[name_off] != 0 and len(name) < 200:
                name += bytes([data[name_off]])
                name_off += 1
            name_str = name.decode('ascii', errors='replace')
            
            if any(kw in name_str for kw in ['BspNode', 'BspSurf', 'FBspVert', 'UModel', 'NumVert']):
                func_rva = struct.unpack_from('<I', data, addr_table_off + i*4)[0]
                func_off = rva_to_file(func_rva)
                bsp_exports.append((name_str, func_rva, func_off))
                
        print(f"BSP-related exports: {len(bsp_exports)}")
        for name, rva, foff in bsp_exports:
            print(f"  RVA 0x{rva:08X} (file 0x{foff:08X}): {name}")

# ─── 5. Scan for the FBspNode struct size in serialization ───
print("\n=== Looking for node struct size constants ===")
# In serialization, we'd see: Ar << node.Plane; Ar << node.ZoneMask; etc.
# Or bulk serialization: Ar.Serialize(nodeArray, count * sizeof(FBspNode))
# In bulk, we'd see IMUL by struct size (64 for UE2)

# Look for constants: 0x36 (byte offset of NumVertices in UE2 = 54)
# And 0x34 (byte offset of iZone in UE2 = 52)
# These are distinctive because they appear together
print("Searching for byte-offset patterns 0x34/0x36 near SHL*6...")

for i in range(text_off, text_off + text_size - 30):
    # Pattern: access at +0x36 (NumVertices) near SHL by 6 or IMUL by 0x40
    if data[i] == 0x48 and data[i+1] == 0xC1 and (data[i+2] & 0xF8) == 0xE0 and data[i+3] == 0x06:
        # SHL RAX/RCX/etc, 6
        window = data[i:i+60]
        # Check for 0x36 byte access (movzx/mov byte at +0x36)
        has_36 = False
        has_34 = False
        for j in range(len(window)-1):
            if window[j] == 0x36:
                has_36 = True
            if window[j] == 0x34:
                has_34 = True
        if has_36 and has_34:
            rva = file_to_rva(i)
            hexstr = ' '.join(f'{b:02X}' for b in data[i:i+40])
            print(f"  RVA 0x{rva:08X}: {hexstr}")
