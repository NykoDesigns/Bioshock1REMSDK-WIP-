"""Scan UT2004 Engine.dll for BSP-related strings and patterns."""
import struct
import sys

dll_path = r'Z:\UnrealTournament\UnrealTournament2004\System\Engine.dll'
data = open(dll_path, 'rb').read()
print(f"Engine.dll size: {len(data)} bytes")

# PE header info
pe_sig = struct.unpack_from('<I', data, 0x3C)[0]
machine = struct.unpack_from('<H', data, pe_sig + 4)[0]
print(f"Machine: 0x{machine:04X} ({'x64' if machine == 0x8664 else 'x86' if machine == 0x14C else 'unknown'})")

num_sections = struct.unpack_from('<H', data, pe_sig + 6)[0]
opt_hdr_size = struct.unpack_from('<H', data, pe_sig + 20)[0]
section_start = pe_sig + 24 + opt_hdr_size

# Find .rdata section (where strings live)
rdata_off = rdata_size = 0
text_off = text_size = 0
for i in range(num_sections):
    off = section_start + i * 40
    name = data[off:off+8].rstrip(b'\x00').decode('ascii', errors='replace')
    rawsize = struct.unpack_from('<I', data, off+16)[0]
    rawoff = struct.unpack_from('<I', data, off+20)[0]
    if name == '.rdata':
        rdata_off = rawoff
        rdata_size = rawsize
    if name == '.text':
        text_off = rawoff
        text_size = rawsize

print(f".text: offset=0x{text_off:X} size=0x{text_size:X}")
print(f".rdata: offset=0x{rdata_off:X} size=0x{rdata_size:X}")

# ─── Search for BSP-related strings ───
print("\n=== BSP-Related Strings ===")
keywords = [
    b'BspNode', b'FBspNode', b'BSPNode',
    b'BspSurf', b'FBspSurf', 
    b'NumVertices', b'iVertPool',
    b'ZoneMask', b'TextureU', b'TextureV',
    b'PanU', b'PanV', b'NodeFlags',
    b'NumZones', b'MaxZones', b'MAX_ZONES',
    b'iCollisionBound', b'iRenderBound',
    b'PointRegion', b'iLeaf',
    b'more than', b'zones',
    b'FModelCoords', b'BuildZone',
    b'CalcSurfMinMaxUV', b'SetupZone',
    b'FilterLeaf', b'FilterNode',
]

for kw in keywords:
    positions = []
    start = 0
    while True:
        idx = data.find(kw, start)
        if idx == -1:
            break
        positions.append(idx)
        start = idx + 1
        if len(positions) > 10:
            break
    if positions:
        print(f"\n  '{kw.decode()}'  ({len(positions)} hits)")
        for p in positions[:5]:
            ctx = data[p:p+80]
            try:
                s = ctx.split(b'\x00')[0].decode('ascii', errors='replace')
            except:
                s = repr(ctx[:40])
            print(f"    0x{p:08X}: \"{s}\"")

# ─── Search for FBspSurf field name strings ───
print("\n=== Surf/UV Related Strings ===")
uv_keywords = [
    b'vTextureU', b'vTextureV', b'pBase', b'vNormal',
    b'PolyFlags', b'iBrushPoly', b'LightMap',
    b'iSurf', b'Surf',
    b'CalcUV', b'ComputeUV', b'SetupUV',
    b'UClamp', b'VClamp', b'USize', b'VSize',
    b'UScale', b'VScale',
]

for kw in uv_keywords:
    positions = []
    start = 0
    while True:
        idx = data.find(kw, start)
        if idx == -1:
            break
        positions.append(idx)
        start = idx + 1
        if len(positions) > 10:
            break
    if positions:
        print(f"\n  '{kw.decode()}'  ({len(positions)} hits)")
        for p in positions[:3]:
            ctx = data[p:p+80]
            try:
                s = ctx.split(b'\x00')[0].decode('ascii', errors='replace')
            except:
                s = repr(ctx[:40])
            print(f"    0x{p:08X}: \"{s}\"")

# ─── Also look for UE2 serialization tag strings ───
print("\n=== Serialization Strings ===")
ser_keywords = [
    b'SerializeNode', b'SerializeSurf',
    b'Serialize', b'operator<<',
    b'FArchive', b'UModel',
    b'LoadModel', b'BuildBsp',
]

for kw in ser_keywords:
    idx = data.find(kw)
    if idx != -1:
        ctx = data[idx:idx+80]
        try:
            s = ctx.split(b'\x00')[0].decode('ascii', errors='replace')
        except:
            s = repr(ctx[:40])
        print(f"  0x{idx:08X}: \"{s}\"")

# ─── Scan for the node struct size constant ───
print("\n=== Node Struct Size Detection ===")
# In x64, look for: mov reg, <size> or lea patterns
# Check for the constant 64 (0x40) near BSP-related code
# Also check 40h in .rdata as potential vtable or struct descriptor

# Look for sequences: SHL reg,6 followed by access to offsets 0x18,0x1C,0x20,0x24
# These would be: iVertPool, iSurf, iBack, iFront in standard UE2
shl6_hits = []
for i in range(text_off, text_off + text_size - 20):
    # SHL reg, 6 (C1 Ex 06)
    if data[i] == 0xC1 and (data[i+1] & 0xF8) == 0xE0 and data[i+2] == 0x06:
        # Check next ~30 bytes for 0x18, 0x1C, 0x20, 0x24 offsets
        window = data[i:i+40]
        has18 = b'\x18' in window
        has1c = b'\x1c' in window or b'\x1C' in window
        has20 = b'\x20' in window
        has24 = b'\x24' in window
        if sum([has18, has1c, has20, has24]) >= 2:
            shl6_hits.append(i)

    # REX + SHL reg, 6 (48 C1 Ex 06)
    elif data[i] in (0x48, 0x49) and data[i+1] == 0xC1 and (data[i+2] & 0xF8) == 0xE0 and data[i+3] == 0x06:
        window = data[i:i+40]
        has18 = b'\x18' in window
        has1c = b'\x1c' in window or b'\x1C' in window
        has20 = b'\x20' in window
        has24 = b'\x24' in window
        if sum([has18, has1c, has20, has24]) >= 2:
            shl6_hits.append(i)

print(f"SHL 6 near node-like offsets (0x18-0x24): {len(shl6_hits)} candidates")
for h in shl6_hits[:10]:
    rva = h - text_off + 0x1000
    ctx = ' '.join(f'{b:02X}' for b in data[h:h+20])
    print(f"  RVA 0x{rva:08X}: {ctx}")
