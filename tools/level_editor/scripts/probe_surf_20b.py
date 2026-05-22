"""Probe the 20-byte block after Actor CI in BioShock FBspSurf to find PanU/PanV."""
import struct

bsm_path = r'D:\SteamLibrary\steamapps\common\BioShock Remastered\ContentBaked\pc\Maps\1-Medical.bsm'
data = open(bsm_path, 'rb').read()

# We need to parse the BSM to find the UModel export and navigate to the surfs.
# Instead, let's use the known file structure. From previous runs:
# The BSP parser already reads surfs correctly (3386 surfs, 189646 bytes).
# We need to find the surf array in the raw data.

# Quick approach: parse just enough of the BSM to find the surf data.
# The BSM has a header, then exports. UModel export serial data contains:
# Vectors, Points, Nodes, Surfs.

# From the editor output, we know the total surf section is 189646 bytes for 3386 surfs.
# Average ~56 bytes/surf. Let's find the first surf by searching for the Vengeance header pattern.

# Each surf starts with 8B Vengeance header: INT32(4) + INT32(sub_ver)
# Then CI Material, 24B fixed, CI Actor, 20B remaining

# Let me search for the pattern: 04 00 00 00 01 00 00 00 (check=4, sub_ver=1)
# This is the Vengeance per-element header
pattern = b'\x04\x00\x00\x00\x01\x00\x00\x00'

# Find all occurrences of this pattern
hits = []
start = 0
while True:
    idx = data.find(pattern, start)
    if idx == -1 or len(hits) > 100000:
        break
    hits.append(idx)
    start = idx + 1

print(f"Found {len(hits)} Vengeance element headers in the file")
print(f"File size: {len(data)} bytes")

# The surfs start after vectors, points, and nodes in the UModel serial data.
# From editor output: 590 vectors, 11652 points, 7125 nodes (100B each)
# Node array is 7125 * 100 = 712500 bytes
# The surfs should be somewhere after that.

# Let's try a different approach: manually parse surfs from a known position.
# Actually, let me find consecutive Vengeance headers that are ~56B apart.

def read_compact_index(data, pos):
    """Read UE2 compact index (variable length 1-5 bytes)."""
    if pos >= len(data):
        return 0, 0
    b0 = data[pos]
    neg = (b0 & 0x80) != 0
    more = (b0 & 0x40) != 0
    val = b0 & 0x3F
    shift = 6
    pos += 1
    while more and pos < len(data) and shift < 32:
        b = data[pos]
        more = (b & 0x80) != 0
        val |= (b & 0x7F) << shift
        shift += 7
        pos += 1
    if neg:
        val = -val
    return val, pos

# Find clusters of Vengeance headers spaced ~50-60 bytes apart
print("\nSearching for surf array start (clusters of headers ~50-60B apart)...")
best_run = 0
best_start = 0
for i in range(len(hits) - 10):
    # Check if next 10 headers are roughly evenly spaced
    diffs = [hits[i+j+1] - hits[i+j] for j in range(10)]
    avg = sum(diffs) / len(diffs)
    if 45 <= avg <= 65 and max(diffs) - min(diffs) < 20:
        if 10 > best_run:
            best_run = 10
            best_start = i
            print(f"  Candidate at hit[{i}] = offset 0x{hits[i]:X}, avg spacing={avg:.1f}, range={min(diffs)}-{max(diffs)}")

# Use the first good candidate
if best_start > 0 or best_run > 0:
    surf_array_file_offset = hits[best_start]
else:
    print("No clear surf array found by spacing heuristic.")
    # Fallback: try parsing from around the 3.5M mark (typical for Medical)
    surf_array_file_offset = 0

# Now let's manually parse a few surfs and dump the 20B blocks
print(f"\n=== Parsing surfs starting at file offset 0x{surf_array_file_offset:X} ===")

# Actually, let me just try multiple starting positions and look for ones that parse cleanly.
# We know: 8B header + CI_mat + 24B + CI_actor + 20B = ~56B

def parse_surfs_and_dump_20b(data, start_pos, max_surfs=20):
    """Parse surfs from start_pos and dump the 20B remaining block."""
    pos = start_pos
    end = len(data)
    
    # First, read the compact index count (TArray header)
    # Actually, the TArray has a CI count before the elements
    
    results = []
    for i in range(max_surfs):
        # Check for Vengeance header
        if pos + 8 > end:
            break
        check = struct.unpack_from('<I', data, pos)[0]
        sub_ver = struct.unpack_from('<I', data, pos+4)[0]
        if check != 4:
            break
        pos += 8
        
        # CI Material
        mat_val, new_pos = read_compact_index(data, pos)
        ci_mat_len = new_pos - pos
        pos = new_pos
        
        # 24B fixed
        if pos + 24 > end:
            break
        poly_flags = struct.unpack_from('<I', data, pos)[0]
        pBase = struct.unpack_from('<i', data, pos+4)[0]
        vNormal = struct.unpack_from('<i', data, pos+8)[0]
        vTextureU = struct.unpack_from('<i', data, pos+12)[0]
        vTextureV = struct.unpack_from('<i', data, pos+16)[0]
        iBrushPoly = struct.unpack_from('<i', data, pos+20)[0]
        pos += 24
        
        # CI Actor
        actor_val, new_pos = read_compact_index(data, pos)
        ci_act_len = new_pos - pos
        pos = new_pos
        
        # 20B remaining
        if pos + 20 > end:
            break
        block_20b = data[pos:pos+20]
        
        results.append({
            'idx': i,
            'sub_ver': sub_ver,
            'mat': mat_val,
            'ci_mat_len': ci_mat_len,
            'poly_flags': poly_flags,
            'pBase': pBase,
            'vNormal': vNormal,
            'vTextureU': vTextureU,
            'vTextureV': vTextureV,
            'iBrushPoly': iBrushPoly,
            'actor': actor_val,
            'ci_act_len': ci_act_len,
            'block_20b': block_20b,
        })
        pos += 20
    
    return results

# Find the surf array by looking at known positions from the editor output
# The editor said "Parsed 3386 surfs (189646 bytes)"
# Let's search for the CI count = 3386 (0xD3A) in compact index encoding
# CI encoding of 3386: need to figure it out
# 3386 = 0xD3A
# Byte 0: 0x3A & 0x3F = 58, with continue bit: 0x3A | 0x40 = 0x7A
# Byte 1: 0xD3A >> 6 = 53 = 0x35. No continue: 0x35
# Wait, let me re-do:
# CI encoding: val = 3386 (positive, so no sign bit)
# Byte 0: val & 0x3F = 3386 & 63 = 3386 % 64 = 58 (0x3A). More? 3386 > 63, yes. So b0 = 58 | 0x40 = 0x7A
# Remaining: 3386 >> 6 = 52 (0x34). More? 52 > 127? No. So b1 = 0x34
# So CI(3386) = [0x7A, 0x34]
ci_3386 = bytes([0x7A, 0x34])

# Search for this CI followed by the Vengeance header pattern
search = ci_3386 + pattern  # CI count + first element header
positions = []
start = 0
while True:
    idx = data.find(search, start)
    if idx == -1 or len(positions) > 20:
        break
    positions.append(idx)
    start = idx + 1

print(f"\nSearching for CI(3386) + Vengeance header: found at {len(positions)} positions")
for p in positions:
    print(f"  File offset 0x{p:X}")

if positions:
    # Parse from the first match (skip the CI count bytes)
    surf_start = positions[0] + 2  # skip CI count
    surfs = parse_surfs_and_dump_20b(data, surf_start, 15)
    
    print(f"\nParsed {len(surfs)} surfs. 20B block analysis:")
    print(f"{'Surf':>4} {'Mat':>5} {'pBase':>5} {'vTexU':>5} {'vTexV':>5} {'Actor':>6}  20B hex dump")
    print("-" * 100)
    
    for s in surfs:
        b = s['block_20b']
        hex_str = ' '.join(f'{x:02X}' for x in b)
        print(f"{s['idx']:4d} {s['mat']:5d} {s['pBase']:5d} {s['vTextureU']:5d} {s['vTextureV']:5d} {s['actor']:6d}  {hex_str}")
    
    # Now analyze each position in the 20B block
    print(f"\n=== Analyzing each byte/word/dword position in the 20B block ===")
    
    # Try reading as INT16 at each offset
    print("\nINT16 interpretation at each offset:")
    for off in range(0, 19, 2):
        values = [struct.unpack_from('<h', s['block_20b'], off)[0] for s in surfs]
        zeros = sum(1 for v in values if v == 0)
        small = sum(1 for v in values if 0 < abs(v) <= 256)
        print(f"  Offset +{off:2d}: values={values[:8]}...  zeros={zeros}, small(<=256)={small}")
    
    # Try reading as INT32 at each offset
    print("\nINT32 interpretation at each offset:")
    for off in range(0, 17, 4):
        values = [struct.unpack_from('<i', s['block_20b'], off)[0] for s in surfs]
        zeros = sum(1 for v in values if v == 0)
        small = sum(1 for v in values if 0 < abs(v) <= 256)
        print(f"  Offset +{off:2d}: values={values[:8]}...  zeros={zeros}, small(<=256)={small}")
    
    # Also check: maybe PanU/PanV moved further back. In Vengeance, they might come
    # after some extra fields that UE2 doesn't have.
    print("\nBYTE interpretation at each offset (looking for zone indices 0-127):")
    for off in range(20):
        values = [s['block_20b'][off] for s in surfs]
        in_zone_range = sum(1 for v in values if v <= 127)
        print(f"  Offset +{off:2d}: values={values[:15]}  (<=127: {in_zone_range}/{len(surfs)})")
