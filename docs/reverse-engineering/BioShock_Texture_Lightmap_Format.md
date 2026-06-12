# BioShock 1 — Texture & BSP Lightmap Format

Reverse-engineering notes on how BioShock 1 (the "Vengeance" engine, Irrational's fork of
Unreal Engine 2.5) stores texture pixel data and BSP static-lighting ("luminance") maps.

Everything here has been validated by round-tripping the retail PC build byte-for-byte across all
16 shipped maps plus the `.U` script packages. Conventions used below:

- **CI** = `FCompactIndex`, UE2's variable-length signed int.
- **FString** = the length-prefixed string (the sign of the length flags ANSI vs UTF-16, as in
  stock UE2).
- **objref** = a `CI` object reference into the package's export/import table.
- **ObjHeader** = BioShock's per-object "Vengeance tag": `INT32 check (==4)` + `INT32 version`.
  Irrational put these in front of most custom-serialized structs as a versioning guard.

Version gates below refer to the archive's licensee/engine version; on the retail PC build every
gate noted as "always" fires.

---

## 1. Big picture

A BioShock texture is split across **two** places:

1. **The package (`.U` / map `.bsm`)** holds the `UTexture` object: its tagged properties
   (including the pixel `Format`) and a **mip table**. Each mip either carries its pixels
   **inline**, or has `Num == 0` meaning the pixels were **externalized**.
2. **The `.blk` bulk files** hold the externalized pixel blocks. `Catalog.bdc` is the index that
   maps `(package, object)` → `(.blk file, offset, size)`.

BSP lightmaps are **not a special format** — they are ordinary DXT `UTexture`s living in a
`LightMaps_BSP` group inside the level package, with their mips externalized into the level's
`.blk`. What's special is the **descriptor data in `UModel`** that says which atlas a surface
lands in and how to compute its lightmap UVs. That's covered in §5–§6.

---

## 2. `UTexture` wire format

The super-serializer is just the standard `UObject` serialize (object header + tagged properties —
nothing custom in between). After the tagged properties, the texture-specific payload is:

```
ObjHeader                 check==4, subVer (==1 retail)
QWORD  CachedBulkDataSize                                      [only if subVer != 0]
CI     MipCount
Mip[MipCount]:
    ObjHeader             check==4, ver (==2 retail)
    TLazyArray<BYTE> Data (the mip pixels):
        INT  SkipOffset   absolute end-of-array file offset (preserve verbatim)
        INT  BulkA        Bio lazy/bulk descriptor
        INT  BulkB        Bio lazy/bulk descriptor
        CI   Num          inline byte count; 0 => pixels live in the .blk
        BYTE Data[Num]    inline pixel bytes (DXT/G8/…) when present
    INT  USize            mip width  (texels)
    INT  VSize            mip height (texels)
    BYTE UBits            log2(USize)
    BYTE VBits            log2(VSize)
```

Notes:

- **`SkipOffset`** is the stock UE2 `TLazyArray` skip pointer — an absolute file offset to the byte
  just past the array. On save it must be back-patched to the real position. Preserve it verbatim
  for round-tripping; the engine uses it to skip the inline blob during a lazy load.
- **`BulkA` / `BulkB`** are BioShock additions to the lazy-array header. They are the lazy/bulk
  descriptor fields the engine uses to decide inline vs. external.
- An **externalized mip** (`Num == 0`) serializes to exactly **31 bytes** — the header + the two
  size ints + the two bits bytes, with no pixel payload.
- **`CachedBulkDataSize`** is a *hint* of the source asset's full bulk size. It is `>=` the sum of
  externalized mip sizes, and equal only when no source mips were dropped during cooking (true on a
  few maps, **not** most). Do **not** use it to size the external read.

The `UTexture` body has no unknown/leftover bytes — it's fully accounted for.

---

## 3. Pixel formats

The pixel format is the tagged property **`Format`** (a `ByteProperty`, one byte = enum ordinal).
The enum is `BitmapMaterial.ETextureFormat`:

| Ord | Format    | Bytes per block / texel |
| --- | --------- | --- |
| 0   | P8        | `w*h`        (palettized) |
| 1   | *(dep.)*  | — |
| 2   | R5G6B5    | `w*h*2` |
| 3   | DXT1      | `ceil(w/4)*ceil(h/4)*8` |
| 4   | *(dep.)*  | — |
| 5   | RGBA8     | `w*h*4` |
| 6   | NODATA    | — |
| 7   | DXT3      | `ceil(w/4)*ceil(h/4)*16` |
| 8   | DXT5      | `ceil(w/4)*ceil(h/4)*16` |
| 9   | A8        | `w*h` |
| 10  | A16       | `w*h*2` |
| 11  | ABGR16F   | `w*h*8` |
| 12  | **3DC**   | `ceil(w/4)*ceil(h/4)*16`  (BC5/ATI2, normal maps) |

**`blockSize(format, USize, VSize)` predicts each mip's byte count exactly** — verified on every
inline mip across the corpus.

> **BSP luminance maps are DXT.** The luminance map is optimised for DXT compression: the shader
> stores the lightmap RGB swizzled into a DXT texture and unswizzles it in the pixel shader with a
> `.yzx` swizzle.

---

## 4. The `.blk` bulk files and `Catalog.bdc`

Most texture pixel data is externalized out of the packages into flat **`.blk`** files (one per
level, plus a shared `DynamicBulkFileTextures.blk`). The `.blk` files have **no internal structure
of their own** — they are just **32 KB-aligned raw payload blocks** concatenated back to back.
`Catalog.bdc` is the sole index.

### 4.1 `Catalog.bdc` layout

```
BulkCatalog:
    u8      Endian            (0 = PC / little-endian)
    i64     Cooker            (cooker hash / timestamp)
    i32     Flags
    CI      FileCount
    BulkCatalogFile[FileCount]:
        i64     Unknown0      (cooker bookkeeping)
        FString Filename      (the .blk filename)
        CI      ItemCount
        BulkCatalogItem[ItemCount]:
            FString ObjectName
            FString PackageName
            i32     Flags
            i32     DataOffset    (byte offset into the .blk)
            i32     DataSize      (payload byte count)
            i32     DataSize2     (== DataSize on PC; second/uncompressed size)
            i32     Unknown20
```

Re-emitting a parsed `Catalog.bdc` is byte-for-byte identical to retail, which is the proof the
layout is complete.

### 4.2 How a mip resolves to bytes

When a mip has `Num == 0`, the engine looks the pixels up in the catalog:

1. **Key = `(GetOutermost()->Name, Object->Name)`**, each an `FName {index, number}`. The engine
   matches the **raw 4-int key** (both names' index + number) — it does not compare strings at
   runtime.
   - **FName number caveat:** the text form appends `number - 1` with **no separator**
     (`number 0` = bare name, `number 6` → `"...5"`). This off-by-one is a real bug source when
     matching string-keyed catalogs. The catalog stores `ToString` strings that are interned back
     through the FName constructor, so a string-based tool must render the suffix the same way.

2. **Lookup is scoped to one `.blk` per attempt**, tried in this order:
   1. **`<levelName>Level.blk`** — the current level's blk (the engine concatenates
      `name + "Level.blk"`; map basenames match blk names exactly; `_int` localized maps share the
      base level blk).
   2. **`DynamicBulkFileTextures.blk`** — the shared dynamic pool.
   3. **`<name>.blk`** — a name-derived fallback.

   Statically, `GetOutermost` is ambiguous (it can be the cooked sub-package export *or* the map
   root), so a tool should try both the topmost export package and the map name as the package half
   of the key.

3. **Read `DataSize` bytes verbatim** from `[DataOffset, DataOffset + DataSize)` in the resolved
   `.blk`. The engine does **no size check** — it trusts the catalog record.

Externalized mips for one texture are concatenated **largest-first** in their block, no padding, so
`catalog.DataSize == Sum(blockSize over all Num==0 mips)`. (That equality is a useful *validation*
of the format model, not something the engine itself enforces.)

Across all 16 maps every externalized texture resolves with zero size mismatches and zero
unresolved entries; mip-0 DXT/3DC decodes to coherent images.

---

## 5. BSP lightmaps — the `UModel` descriptors

The lightmap **pixels** are just DXT textures (§2–§4). What ties a BSP surface to its lightmap is a
chain of descriptors inside `UModel`. The relevant arrays:

| `UModel` member | element | role |
| --- | --- | --- |
| `LightMap` | `FLightMapIndex` | one **per surface** (`count == numSurfs`) |
| `LightMapTextures` | `FLightMapTexture` | the **luminance atlas pool** (8–14 atlases/level, 1024 px wide) |

And the per-node link:

- **`FBspNode.iLightMap`** → index into `LightMap[]`. Proven across maps: `LightMap[node.iLightMap]
  .iSurf == node.iSurf`, and `LightMap` is **not** stored in surface order (so the index is real,
  not implicit).

### 5.1 `FLightMapIndex` — the per-surface descriptor

In-memory stride 112 bytes. Serialized as `ObjHeader(ver=2)` then:

| field | type | notes |
| ----- | ---- | --- |
| `iSurf` | INT | back-ref to owning surface |
| `SizeX` | INT | lightmap texel width  (range 7..512); UV scale = `SizeX/1024` |
| `SizeY` | INT | lightmap texel height; UV scale = `SizeY/1024` |
| `WorldToLightMap` | FMatrix (64 B) | world pos → lightmap UV (see §6) |
| `Pan` / `UVBias` | 3 floats | not read by the UV build |
| `Lights` | `TArray<FLightMapLight>` | the baked-light entries below |

### 5.2 `FLightMapLight` — the baked-light entry (carries the atlas selection)

In-memory stride 28 bytes. Serialized as `ObjHeader(ver=1)` + 3×objref + 3×INT. Proven across
multiple maps:

| field | type | notes |
| ----- | ---- | --- |
| `Lights[0]` | objref → Light actor | up to 3 physical lights baked into this entry |
| `Lights[1]` | objref (or 0) | |
| `Lights[2]` | objref (or 0) | |
| **`iAtlas`** | INT | index into `LightMapTextures[]` — **selects the atlas texture**. Always in `[0, numAtlases)`. |
| `TileX` | INT | atlas pixel X of this surface's tile (0..1016) |
| `TileY` | INT | atlas pixel Y of this surface's tile (0..1016) |

### 5.3 `FLightMapTexture` — an atlas pool entry

Just `ObjHeader` + one **objref to a DXT `UTexture`** in the `LightMaps_BSP` group. That texture's
pixels resolve through the §4 `.blk` pipeline like any other. `iAtlas` from §5.2 indexes this array
to pick the atlas.

---

## 6. The lightmap UV math (render side)

The Vengeance renderer is a DX9/SM3 rewrite (stock UE2 was fixed-function). At load, the engine
packs the BSP surfaces many-to-one into the 9–14 `LightMaps_BSP` atlases and builds a per-vertex
lightmap UV stream. Per vertex, per baked light:

```
(U', V') = WorldToLightMap (FMatrix from FLightMapIndex) × vertexWorldPos
U = U' * (SizeX / 1024) + (TileX + 0.5) / 1024
V = V' * (SizeY / 1024) + (TileY + 0.5) / 1024
```

- `vertexWorldPos` is the surface vertex's world position.
- `1024` = atlas width (`1/1024 = 0.0009765625`).
- `SizeX/SizeY` from `FLightMapIndex`; `TileX/TileY` from `FLightMapLight`.
- Atlas texture = `LightMapTextures[ FLightMapLight.iAtlas ]`.

This packs the surface's `SizeX×SizeY` lightmap into its tile at `(TileX, TileY)` in the chosen
atlas, and stores the result into the render vertex buffer's luminance UV stream (read by the shader
as the luminance-map UV).

### 6.1 The draw

The lighting pass binds, per draw:

| slot | source |
| --- | --- |
| `s_luminanceMap1` | `LightMapTextures[ light0.iAtlas ]` |
| `s_luminanceMap2` | `LightMapTextures[ light1.iAtlas ]` (composite layers) |
| `luminanceMapUVPacking` | the per-vertex UV stream scale/offset built above |

The pixel shader samples those atlases and **unswizzles the DXT-packed luminance with `.yzx`**.

---

## 7. Summary

- Textures = standard `UObject` tagged properties + a mip table; each mip's pixels are inline or
  externalized (`Num == 0`).
- Externalized pixels live in 32 KB-aligned blocks inside `.blk` files, indexed by `Catalog.bdc`
  keyed on `(package, object)` FNames. Mind the FName `number - 1` text convention.
- Pixel byte counts follow `blockSize(Format, w, h)` exactly.
- BSP lightmaps are ordinary DXT textures; the `UModel` descriptors (`FLightMapIndex` →
  `FLightMapLight.iAtlas/TileX/TileY` → `FLightMapTexture`) select the atlas and the tile, and the
  `WorldToLightMap` matrix plus the `Size/1024` and `(Tile+0.5)/1024` packing give the final
  atlas-space UVs. The shader unswizzles luminance with `.yzx`.
