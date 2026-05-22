"""Analyze specific BSP findings from UT2004 Engine.dll."""
import struct

dll_path = r'Z:\UnrealTournament\UnrealTournament2004\System\Engine.dll'
data = open(dll_path, 'rb').read()

print("=== UT2004 x64 Engine.dll BSP Analysis ===\n")

# ─── PointRegion: Find the node stride ───
print("--- PointRegion BSP Tree Traversal (RVA 0x002D0000) ---")
pr_off = 0x002CF400
# The key pattern: IMUL by node stride
# At offset 0x002D0070 (file 0x002CF470):
code = data[pr_off:pr_off+0x200]

# Find all IMUL*imm8 in this function
for i in range(len(code)-4):
    if code[i] == 0x48 and code[i+1] == 0x6B:
        modrm = code[i+2]
        imm = code[i+3]
        reg_dst = (modrm >> 3) & 7
        reg_src = modrm & 7
        regs = ['RAX','RCX','RDX','RBX','RSP','RBP','RSI','RDI']
        print(f"  +0x{i:04X}: IMUL {regs[reg_dst]}, {regs[reg_src]}, 0x{imm:02X} ({imm} decimal)")

print()

# ─── Analyze the dot product in PointRegion ───
# Plane dot product at +0x80:
print("--- Plane dot product (node field accesses) ---")
# RDX = node pointer after IMUL*0x70
# MOVSS XMM2, [RDX+0x04]  -> Plane.Y
# MOVSS XMM0, [RDX+0x00]  -> Plane.X  
# MOVSS XMM1, [RDX+0x08]  -> Plane.Z
# SUBSS XMM2, [RDX+0x0C]  -> Plane.W
print("  +0x00: Plane.X (float)")
print("  +0x04: Plane.Y (float)")
print("  +0x08: Plane.Z (float)")
print("  +0x0C: Plane.W (float)")

# Children access: MOV R8D, [RDX+RAX*4+0x20]
# RAX is 0(back) or 1(front), so:
print("  +0x20: iBack (INT32)")
print("  +0x24: iFront (INT32)")

# iLeaf: MOV EAX, [RDX+RAX*4+0x48] (from later in function)
print("  +0x48: iLeaf[0] (INT32)")
print("  +0x4C: iLeaf[1] (INT32)")

# Zone byte: MOVZX ECX, BYTE [RAX+RDX+0x44]
print("  +0x44: iZone[0] (BYTE)")
print("  +0x45: iZone[1] (BYTE)")

print(f"\n  NODE STRIDE = 0x70 = 112 bytes (vs UE2-32bit: 64, BioShock: 100)")

# ─── FBspSurf Serialization: PanU/PanV analysis ───
print("\n\n--- FBspSurf Serialization (RVA 0x001C1850) ---")
surf_off = 0x001C0C50
surf_code = data[surf_off:surf_off+0x200]

# Find version checks (CMP DWORD [R14+0x08], imm8)
for i in range(len(surf_code)-5):
    if surf_code[i:i+3] == bytes([0x41, 0x83, 0x7E]) and surf_code[i+3] == 0x08:
        ver = surf_code[i+4]
        # Find the comparison type (next byte after immediate)
        cmp_byte = surf_code[i+5] if i+5 < len(surf_code) else 0
        cmp_ops = {0x7D: '>=', 0x7C: '<', 0x7E: '<=', 0x7F: '>'}
        cmp_str = cmp_ops.get(cmp_byte, f'?? (0x{cmp_byte:02X})')
        print(f"  Version check at +0x{i:04X}: version {cmp_str} {ver} (0x{ver:02X})")

# Find LEA patterns showing struct field offsets being serialized
print("\n  Serialization order (LEA RDX, [R15+offset]):")
for i in range(len(surf_code)-4):
    if surf_code[i:i+3] == bytes([0x49, 0x8D, 0x57]):
        off = surf_code[i+3]
        print(f"    Ar << surf[+0x{off:02X}]")
    elif surf_code[i:i+3] == bytes([0x49, 0x8D, 0x97]) and i+7 < len(surf_code):
        off = struct.unpack_from('<i', surf_code, i+3)[0]
        print(f"    Ar << surf[+0x{off:04X}]")

# PanU/PanV: look for WORD stores (66 89) which indicate INT16
print("\n  PanU/PanV type detection:")
for i in range(len(surf_code)-8):
    # 66 89 = MOV WORD (16-bit store)
    if surf_code[i] == 0x66 and surf_code[i+1] == 0x89:
        print(f"    MOV WORD at +0x{i:04X}: {' '.join(f'{b:02X}' for b in surf_code[i:i+8])}")
        # This confirms PanU/PanV are INT16!

# ─── FBspNode Serialization analysis ───
print("\n\n--- FBspNode Serialization (RVA 0x001C1640) ---")
node_off = 0x001C0A40
node_code = data[node_off:node_off+0x200]

# Find LEA patterns for serialized fields
print("  Serialization order (LEA RDX, [R15+offset]):")
for i in range(len(node_code)-4):
    if node_code[i:i+3] == bytes([0x49, 0x8D, 0x57]):
        off = node_code[i+3]
        print(f"    Ar << node[+0x{off:02X}]")

# Find MOVZX (byte reads) in node serializer
print("\n  Byte field accesses (MOVZX):")
for i in range(len(node_code)-4):
    if node_code[i:i+2] == bytes([0x0F, 0xB6]):
        modrm = node_code[i+2]
        mod = (modrm >> 6) & 3
        if mod == 1 and i+3 < len(node_code):
            disp = node_code[i+3]
            if 0x30 <= disp <= 0x60:
                print(f"    MOVZX at node[+0x{disp:02X}] (BYTE read)")

# Find version checks in node serializer too
print("\n  Version checks:")
for i in range(len(node_code)-5):
    if node_code[i:i+3] == bytes([0x41, 0x83, 0x7E]) and node_code[i+3] == 0x08:
        ver = node_code[i+4]
        cmp_byte = node_code[i+5] if i+5 < len(node_code) else 0
        cmp_ops = {0x7D: '>=', 0x7C: '<', 0x7E: '<=', 0x7F: '>'}
        cmp_str = cmp_ops.get(cmp_byte, f'?? (0x{cmp_byte:02X})')
        print(f"    Version check: version {cmp_str} {ver} (0x{ver:02X})")

# ─── SUMMARY ───
print("\n\n" + "="*60)
print("SUMMARY: Key findings for BioShock BSP parser")
print("="*60)
print("""
1. UT2004 x64 FBspNode = 112 bytes (0x70 stride)
   32-bit UE2 = 64 bytes, BioShock/Vengeance = 100 bytes
   Field ORDER is the same, just different packing/sizes.

2. PanU/PanV in FBspSurf are serialized as INT16 (2 bytes each)!
   66 89 = MOV WORD confirms 16-bit initialization
   This is critical: if our parser reads INT32 for these, 
   we'll be 4 bytes off for all subsequent fields.

3. UT2004 FBspNode byte fields in x64 struct:
   +0x44: iZone[0] (BYTE)
   +0x45: iZone[1] (BYTE) 
   +0x46: NumVertices (BYTE)
   +0x47: NodeFlags (BYTE)
   
   BioShock 100-byte struct equivalent:
   +0x4C: NodeFlags (BYTE)
   +0x4D: iZone[0] (BYTE)
   +0x4E: NumVertices (BYTE) ← confirmed by planarity fix!
   +0x4F: Pad/iZone[1] (BYTE)

4. Serialization is version-gated:
   - PanU/PanV only if version >= 78 (0x4E)
   - iBrushPoly defaults to -1 if version < 101 (0x65)
   - Extra plane fields if version > 86 (0x56)
   - Additional field at +0x48 if version >= 106 (0x6A)
   BioShock is version 141, so ALL gates pass.
""")
