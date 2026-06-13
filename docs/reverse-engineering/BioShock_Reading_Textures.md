# Reading BioShock 1 Textures — Format Specification

A guide for decoding texture pixel data out of BioShock 1
(the "Vengeance" engine — Irrational's fork of Unreal Engine 2.5).

This describes the on-disk layout only. Everything here has been validated by reading the
retail PC build and round-tripping it byte-for-byte across all shipped maps and the `.U`
script packages: every texture resolves, every inline mip's byte count matches the format
math exactly, and mip 0 decodes to a coherent image. You can implement against it directly.

---

## Conventions

These are the standard UE2 primitives; BioShock keeps them unchanged unless noted.

- **`CI`** — `FCompactIndex`, UE2's variable-length signed integer. (One signed value packed
  into 1–5 bytes; the first byte's high bits carry sign + continuation.)
- **`FString`** — length-prefixed string. The length is a `CI`; a negative length means the
  payload is UTF-16, positive means ANSI. Includes the trailing NUL in the count.
- **`FName`** — an interned name, stored on the wire as two `CI`s: `{ index, number }`.
  `index` points into the package's name table; `number` is an instance suffix (see the
  caveat in §4.2).
- **objref** — an object reference, a single `CI` indexing the package's export/import table.
- **ObjHeader** — BioShock's per-object "version tag," a small struct Irrational prepends to
  most of its custom-serialized data: `INT32 check` (always `4`) + `INT32 version`. Treat a
  `check != 4` as a hard parse error — it means you are misaligned.

All integers are little-endian (PC build).

---

## 1. The big picture

A BioShock texture lives in **two** places at once:

1. **The package** (a `.U` script package, or a map's content package) holds the **`UTexture`
   object**: its tagged properties — including the pixel `Format` — followed by a **mip
   table**. Each mip in that table either carries its pixel bytes **inline**, or is marked
   **externalized** (`Num == 0`), meaning the pixels were moved out of the package.

2. **The `.blk` bulk files** hold the externalized pixel blocks. A companion index,
   **`Catalog.bdc`**, maps `(package, object)` → `(.blk file, offset, size)` so you can find
   a given texture's pixels.

So reading a texture is: parse the `UTexture` to get its format and mip dimensions, then for
each mip either take the inline bytes or go fetch them from the catalog + `.blk`.

> **BSP lightmaps are not a special texture format.** They are ordinary DXT `UTexture`s that
> happen to live in a `LightMaps_BSP` group, with their pixels externalized into the level's
> `.blk` just like any other texture. Decode them exactly as below. (How a BSP *surface*
> selects its lightmap atlas and computes UVs is a separate `UModel` topic, out of scope here.)

---

## 2. The `UTexture` object

A `UTexture` serializes as a **standard UE2 object** (its object header + tagged property
block — nothing custom in between), and then a texture-specific tail. Parse the tagged
properties as you would any UE2 object; the one you need from them is **`Format`** (§3).

Immediately after the tagged properties, read the tail:

```
ObjHeader                       check == 4, subVer (== 1 on retail)
QWORD  CachedBulkDataSize       ONLY present if subVer != 0
CI     MipCount                 number of mip levels (0..64)
Mip[MipCount]                   each as below
```

Each **Mip**:

```
ObjHeader                       check == 4, ver (== 2 on retail)
--- inline pixel array (a UE2 "lazy array" of bytes) ---
INT  SkipOffset                 absolute file offset of the byte just past this array
INT  BulkA                      bulk descriptor (see notes)
INT  BulkB                      bulk descriptor (see notes)
CI   Num                        inline byte count; 0 => pixels are externalized
BYTE Data[Num]                  the inline pixel bytes (present only when Num > 0)
--- mip dimensions ---
INT  USize                      mip width  in texels
INT  VSize                      mip height in texels
BYTE UBits                      log2(USize)
BYTE VBits                      log2(VSize)
```

Notes:

- **`SkipOffset`** is the engine's lazy-load skip pointer: an absolute offset to the end of
  the byte array, so a lazy reader can jump over the inline blob without parsing it. When you
  are only *reading*, you can ignore its value (just consume the 4 bytes). If you ever rewrite
  a package you must back-patch it to the true end position.
- **`BulkA` / `BulkB`** are BioShock additions to the lazy-array header. They are the bulk
  descriptor fields the engine uses internally to decide inline-vs-external; for a reader they
  are opaque — consume them.
- **An externalized mip (`Num == 0`)** therefore serializes to exactly **31 bytes**: the
  8-byte header + three INTs + a 1-byte `CI` zero + two size INTs + two `BYTE`s, with no pixel
  payload. The pixels are in the `.blk` (§4).
- **`CachedBulkDataSize`** is a *hint* of the source asset's total bulk size. It is `>=` the
  sum of the externalized mip sizes, and equal only when no source mips were dropped during
  cooking (true for a few textures, not most). **Do not** use it to size your external read —
  use the catalog record and the format math.
- There are **no leftover/unknown bytes** in the `UTexture` tail. If you have trailing bytes
  after the last mip, you are misaligned upstream (most likely in the tagged-property parse).

---

## 3. Pixel formats

The format is the tagged property **`Format`** — a single byte (a `ByteProperty` holding an
enum ordinal). The enum is `ETextureFormat`:

| Ord | Format   | Bytes for a `w × h` mip                         |
| --- | -------- | ----------------------------------------------- |
| 0   | P8       | `w*h`            (8-bit palettized)             |
| 1   | *(unused)* | —                                             |
| 2   | R5G6B5   | `w*h*2`                                          |
| 3   | DXT1     | `ceil(w/4) * ceil(h/4) * 8`                      |
| 4   | *(unused)* | —                                             |
| 5   | RGBA8    | `w*h*4`                                          |
| 6   | NODATA   | — (no pixel data)                               |
| 7   | DXT3     | `ceil(w/4) * ceil(h/4) * 16`                     |
| 8   | DXT5     | `ceil(w/4) * ceil(h/4) * 16`                     |
| 9   | A8       | `w*h`                                            |
| 10  | A16      | `w*h*2`                                          |
| 11  | ABGR16F  | `w*h*8`            (16-bit float RGBA)           |
| 12  | 3DC      | `ceil(w/4) * ceil(h/4) * 16`  (BC5 / ATI2, normal maps) |

This byte-count formula — call it `blockSize(Format, w, h)` — predicts every mip's length
**exactly**. Use it both to validate inline mips (`Num == blockSize(Format, USize, VSize)`)
and to know how many bytes an externalized mip occupies inside its `.blk` block.

Decoding the pixels themselves is standard: DXT1/3/5 are the usual S3TC/BC1/2/3 block codecs;
**3DC** is BC5/ATI2 (two BC4 alpha blocks giving R and G; reconstruct B as a unit normal's Z).

> The DXT block layout is unchanged from desktop D3D; mip 0 of any externalized DXT texture
> decompresses straight to a correct RGBA image with no swizzle. (Some BSP luminance textures
> are stored with their channels swizzled and the shader unswizzles with a `.yzx` — but that
> is a content convention of the lightmap pass, not a change to the texture format.)

---

## 4. External pixels: the `.blk` files and `Catalog.bdc`

Most texture pixel data is moved out of packages into flat **`.blk`** files — one per level,
plus a shared `DynamicBulkFileTextures.blk`. A `.blk` has **no internal structure of its
own**: it is just **32 KB-aligned raw payload blocks concatenated back to back**. The only
index is `Catalog.bdc`.

### 4.1 `Catalog.bdc` layout

```
BulkCatalog:
    u8       Endian            0 = PC / little-endian
    i64      Cooker            cooker hash / timestamp (bookkeeping)
    i32      Flags
    CI       FileCount
    File[FileCount]:
        i64      Unknown0      cooker bookkeeping
        FString  Filename      the .blk filename this record indexes
        CI       ItemCount
        Item[ItemCount]:
            FString  ObjectName    the texture object's name
            FString  PackageName   its package name
            i32      Flags
            i32      DataOffset    byte offset into the .blk
            i32      DataSize      payload byte count
            i32      DataSize2     == DataSize on PC (second / uncompressed size)
            i32      Unknown20
```

### 4.2 Resolving a mip to bytes

When a mip has `Num == 0`, its pixels come from the catalog:

1. **Key = `(package name, object name)`** — the texture's outermost package name and the
   texture object's own name. The engine matches on the raw `FName` integers at runtime, but
   for a tool, matching the rendered strings works as long as you render the `FName` suffix
   correctly:

   > **`FName` number caveat.** The text form of an `FName` appends `number - 1` with **no
   > separator**: `number == 0` is the bare name, `number == 6` renders as `...5`. This
   > off-by-one is the classic source of "texture not found" bugs when matching a string-keyed
   > catalog. `Catalog.bdc` stores the rendered strings, so render your key the same way.

2. **The lookup is scoped to one `.blk` at a time**, tried in this order:
   1. **`<levelName>Level.blk`** — the current level's blk. (Map base names match blk names
      exactly; localized `_int` maps share the base level's blk.)
   2. **`DynamicBulkFileTextures.blk`** — the shared dynamic pool.
   3. **`<name>.blk`** — a name-derived fallback.

   Because the "package" half of the key can be either the cooked sub-package export *or* the
   map root, a robust tool should try **both** the topmost export package name and the map
   name as the package half of the key.

3. **Read `DataSize` bytes** straight from `[DataOffset, DataOffset + DataSize)` in the
   resolved `.blk`. The engine does **no size check** here — it trusts the catalog record.

### 4.3 Splitting a block into mips

One catalog record covers **all** the externalized mips of one texture, concatenated
**largest mip first**, with no padding between them. So:

```
DataSize == sum of blockSize(Format, USize, VSize) over every mip whose Num == 0
```

Walk the texture's externalized mips in declared order (they are stored largest-first), and
carve `blockSize(Format, USize, VSize)` bytes for each off the front of the block. That gives
you each mip's pixels; decode per §3.

(The `DataSize == sum-of-blockSizes` equality is a strong validation that your format model is
correct — it is not something the engine itself enforces, but it holds across the entire
corpus.)

---

## 5. Read algorithm, end to end

To decode texture *T* in package *P*:

1. Parse *T* as a standard UE2 object; from its tagged properties take **`Format`**.
2. Read the `UTexture` tail (§2): `ObjHeader`, optional `CachedBulkDataSize`, `MipCount`, and
   the mip array.
3. For each mip:
   - If `Num > 0`: the `Data[Num]` bytes are the pixels. (Sanity:
     `Num == blockSize(Format, USize, VSize)`.)
   - If `Num == 0`: it is externalized — defer to step 4.
4. If any mip was externalized, look up `(P, T)` in `Catalog.bdc` (§4.2), read the block, and
   carve it into the externalized mips largest-first (§4.3).
5. Decompress each mip's bytes per its `Format` (§3).

---

## 6. Summary

- A texture = a standard UE2 object (tagged properties, including `Format`) + a mip table.
- Each mip is either **inline** (`Num > 0`, pixels follow) or **externalized** (`Num == 0`,
  31-byte record, pixels live in a `.blk`).
- Externalized pixels sit in 32 KB-aligned blocks inside `.blk` files, indexed by
  `Catalog.bdc` keyed on `(package, object)` names — mind the `FName` `number - 1` rendering.
- `blockSize(Format, w, h)` gives every mip's exact byte length; mips share one catalog block,
  largest-first, no padding.
- BSP lightmaps are just DXT textures decoded the same way.
</content>
</invoke>
