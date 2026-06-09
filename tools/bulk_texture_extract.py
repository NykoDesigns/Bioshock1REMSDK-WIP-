#!/usr/bin/env python3
"""
Bulk Texture Extractor
Extracts ALL textures from BioShock's .blk bulk content files using Catalog.bdc.
These are textures that umodel can't export (CachedBulkDataSize = -1 / HasBeenStripped=1).

Writes DDS files (with proper headers) that the level editor can load.
Also writes TGA for the largest mip (for compatibility with existing TextureCache).

Usage: python bulk_texture_extract.py [--output DIR]
"""
import os, sys, json, struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "re_tool", "config.json")

with open(CONFIG_PATH) as f:
    cfg = json.load(f)

GAME_DIR = cfg["game_content_dir"]  # D:\...\ContentBaked\pc
BULK_DIR = os.path.join(GAME_DIR, "BulkContent")
BDC_PATH = os.path.join(BULK_DIR, "Catalog.bdc")
EXPORT_DIR = cfg["umodel_export_dir"]  # Z:\UEViewer\export

# ── CompactIndex reader (BioShock format) ──────────────────────────────────
def read_ci(data, pos):
    """Read a CompactIndex from data at pos. Returns (value, bytes_read)."""
    b0 = data[pos]
    sign = (b0 & 0x80) != 0
    val = b0 & 0x3F
    br = 1
    if b0 & 0x40:
        b1 = data[pos + br]; br += 1
        val |= (b1 & 0x7F) << 6
        if b1 & 0x80:
            b2 = data[pos + br]; br += 1
            val |= (b2 & 0x7F) << 13
            if b2 & 0x80:
                b3 = data[pos + br]; br += 1
                val |= (b3 & 0x7F) << 20
                if b3 & 0x80:
                    b4 = data[pos + br]; br += 1
                    val |= (b4 & 0x1F) << 27
    return (-val if sign else val), br

def read_fstring(data, pos):
    """Read BioShock FString (CI length + UTF-16LE chars). Returns (string, bytes_read)."""
    raw_len, br = read_ci(data, pos)
    total_br = br
    if raw_len <= 0 or raw_len > 65536:
        return "", total_br
    byte_size = raw_len * 2
    result = ""
    for i in range(raw_len):
        wc = struct.unpack_from('<H', data, pos + total_br + i * 2)[0]
        if wc == 0:
            break
        result += chr(wc & 0xFF)
    total_br += byte_size
    return result, total_br

# ── Parse Catalog.bdc ─────────────────────────────────────────────────────
def parse_catalog(bdc_path):
    """Parse Catalog.bdc and return list of CatalogEntry dicts."""
    with open(bdc_path, 'rb') as f:
        data = f.read()
    
    pos = 0
    endian = data[pos]; pos += 1
    pos += 8  # int64 f4
    pos += 4  # int fC
    
    num_files, br = read_ci(data, pos); pos += br
    if num_files <= 0 or num_files > 10000:
        print(f"[Catalog] Bad file count: {num_files}")
        return []
    
    entries = []
    for fi in range(num_files):
        if pos >= len(data): break
        pos += 8  # int64 f0
        blk_filename, br = read_fstring(data, pos); pos += br
        num_items, br = read_ci(data, pos); pos += br
        if num_items < 0 or num_items > 100000:
            break
        
        for ii in range(num_items):
            if pos >= len(data): break
            obj_name, br = read_fstring(data, pos); pos += br
            pkg_name, br = read_fstring(data, pos); pos += br
            if pos + 20 > len(data): break
            pos += 4  # f10
            data_offset = struct.unpack_from('<i', data, pos)[0]; pos += 4
            data_size = struct.unpack_from('<i', data, pos)[0]; pos += 4
            pos += 4  # DataSize2
            pos += 4  # f20
            
            if obj_name and data_size > 0:
                entries.append({
                    'objectName': obj_name,
                    'packageName': pkg_name,
                    'blkFilename': blk_filename,
                    'dataOffset': data_offset,
                    'dataSize': data_size,
                })
    
    return entries

# ── DDS header writer ──────────────────────────────────────────────────────
# DXT1: 8 bytes per 4x4 block = 0.5 bytes/pixel
# DXT5: 16 bytes per 4x4 block = 1 byte/pixel
# RGBA8: 4 bytes/pixel

DDS_MAGIC = 0x20534444  # "DDS "
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000

DDPF_FOURCC = 0x4
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400000
DDSCAPS_COMPLEX = 0x8

def make_dds_header(width, height, mip_count, fourcc):
    """Create a 128-byte DDS header."""
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE
    if mip_count > 1:
        flags |= DDSD_MIPMAPCOUNT
    
    if fourcc == b'DXT1':
        block_size = 8
    elif fourcc in (b'DXT3', b'DXT5'):
        block_size = 16
    else:
        block_size = 16  # default to DXT5
    
    pitch = max(1, (width + 3) // 4) * block_size
    
    caps = DDSCAPS_TEXTURE
    if mip_count > 1:
        caps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
    
    header = struct.pack('<I', DDS_MAGIC)  # magic
    header += struct.pack('<I', 124)  # header size
    header += struct.pack('<I', flags)
    header += struct.pack('<I', height)
    header += struct.pack('<I', width)
    header += struct.pack('<I', pitch)  # pitchOrLinearSize
    header += struct.pack('<I', 0)  # depth
    header += struct.pack('<I', mip_count)
    header += b'\x00' * 44  # reserved[11]
    # Pixel format (32 bytes)
    header += struct.pack('<I', 32)  # pfSize
    header += struct.pack('<I', DDPF_FOURCC)  # pfFlags
    header += fourcc  # fourCC
    header += struct.pack('<I', 0)  # rgbBitCount
    header += struct.pack('<I', 0)  # rMask
    header += struct.pack('<I', 0)  # gMask
    header += struct.pack('<I', 0)  # bMask
    header += struct.pack('<I', 0)  # aMask
    # Caps
    header += struct.pack('<I', caps)
    header += struct.pack('<I', 0)  # caps2
    header += struct.pack('<I', 0)  # caps3
    header += struct.pack('<I', 0)  # caps4
    header += struct.pack('<I', 0)  # reserved2
    
    return header

def guess_dimensions(data_size, fmt='DXT1'):
    """Given total bulk data size (all mips), guess the base resolution and mip count."""
    if fmt == 'DXT1':
        bpp = 0.5  # bytes per pixel
    else:
        bpp = 1.0
    
    # Try common resolutions, with varying mip counts
    for base in [2048, 1024, 512, 256, 128, 64]:
        for max_mips in range(10, 0, -1):
            total = 0
            w, h = base, base
            for m in range(max_mips):
                mw = max(1, w >> m)
                mh = max(1, h >> m)
                blocks_x = max(1, (mw + 3) // 4)
                blocks_y = max(1, (mh + 3) // 4)
                if fmt == 'DXT1':
                    total += blocks_x * blocks_y * 8
                else:
                    total += blocks_x * blocks_y * 16
                if mw == 1 and mh == 1:
                    break
            if total == data_size:
                actual_mips = min(max_mips, m + 1) if 'm' in dir() else max_mips
                return base, base, max_mips
    
    # Try non-square
    for w in [2048, 1024, 512, 256, 128, 64]:
        for h in [2048, 1024, 512, 256, 128, 64]:
            if w == h:
                continue
            total = 0
            for m in range(10):
                mw = max(1, w >> m)
                mh = max(1, h >> m)
                blocks_x = max(1, (mw + 3) // 4)
                blocks_y = max(1, (mh + 3) // 4)
                if fmt == 'DXT1':
                    total += blocks_x * blocks_y * 8
                else:
                    total += blocks_x * blocks_y * 16
                if mw == 1 and mh == 1:
                    break
                if total == data_size:
                    return w, h, m + 1
                if total > data_size:
                    break
    
    return None, None, None

def compute_mip_size(w, h, fmt='DXT1'):
    """Size of a single mip level."""
    blocks_x = max(1, (w + 3) // 4)
    blocks_y = max(1, (h + 3) // 4)
    return blocks_x * blocks_y * (8 if fmt == 'DXT1' else 16)

# ── TGA writer (from raw BGRA) ────────────────────────────────────────────
def decode_dxt1_block(data, offset):
    """Decode a single DXT1 4x4 block to 16 RGBA pixels."""
    c0 = struct.unpack_from('<H', data, offset)[0]
    c1 = struct.unpack_from('<H', data, offset + 2)[0]
    
    def rgb565_to_rgba(c):
        r = ((c >> 11) & 0x1F) * 255 // 31
        g = ((c >> 5) & 0x3F) * 255 // 63
        b = (c & 0x1F) * 255 // 31
        return (r, g, b, 255)
    
    color0 = rgb565_to_rgba(c0)
    color1 = rgb565_to_rgba(c1)
    
    if c0 > c1:
        color2 = tuple((2 * color0[i] + color1[i]) // 3 for i in range(3)) + (255,)
        color3 = tuple((color0[i] + 2 * color1[i]) // 3 for i in range(3)) + (255,)
    else:
        color2 = tuple((color0[i] + color1[i]) // 2 for i in range(3)) + (255,)
        color3 = (0, 0, 0, 0)
    
    palette = [color0, color1, color2, color3]
    lookup = struct.unpack_from('<I', data, offset + 4)[0]
    
    pixels = []
    for i in range(16):
        idx = (lookup >> (i * 2)) & 0x3
        pixels.append(palette[idx])
    return pixels

def dxt1_to_rgba(data, width, height):
    """Decode DXT1 data to RGBA byte array."""
    pixels = bytearray(width * height * 4)
    blocks_x = max(1, (width + 3) // 4)
    blocks_y = max(1, (height + 3) // 4)
    offset = 0
    
    for by in range(blocks_y):
        for bx in range(blocks_x):
            block_pixels = decode_dxt1_block(data, offset)
            offset += 8
            for py in range(4):
                for px in range(4):
                    x = bx * 4 + px
                    y = by * 4 + py
                    if x < width and y < height:
                        pi = (y * width + x) * 4
                        r, g, b, a = block_pixels[py * 4 + px]
                        pixels[pi] = r
                        pixels[pi + 1] = g
                        pixels[pi + 2] = b
                        pixels[pi + 3] = a
    return pixels

def write_tga(path, width, height, rgba_data):
    """Write an uncompressed 32-bit TGA."""
    header = bytearray(18)
    header[2] = 2  # uncompressed RGB
    header[12] = width & 0xFF
    header[13] = (width >> 8) & 0xFF
    header[14] = height & 0xFF
    header[15] = (height >> 8) & 0xFF
    header[16] = 32  # bits per pixel
    header[17] = 0x28  # top-left origin + 8 alpha bits
    
    # Convert RGBA to BGRA for TGA
    bgra = bytearray(len(rgba_data))
    for i in range(0, len(rgba_data), 4):
        bgra[i] = rgba_data[i + 2]      # B
        bgra[i + 1] = rgba_data[i + 1]  # G
        bgra[i + 2] = rgba_data[i]      # R
        bgra[i + 3] = rgba_data[i + 3]  # A
    
    with open(path, 'wb') as f:
        f.write(header)
        f.write(bgra)

# ── Main ──────────────────────────────────────────────────────────────────
def main():
    output_dir = EXPORT_DIR
    if len(sys.argv) >= 3 and sys.argv[1] == '--output':
        output_dir = sys.argv[2]
    
    print(f"[BulkExtract] Parsing {BDC_PATH}")
    entries = parse_catalog(BDC_PATH)
    print(f"[BulkExtract] {len(entries)} catalog entries")
    
    # Collect all texture names that are missing from umodel exports
    missing_textures = set()
    missing_file = os.path.join(SCRIPT_DIR, "level_editor", "mesh_tex_missing.txt")
    if os.path.exists(missing_file):
        with open(missing_file, 'r') as f:
            for line in f:
                parts = line.strip().split('\t')
                if len(parts) >= 3 and parts[0] == 'NO-TGA':
                    # Extract texture name from tex='name'
                    tex_part = parts[2]
                    if tex_part.startswith("tex='") and tex_part.endswith("'"):
                        tex_name = tex_part[5:-1]
                        if tex_name and tex_name != 'invisible':
                            missing_textures.add(tex_name)
    
    print(f"[BulkExtract] {len(missing_textures)} missing textures to search for")
    
    # Build lookup: objectName → entry (catalog entries are like "Texture_49", not the texture name)
    # We need to match by package name to find textures for specific maps
    # Strategy: extract ALL texture entries and save as DDS, then also decode mip0 to TGA
    
    # Group entries by packageName to understand the data
    by_package = {}
    for e in entries:
        pkg = e['packageName']
        if pkg not in by_package:
            by_package[pkg] = []
        by_package[pkg].append(e)
    
    # Count texture vs non-texture entries
    texture_entries = [e for e in entries if 'Texture' in e['objectName'] or 'texture' in e['objectName']]
    other_entries = [e for e in entries if e not in texture_entries]
    print(f"[BulkExtract] Texture entries: {len(texture_entries)}, other: {len(other_entries)}")
    
    # Extract all texture entries and write DDS files
    extracted = 0
    failed = 0
    skipped = 0
    
    # Create output directory structure
    bulk_out = os.path.join(output_dir, "_BulkTextures")
    os.makedirs(bulk_out, exist_ok=True)
    
    for entry in texture_entries:
        blk_path = os.path.join(BULK_DIR, entry['blkFilename'])
        if not os.path.exists(blk_path):
            failed += 1
            continue
        
        # Read raw data
        try:
            with open(blk_path, 'rb') as f:
                f.seek(entry['dataOffset'])
                raw = f.read(entry['dataSize'])
            if len(raw) != entry['dataSize']:
                failed += 1
                continue
        except:
            failed += 1
            continue
        
        # Try to guess dimensions (try DXT1 first since lightmaps use it, then DXT5)
        w, h, mips = guess_dimensions(entry['dataSize'], 'DXT1')
        fmt = 'DXT1'
        if w is None:
            w, h, mips = guess_dimensions(entry['dataSize'], 'DXT5')
            fmt = 'DXT5'
        
        if w is None:
            # Can't determine dimensions — skip
            skipped += 1
            continue
        
        # Write DDS file
        fourcc = b'DXT1' if fmt == 'DXT1' else b'DXT5'
        dds_header = make_dds_header(w, h, mips, fourcc)
        
        safe_name = entry['objectName'].replace('/', '_').replace('\\', '_')
        pkg_safe = entry['packageName'].replace('/', '_').replace('\\', '_')
        out_dir = os.path.join(bulk_out, pkg_safe)
        os.makedirs(out_dir, exist_ok=True)
        
        dds_path = os.path.join(out_dir, f"{safe_name}.dds")
        with open(dds_path, 'wb') as f:
            f.write(dds_header)
            f.write(raw)
        
        # Also write TGA of mip0 (for TextureCache compatibility)
        if fmt == 'DXT1':
            mip0_size = compute_mip_size(w, h, 'DXT1')
            if len(raw) >= mip0_size:
                try:
                    rgba = dxt1_to_rgba(raw[:mip0_size], w, h)
                    tga_path = os.path.join(out_dir, f"{safe_name}.tga")
                    write_tga(tga_path, w, h, rgba)
                except:
                    pass  # DDS is still valid
        
        extracted += 1
    
    print(f"\n[BulkExtract] Results:")
    print(f"  Extracted: {extracted} textures as DDS")
    print(f"  Failed: {failed}")
    print(f"  Skipped (unknown dimensions): {skipped}")
    print(f"  Output: {bulk_out}")
    
    # Report which of the missing textures might now be available
    # The catalog entries use generic names like "Texture_49", not the actual texture names
    # We need the BSM's name table to map Texture_49 → actual_name
    # For now, report the total extraction count
    print(f"\nNote: Catalog entries use generic names (e.g. 'Texture_49').")
    print(f"To map these to actual texture names, the BSM's export table must be cross-referenced.")
    print(f"The BSM Export Cross-Reference tool (Tool 5) will handle this mapping.")

if __name__ == "__main__":
    main()
