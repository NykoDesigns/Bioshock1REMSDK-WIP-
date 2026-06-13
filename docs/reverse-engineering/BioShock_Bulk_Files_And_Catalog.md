# BioShock 1 Bulk Files (`.blk`) & `Catalog.bdc` — Format Specification

A guide for BioShock 1's external bulk-data system (the "Vengeance"
engine — Irrational's fork of Unreal Engine 2.5).

This is the system that stores most **texture pixel data** *outside* the packages. If you are
reading textures and a mip says "my pixels aren't here" (`Num == 0`), this document is where
those pixels actually live. It is self-contained; you do not need the texture document to
understand it, though §5 shows how the two connect.

Everything here is validated by reading the retail PC build and re-emitting a parsed
`Catalog.bdc` **byte-for-byte** against the shipped file, and by resolving every externalized
texture across all shipped maps with zero size mismatches and zero unresolved entries.

---

## Conventions

Standard UE2 primitives, with one BioShock twist on strings:

- **`CI`** — `FCompactIndex`, the variable-length signed int (1–5 bytes; first byte's high
  bits carry sign + continuation).
- **`FString`** — length-prefixed string. **BioShock reverses the stock UE2 sign convention:**
  - **`len > 0`** → **UTF-16LE**, `len` code units (the count includes the NUL terminator).
  - **`len < 0`** → **ANSI**, `|len|` bytes.
  - `len == 0` → empty.

  (Stock UE2 is the opposite — negative is the Unicode case. Get this backwards and every
  string in the catalog mis-decodes, so it is worth a unit test.)
- **`FName`** — interned name, two parts `{ index, number }`. Relevant here only for the
  number→text rule in §4.2.

All integers are little-endian (PC build).

---

## 1. The big picture

BioShock's cooker pulls bulky, rarely-structural payloads — overwhelmingly **texture mip
pixels** — out of the `.U` / map packages and concatenates them into a handful of flat
**`.blk`** files. A single index file, **`Catalog.bdc`**, records *for every externalized
resource* where its bytes are: which `.blk`, at what offset, how many bytes.

So the system is exactly two kinds of file:

| File | Role |
| --- | --- |
| `*.blk` | **Raw payload.** A `.blk` has **no internal structure of its own** — it is just resource blocks concatenated back to back (block starts are 32 KB-aligned). You cannot parse a `.blk` on its own; it carries no headers, names, sizes, or table of contents. |
| `Catalog.bdc` | **The index.** The *sole* structure. It names every `.blk` and lists each resource inside it as `(object, package) → (offset, size)`. Understanding the catalog *is* understanding the `.blk` layout. |

There is normally **one `.blk` per level** (named after the map) plus a shared
**`DynamicBulkFileTextures.blk`** for dynamically/commonly loaded textures.

```
        Catalog.bdc                          SomeLevel.blk
   ┌───────────────────────┐            ┌────────────────────────┐
   │ file "SomeLevel.blk"  │            │ [block @offsetA, sizeA] │ ← texture X's mips
   │   item (X, pkg) ──────┼─ offsetA ─▶│ [block @offsetB, sizeB] │ ← texture Y's mips
   │   item (Y, pkg) ──────┼─ offsetB ─▶│ ...                     │
   │ file "Dynamic...blk"  │            └────────────────────────┘
   │   item (Z, pkg) ──────┼─▶ (into DynamicBulkFileTextures.blk)
   └───────────────────────┘
```

---

## 2. The `.blk` files

A `.blk` is a **flat concatenation of raw payload blocks**. Properties of the container:

- **No header, no footer, no index, no per-block tag.** The first byte of the file is the
  first byte of the first resource's payload.
- **One block per catalogued resource.** A block holds *all* of that resource's externalized
  bytes (for a texture, that is every externalized mip, concatenated — see §5).
- **Block starts are 32 KB-aligned.** Consecutive blocks are padded up to the next 32,768-byte
  boundary. This is an observed property of how the cooker lays them out; you do **not** compute
  it yourself when reading — the catalog's `DataOffset` already points at the exact start, and
  the engine simply seeks there.
- **The bytes are verbatim payload.** For textures that means raw DXT/3DC/etc. mip data — no
  compression, no per-block transform. (What a block *means* is defined entirely by the
  resource type that owns it; the `.blk` is type-agnostic.)

Because the file is structureless, **you can only locate anything via `Catalog.bdc`**. A block
is addressed purely by `(DataOffset, DataSize)` from its catalog item.

---

## 3. `Catalog.bdc` layout

The catalog is one small, fully-structured file. Top to bottom:

```
BulkCatalog:
    u8      Endian            0 = PC / little-endian
    i64     Cooker            cooker hash / timestamp (bookkeeping; preserve verbatim)
    i32     Flags
    CI      FileCount
    File[FileCount]:
        i64     Unknown0      cooker bookkeeping (preserve verbatim)
        FString Filename      the .blk filename this record indexes (e.g. "SomeLevel.blk")
        CI      ItemCount
        Item[ItemCount]:
            FString ObjectName     the resource object's name
            FString PackageName    the resource's package name
            i32     Flags
            i32     DataOffset     byte offset of the block within the .blk
            i32     DataSize       payload byte count
            i32     DataSize2      second size; == DataSize on PC (the uncompressed size)
            i32     Unknown20      bookkeeping
```

Notes:

- The structure is **two levels**: a list of `.blk` files, each owning a list of items. The
  item's owning `.blk` is the `Filename` of the `File` record it sits under — items do **not**
  name their own file.
- **`DataSize2 == DataSize`** on the PC build. The two-size shape exists for platforms/paths
  that compress blocks; on PC nothing is compressed, so they are equal and you read `DataSize`
  bytes directly.
- `Endian`, `Cooker`, the two `Flags`, `Unknown0`, and `Unknown20` are bookkeeping you do not
  need to *interpret* to read data, but you must **preserve them verbatim** if you ever rewrite
  the catalog (re-emitting a parsed catalog byte-for-byte is the proof the layout is complete).
- There are no string tables or back-references — `ObjectName`/`PackageName` are stored inline
  as `FString`s per item.

---

## 4. Resolving a resource to bytes

Given a resource you want the external bytes for — identified by its object name and the
package it lives in — here is the lookup the engine performs.

### 4.1 The key

The lookup key is the pair **`(PackageName, ObjectName)`**. Internally the engine compares the
raw `FName` integers, but a string-keyed tool works as long as you render names the same way the
catalog stored them (note §4.2).

A practical subtlety: the "package" half is ambiguous statically. A texture's *outermost*
package can be either the cooked sub-package export **or** the map root, depending on how it was
cooked. A robust reader should try **both** the topmost export package name and the map name as
the package half of the key.

### 4.2 The `FName` number → text rule

`FName`s carry a numeric suffix, and BioShock renders it with an **off-by-one and no
separator**:

- `number == 0` → the bare base name (`"Floor"`).
- `number == N (N>0)` → base name followed by `N - 1`, **no underscore** (`number == 6` →
  `"Floor5"`).

`Catalog.bdc` stores the already-rendered strings, so if your tool builds keys from `FName`s it
must reproduce this exact suffix rule. This off-by-one is the single most common cause of
"resource present in the `.blk` but my tool can't find it" bugs.

### 4.3 Which `.blk` to search

The lookup is scoped to **one `.blk` at a time**, tried in this order:

1. **`<levelName>Level.blk`** — the current level's bulk file. (Map base names match `.blk`
   names; localized `_int` maps share the base level's `.blk`.)
2. **`DynamicBulkFileTextures.blk`** — the shared dynamic pool.
3. **`<name>.blk`** — a name-derived fallback.

Walk the catalog's `File` records to find the one whose `Filename` matches the candidate, then
search its `Item` list for the `(package, object)` key.

### 4.4 Reading the block

On a match, **read `DataSize` bytes** from `[DataOffset, DataOffset + DataSize)` in the resolved
`.blk`. That range is the resource's complete external payload. The engine does **no size or
bounds check** here — it trusts the catalog record — so a faithful reader does the same.

---

## 5. How a texture block splits into mips

Textures are the primary consumer of this system, so the one type-specific detail worth
including: a texture's catalog item covers **all** of its externalized mips at once. Inside the
block they are concatenated **largest mip first**, with **no padding between mips**.

To carve them, you need the texture's format and each externalized mip's dimensions (from the
`UTexture` object in the package). Then, walking the externalized mips in declared order
(largest first), take `blockSize(Format, width, height)` bytes per mip off the front of the
block, where `blockSize` is the texture format's byte-count formula (e.g. DXT1 =
`ceil(w/4)*ceil(h/4)*8`).

This gives the strong invariant that validates the whole model:

```
DataSize == Σ blockSize(Format, mip.width, mip.height)  over every mip with Num == 0
```

Across the corpus this holds with zero mismatches. (The engine itself doesn't enforce the sum
— it trusts `DataSize` — but the equality is what proves the format is understood, and is a
good assertion to keep in a reader.)

> For the full `UTexture` object layout, the `Num == 0` externalization flag, and the
> `blockSize` table, see the texture format document. This document only covers *where the
> bytes live and how to fetch them*.

---

## 6. Reading the bulk system, end to end

1. Parse `Catalog.bdc` once (§3) into a map: `(package, object)` → `(blkFilename, DataOffset,
   DataSize)`, scoped per `.blk`.
2. When a resource needs its external bytes, build the `(package, object)` key (mind the
   `FName` suffix rule, §4.2) and try the candidate `.blk`s in order (§4.3).
3. On a hit, read `DataSize` bytes at `DataOffset` from that `.blk` (§4.4).
4. Hand the block to the owning resource type to interpret (for a texture, carve it into mips
   largest-first, §5).

---

## 7. Summary

- BioShock externalizes bulk payloads (mostly texture mip pixels) into flat **`.blk`** files
  that have **no internal structure** — just 32 KB-aligned raw blocks back to back.
- **`Catalog.bdc`** is the only index: a list of `.blk` files, each listing its resources as
  `(ObjectName, PackageName) → (DataOffset, DataSize)`. On PC `DataSize2 == DataSize` (nothing
  is compressed).
- Resolve a resource by `(package, object)`, searching `<level>Level.blk` →
  `DynamicBulkFileTextures.blk` → `<name>.blk`; read `DataSize` bytes at `DataOffset`. No
  runtime size check.
- Mind two BioShock gotchas: the **reversed `FString` sign** (positive = UTF-16) and the
  **`FName` `number - 1`, no-separator** text rule.
- A texture's block is all its externalized mips, largest-first, no padding;
  `DataSize == Σ blockSize(format, w, h)`.
</content>
