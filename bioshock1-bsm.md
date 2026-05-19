# Bioshock 1 Package & Map Format Specification

This document is a reference for Bioshock 1's `.bsm` map files and `.U` script
packages, as shipped on the GOG release of the game (Vengeance engine,
Irrational's UE2.5 fork). It collects every byte layout that has been
reverse-engineered and validated, organised so a reader implementing a loader
from scratch can work top-to-bottom.

**Audience.** Engineers porting Bioshock content into another engine, writing
standalone extractors, or working with UE2.5-era packages from the Vengeance
branch.

**Scope.** Container format (header, compression, name/import/export tables);
object wire format (per-object Vengeance header, state frame, tagged
properties); per-class binary layouts for the engine objects that have been
validated (UModel/BSP, UPolys, UTexture + Catalog.bdc bulk pipeline,
UStaticMesh, ULevel, Light actors, designer-placed `dyn_*` props); the
class-identity machinery needed to handle map-embedded UClass definitions; and
the known divergences not yet fully reverse-engineered (SkeletalMesh, Havok
rigid bodies, particle-subclass property bodies).

**Sources.**
- Static analysis of the shipped Bioshock executable (GOG, dated 2007).
- Byte-exact parsers in Python validating every layout claimed below.
- Cross-validation against UEViewer's Bioshock-specific code paths.
- Cross-references against UT2004 (Epic v128/Lic 29), Gears-era UE3
  (Epic v186), and 2012 UE3 (Epic v800+).

**Versioning.** All Bioshock packages — both `.bsm` map files and `.U` script
packages — use Epic version **141** and Licensee version **56**. The package
tag (`0x9E2A83C1`) is unchanged from UT2004.

**Status legend used throughout.**
- *Validated:* parser produces byte-exact match on every export across at least
  two test maps.
- *Documented:* layout known from binary analysis and code inspection but not
  exhaustively byte-validated.
- *Partial:* outer structure known, some inner fields not yet decoded.
- *Gap:* layout not yet known.

---

## Part A — Container Format

### A.1 Version table

| Engine | Epic | Licensee | Tag |
|---|---|---|---|
| UT2004 | 128 | 29 | `0x9E2A83C1` |
| SWAT 4 | 129 | 27 | `0x9E2A83C1` |
| **Bioshock** | **141** | **56** | **`0x9E2A83C1`** |
| Gears UE3 (2006) | 186 | 0 | `0x9E2A83C1` |
| 2012 UE3 | 800+ | varies | `0x9E2A83C1` |

### A.2 Heritage at a glance

Bioshock is a hybrid of UE2.5 serialization primitives, selectively-merged
early-UE3 features, and Vengeance-specific (Irrational) additions. The
*Vengeance-specific* column is what makes Bioshock incompatible with stock
UE2.5 / SWAT 4 / UE3 loaders.

| Feature | UT2004 | Gears UE3 | **Bioshock** | 2012 UE3 |
|---|---|---|---|---|
| Index encoding | CompactIndex | INT | **CompactIndex** | INT |
| FName on disk | NameIndex only | NameIndex only | **NameIndex + Number** | NameIndex + Number |
| FNameEntry flags | DWORD (4 B) | DWORD (4 B) | **QWORD (8 B)** | DWORD (4 B) |
| FString convention | +ANSI / −Unicode | +ANSI / −Unicode | **+Unicode / −ANSI** | +ANSI / −Unicode |
| Export Archetype | No | No | **Yes (always 0)** | Yes |
| Export ExportFlags | No | No | **Yes (always 0)** | Yes |
| Export ComponentMap | No | Yes | **No** | Removed |
| ObjectFlags size | DWORD | DWORD | **QWORD (hi=0)** | QWORD |
| FPropertyTag format | Packed info byte | Packed info byte | **Packed info byte** | FName-based |
| Bulk data | TLazyArray | TLazyArray | TLazyArray + external `.blk` | FUntypedBulkData |
| Package compression | None | None | **Custom zlib chunks** | `PKG_StoreCompressed` |
| Per-object header | None | None | **8 B Vengeance `(check, sv)`** | None |
| State frame | UE2.5 (~15 B) | UE2.5-ish | **Custom Vengeance (~46–52 B)** | UE3 (StateStack TArray) |

### A.3 File layout

A `.bsm` file is laid out as:

```
+---------------------------------------+ 0
| FPackageFileSummary (header)          |
+---------------------------------------+ 64 (when GenerationCount = 1)
| INT32 ChunkCount                      |
| INT32 ChunkOffsets[ChunkCount]        |  (file offsets of each compressed block)
+---------------------------------------+
| Compressed block 0: INT32 size + zlib |
| Compressed block 1: INT32 size + zlib |
| ...                                   |
| Compressed block N-1                  |
+---------------------------------------+ EOF
```

Concatenating the decompressed payloads of all blocks produces the
**decompressed stream**. Every offset stored in the header (NameOffset,
ImportOffset, ExportOffset, and the SerialOffset of every export) is an offset
into a virtual stream that begins with the header followed by the decompressed
payload — i.e. a position `p` in that virtual stream maps to
`decompressed[p - header_size]` once the chunk index has been parsed away.

The chunk-and-offset table is *not* part of the decompressed stream; it sits
between the header and the first compressed block.

### A.4 FPackageFileSummary

Identical in shape to UE2.5. None of the UE3 header extensions (TotalHeaderSize,
FolderName, DependsOffset, EngineVersion, CompressedChunks, etc.) are present.

```
Offset  Size  Field
0       4     Tag                = 0x9E2A83C1
4       4     FileVersion         (low 16: Epic = 141; high 16: Licensee = 56)
8       4     PackageFlags
12      4     NameCount
16      4     NameOffset           (offset into decompressed stream)
20      4     ExportCount
24      4     ExportOffset
28      4     ImportCount
32      4     ImportOffset
36      16    GUID
52      4     GenerationCount
56      8*N   Generations[]        (per entry: ExportCount + NameCount, 8 B)
```

For `GenerationCount = 1` the header ends at offset 64. UE3 generations add a
`NetObjectCount` field (so UE3 generations are 12 B); Bioshock matches UE2.5's 8 B.

### A.5 PackageFlags

UT2004 defines flags up to `0x00008000`. Bioshock observed values:

| Value | Where seen |
|---|---|
| `0x00360001` | Map files (`.bsm`) |
| `0x00270001` | Script packages (`.U`) |

The bits set above `0x8000` are Vengeance-specific. The `0x00200000` bit is the
most likely "this package is compressed" indicator (it's the bit that is always
set on files known to be compressed, and never set on files known not to be).
Other bit meanings in `0x00360000` / `0x00270000` are not yet identified.

### A.6 Zlib chunk compression

Immediately following the header (i.e. at byte 64 for GenerationCount=1):

```
Offset            Size       Field
0 (header end)    4          ChunkCount (INT32)
+4                4*N        ChunkOffsets[ChunkCount] (INT32 file offsets)
+4+4*N            variable   Compressed blocks (in any order, indexed by ChunkOffsets)
```

Each compressed block:

```
Size  Field
4     CompressedSize (INT32)
N     zlib-compressed payload (standard zlib, header bytes 78 9C)
```

Each decompressed block is exactly **32,768 bytes (32 KB)** except possibly the
last, which may be shorter. The decompressed stream is the concatenation of all
blocks in chunk-index order.

This is *not* the UE3 `PKG_StoreCompressed` / `FCompressedChunk` scheme — UE3
stores its compressed-chunk table in the header proper with
`(UncompressedOffset, UncompressedSize, CompressedOffset, CompressedSize)` per
chunk. Bioshock uses a flat "count + offsets + blocks" layout.

**Example: `0-Lighthouse.bsm`.** File size 44.2 MB; 2,490 chunks; decompressed
size 81.6 MB (81,590,631 bytes); compression ratio 54.2 %.

**Implementation note.** A practical approach is an offline preprocessor that
rewrites a compressed `.bsm` into a fully decompressed `.bsm`:

1. Read the original header.
2. Decompress every chunk into a single buffer.
3. Write a new file: (a) original header with the compression bit cleared in
   PackageFlags, (b) the decompressed payload directly (no chunk index).

This lets the rest of the load path treat the file as a plain UE2.5 package.
All offsets in the rewritten file remain valid because the header offsets were
already in *decompressed-stream space*.

### A.7 FString — reversed sign convention

UT2004, SWAT 4, *and all UE3 builds* agree:

- Positive length → ANSI (1 byte/char + null)
- Negative length → Unicode (UTF-16LE, 2 bytes/char + null)

Bioshock **reverses** this:

- **Positive length → Unicode (UTF-16LE)**
- **Negative length → ANSI**

This is the single change that breaks SWAT 4 and stock UE2.5 loaders fastest:
interpreting the first FName entry's positive length as ANSI yields `"N o n e"`
(i.e. a UTF-16 buffer mis-decoded as ANSI), and the resulting string-length
error cascades into total name-table misalignment.

The convention is unique to Irrational. It does not appear in any Epic build
at any version.

### A.8 FNameEntry — QWORD flags

UT2004 / SWAT 4 / Gears UE3 / 2012 UE3:
```
FString  Name
DWORD    Flags
```

Bioshock:
```
FString  Name      (Unicode via reversed convention)
QWORD    Flags     (8 bytes)
```

The upper 32 bits of `Flags` are always zero across every package examined
(23,303 names in `0-Lighthouse.bsm`; 8,641 in `ShockGame.U`; 7,196 in
`Engine.U`; 565 in `Core.U`). The lower 32 bits hold standard UE2.5
RF_LoadFor*/RF_Native/RF_TagExp flag combinations:

| Lower 32 bits | Count in Lighthouse | Composition |
|---|---|---|
| `0x00070010` | 21,598 | `RF_LoadFor*` + `RF_Native` |
| `0x04070010` | 1,682 | + `0x04000000` (Vengeance-specific bit) |
| `0x04070410` | 18 | + `RF_TagExp` |

Reading 4 bytes instead of 8 desynchronises the stream after the *first* name
entry, so the second name's length decodes as a huge or negative number and
`ReadFile` either allocates gigabytes or fails outright. Alignment
verification: QWORD flags produce exact alignment between name-table end and
import-table start; DWORD flags are off by 1,396,629 bytes on Lighthouse.

### A.9 FName — NameIndex + Number

UT2004 / SWAT 4 / Gears UE3 (on disk):
```
CompactIndex  NameIndex
```

Bioshock:
```
CompactIndex  NameIndex
INT32         Number     (0 = no suffix; N>0 = "_N-1" suffix)
```

This is the UE3 `(NameIndex, Number)` concept but using CompactIndex for the
index (UE2.5-style) rather than INT32 (UE3-style).

The Number field disambiguates instances: `Light_0`, `Light_1`, `Light_2`, …
all share `NameIndex = "Light"` with distinct `Number`.

**End-of-properties terminator.** A NAME_None terminator requires **both**
`NameIndex == 0 AND Number == 0`. A `None_5` (NameIndex pointing at "None",
Number=5) is a legitimate FName and must NOT terminate property iteration.
Implementations that compare only the resolved string to "None" prematurely
terminate property parsing on numbered None instances — this causes phantom
"post-properties binary tail" data that can drive a long, ultimately wrong
reverse-engineering effort (see B.2 historical note).

### A.10 FObjectImport

UT2004 / SWAT 4:
```
FName(CI)     ClassPackage
FName(CI)     ClassName
INT32         PackageIndex
FName(CI)     ObjectName
```

Bioshock:
```
FName(CI+I32) ClassPackage    (CompactIndex + INT32 Number)
FName(CI+I32) ClassName       (CompactIndex + INT32 Number)
INT32         PackageIndex
FName(CI+I32) ObjectName      (CompactIndex + INT32 Number)
```

Structurally identical to UE2.5. Each FName field gains the 4-byte instance
Number (A.9). Validated by parsing all 602 imports in `0-Lighthouse.bsm` with
exact alignment to the export-table boundary (zero gap).

The field UE2.5 calls `PackageIndex` was renamed to `OuterIndex` in later UE3
— same purpose, same encoding.

#### A.10.1 Positive `PackageIndex` on imports (UE3 convention)

UE2.5 imports use **negative** `PackageIndex` to reference other imports as
parent packages. Bioshock adopts UE3's convention: **positive** `PackageIndex`
references *exports* in the same package as parents.

In `0-Lighthouse.bsm`, 110 of 602 imports have positive `PackageIndex`. Stock
UT2004 code paths assume negative everywhere and crash in four places when
they hit a positive value:

- `VerifyImport` — assertion `Import.PackageIndex < 0` fires.
- `FindExportIndex` — export matching assumes negative.
- `GetImportFullName` — walks the chain assuming negative; on positive,
  computes a garbage array index (e.g. -2089 in our test).
- `GetExportClassPackage` — same assumption.

A loader must branch on `Import.PackageIndex > 0` in all four locations and
resolve the parent via the export table instead.

### A.11 FObjectExport

UT2004 / SWAT 4:
```
CompactIndex  ClassIndex
CompactIndex  SuperIndex
INT32         PackageIndex
CompactIndex  ObjectName        (FName index only)
DWORD         ObjectFlags
CompactIndex  SerialSize
CompactIndex  SerialOffset      (only if SerialSize > 0)
```

Gears UE3 (for comparison):
```
INT32         ClassIndex
INT32         SuperIndex
INT32         PackageIndex
FName         ObjectName        (single NAME_INDEX)
DWORD         ObjectFlags
INT32         SerialSize
INT32         SerialOffset      (only if SerialSize > 0)
TMap<FName,INT> ComponentMap
```

Bioshock:
```
CompactIndex  ClassIndex
CompactIndex  SuperIndex
INT32         PackageIndex
INT32         Archetype          (object archetype reference; always 0 in tested files)
CompactIndex  ObjectName         (FName index)
INT32         ObjectNumber       (FName instance number)
DWORD         ObjectFlags        (lower 32 bits of QWORD)
DWORD         ObjectFlagsHi      (upper 32 bits of QWORD; always 0)
CompactIndex  SerialSize
CompactIndex  SerialOffset       (only if SerialSize > 0)
INT32         ExportFlags        (always 0 in tested files; **always present, even if SerialSize == 0**)
```

2012 UE3 (for comparison):
```
INT32         ClassIndex
INT32         SuperIndex
INT32         OuterIndex
FName         ObjectName         (INT32 NameIndex + INT32 Number)
INT32         ArchetypeIndex
QWORD         ObjectFlags
INT32         SerialSize
INT32         SerialOffset
[TMap ComponentMap if Ver < VER_REMOVED_COMPONENT_MAP]
DWORD         ExportFlags
TArray<INT>   GenerationNetObjectCount
FGuid         PackageGuid
DWORD         PackageFlags
```

#### Notable points

1. **Field order vs 2012 UE3.** Bioshock has Archetype *before* ObjectName;
   2012 UE3 has ArchetypeIndex *after* ObjectName. Irrational either merged
   from a UE3 snapshot in which the order hadn't been settled, or reorganised
   it themselves.
2. **CompactIndex retention.** Bioshock keeps CompactIndex for ClassIndex,
   SuperIndex, ObjectName, SerialSize, SerialOffset. All UE3 builds use plain
   INT32 for these.
3. **QWORD ObjectFlags.** Originally easy to misread as "DWORD ObjectFlags +
   INT32 Unknown=1"; correct interpretation is "QWORD ObjectFlags, upper 32
   bits always 0" by exhaustive checking against 21,472 exports in Lighthouse
   and 23,482 in Ryan. The earlier "value 1" observation is a parsing artefact.
4. **No ComponentMap.** Bioshock does not have the Gears-era ComponentMap.
5. **No trailing fields.** No GenerationNetObjectCount, PackageGuid, or
   PackageFlags after ExportFlags.
6. **Archetype always 0.** The archetype system (instance-template inheritance)
   is either unused in Bioshock or initialised post-load. The Gears-era UE3
   build does not have this field at all.
7. **ExportFlags always 0.** In 2012 UE3 these include `EF_ForcedExport` and
   `EF_ScriptPatcherExport`; Bioshock reserves the field but doesn't use it.
8. **SerialOffsets are perfectly contiguous.** Each export's offset equals the
   previous offset + size. No padding; no gaps. Verified across all 21,472
   exports in Lighthouse and all 23,482 in Ryan.

### A.12 ObjectFlags

Common flag values observed across the export table:

| Value | Count (Lighthouse) | Composition |
|---|---|---|
| `0x00070004` | 13,004 | `RF_LoadFor*` + `RF_Public` |
| `0x000F0004` | 4,155 | + `RF_LoadForEdit` |
| `0x00070000` | 1,354 | `RF_LoadFor*` only |
| `0x02070001` | 1,342 | + `RF_HasStack` + `RF_InSingularFunc` |
| `0x000F0024` | 922 | + `RF_Transactional` |

`RF_HasStack` (`0x02000000`) is set on 1,425 objects in Lighthouse. These are
actor instances with active script execution state, and they have a state
frame between the per-object header and tagged properties (B.2).

---

## Part B — Object Wire Format

Each object is serialised at its `SerialOffset` for `SerialSize` bytes. The
body reads as:

```
+--- (SerialOffset) ----------------------+
| 8 B  Vengeance per-object header (B.1)  |
+-----------------------------------------+
| State frame (B.2)                       |  (only if RF_HasStack is set)
+-----------------------------------------+
| Tagged properties (B.3)                 |  (UObject base; always present)
+-----------------------------------------+
| Class-specific binary payload           |  (varies — see Part C)
+--- (SerialOffset + SerialSize) ---------+
```

### B.1 8-byte Vengeance per-object header

Every serialised object in Bioshock begins with an identical 8-byte prefix:

```
04 00 00 00 03 00 00 00     (INT32 = 4, INT32 = 3)
```

Identical across all exports regardless of class — UStruct-derived (UClass,
UFunction, UProperty), AActor instances, UTexture, USound, etc. Confirmed on
every export in every package examined:

| Package | Exports | All have (4,3) prefix |
|---|---|---|
| `0-Lighthouse.bsm` | 21,472 | yes |
| `ShockGame.U` | 11,276 | yes |
| `Engine.U` | 10,454 | yes |
| `Core.U` | 1,015 | yes |

This is **not** from any Epic codebase (UE3 instead writes a per-object
NetIndex that varies across objects). It is the **Vengeance per-class
serialization header** written by Vengeance's class-header serializer:

- Field 1 (`check`): always **4** for Bioshock packages. Acts as a magic /
  version identifier.
- Field 2 (`sub_ver`): **class-specific sub-version**. UObject writes 3.
  Sub-class serializers (UModel, UTexture, UStaticMesh, FStateFrame, FBspSurf)
  write their own values — see Part C and B.2.

The `(check, sub_ver)` pair gates which fields are present in class-specific
payloads (Part C). Reading the wrong number of bytes here cascades into total
stream misalignment.

### B.2 State frame (RF_HasStack objects)

Objects with `RF_HasStack` (`0x02000000`) carry a state frame between the
Vengeance header and the tagged properties. Bioshock's state frame is **NOT**
UE2.5's ~15-byte format. It is a Vengeance-specific layout, roughly **46–52
bytes** and variable due to embedded CompactIndex object refs.

Reverse-engineered from `FStateFrame::Serialize`, `FPushedState::Serialize`
(runs at least once per state frame), and the Vengeance class-header serializer:

```
# FStateFrame::Serialize
4 B   INT32   Vengeance check       (always 4)
4 B   INT32   Vengeance sub_ver     (always 1 for FStateFrame)
4 B   INT32   raw field @ 0x2C       (usually 0)
4 B   INT32   raw field @ 0x30       (usually 0)
4 B   INT32   raw field @ 0x34       (varies per-instance; looks like 2 UTF-16 chars)
8 B   QWORD   raw field @ 0x40       (usually 0xFFFFFFFFFFFFFFFF)
CI    UObject* @ 0x48                (usually NULL → 1 byte)
4 B   INT32   raw field @ 0x4C       (usually 0)
CI    UObject* @ 0x50                (typically the object's own class — for a
                                       PathNode export this resolves to import
                                       index -16, the PathNode import)

# Per-frame loop — FPushedState linked list, parent ptr at frame+0x1C.
# Top-level frame is itself one iteration. Continues while has_next == 1.
# Bioshock maps consistently have empty stacks: has_next = 0 on first iteration.
repeat {
    # FPushedState::Serialize
    CI    UObject* @ push+0x04
    CI    UObject* @ push+0x08
    4 B   INT32  size @ push+0x20    (0 = no buffered latent properties; non-zero
                                       triggers a property iteration pass that
                                       reads `size` bytes — never observed in any
                                       map sampled across PathNode, FloorPoint,
                                       Light, Brush, BlockingVolume, StaticMeshActor)
    2 B   WORD   serializer output   (always 0x8100 in observed maps; not interpreted)
    4 B   INT32  has_next            (0 = end of chain, 1 = continue loop)
}
```

For an actor with empty state stack and NULL object refs:

| Section | Bytes |
|---|---|
| Vengeance header (check + sub_ver) | 8 |
| Four raw fields (4 + 4 + 4 + 8) | 20 |
| Two CI refs @ 0x48 / 0x50 | 2–4 |
| Raw INT32 @ 0x4C | 4 |
| Loop iter (2× CI + INT32 + WORD + INT32 has_next) | 12–16 |
| **Total** | **~46–52** |

A loader must consume all fields (so the stream stays aligned for
tagged-property parsing). For editor-only use cases that don't execute
Bioshock script state, the fields can be read and discarded.

### B.3 Tagged property serialization

Bioshock uses the **UE2.5 packed info-byte FPropertyTag**, identical in shape
to UT2004. It is *not* the UE3 FName-based FPropertyTag. Confirmed by parser
comparison: 88.4 % success with UE2.5 format vs 21.2 % with UE3 format on
19,949 non-stack objects.

Each property:

```
FName   PropertyName    (CI + INT32 Number; (NameIndex==0 AND Number==0) ⇒ end)
BYTE    Info            (packed: bits 0-3 = type, bits 4-6 = size enc, bit 7 = array/bool)
[FName  StructTypeName  if Info type == StructProperty]
[size   variable-length, per Info bits 4-6 — see table]
[BYTE   ArrayIndex      if Info bit 7 set AND type != BoolProperty]
[bytes  property data of decoded size, unless BoolProperty (no data bytes — value lives in bit 7)]
```

**Size encoding** (`(Info >> 4) & 0x07`):

| Bits 4-6 | Meaning |
|---|---|
| `0x00` | 1 byte |
| `0x10` | 2 bytes |
| `0x20` | 4 bytes |
| `0x30` | 12 bytes |
| `0x40` | 16 bytes |
| `0x50` | follow-up BYTE (0–255) |
| `0x60` | follow-up WORD (0–65,535) |
| `0x70` | follow-up INT (0–2³¹) |

**Type codes** (Info bits 0-3):

```
1=ByteProperty    2=IntProperty    3=BoolProperty    4=FloatProperty
5=ObjectProperty  6=NameProperty   7=StringProperty  8=ClassProperty
9=ArrayProperty   10=StructProperty 11=VectorProperty 12=RotatorProperty
13=StrProperty    14=MapProperty    15=FixedArrayProperty
```

The end-of-properties terminator is a NAME_None FName (NameIndex 0, Number 0)
followed by *no* info byte. The terminator itself is part of the property block.

**Garbage-size guard.** When a class mismatch causes the parser to misread an
Info byte, the resulting `Tag.Size` may decode as multi-billion bytes and
trigger an effectively-infinite skip loop. A defensive loader should break
property iteration when `Tag.Size` exceeds a sane cap (e.g., 1 MB, which is
larger than any real property in the corpus).

**Property-not-found fallback.** Stock UE2.5 lookup uses `Node->GetFName() ==
Tag.Name` — integer equality on global name-table indices. For inherited
UT2004-style properties on rolled-up Bioshock classes (Part D), this
comparison can fail even when both sides print as the same string. The
Bioshock linker's `NameMap` and a native loader's `FName(TEXT(...), FNAME_Add)`
registration can end up with different name-table indexes for some properties
(root cause not yet identified — see E.3). A string-compare fallback after the
FName lookup resolves this in practice.

#### B.3.1 The ~12 % parse failure rate

On non-stack objects, ~12 % (2,319 of 19,949 in the test corpus) fail full
property parsing. Likely causes:
- Bioshock-specific property types or struct subtypes inside ArrayProperty
  containers.
- Class-specific binary payload following the property block where the parser
  treats class-payload bytes as more properties.
- A handful of objects where the `RF_HasStack` flag was missed and the
  property parser is reading state-frame bytes.

This does not block map loading — failed objects are still constructed; they
just land with default property values for the unparsed slots. Worth chasing
if you find a missing scalar field on a specific actor type.

### B.4 Object reference serialization (CompactIndex)

Inside object data, `Ar << UObject*&` resolves through a CompactIndex in
Bioshock (same as UT2004), not the plain INT32 used by UE3.

The CompactIndex value is interpreted as a 1-based signed object index:
- `0` → NULL.
- Positive `N` → ExportMap entry `N - 1`.
- Negative `N` → ImportMap entry `(-N) - 1`.

Cross-package references resolve via the import table: an FObjectImport
identifies the foreign object by `(ClassPackage, ClassName, ObjectName)`
triplet.

**Forgiving import resolution.** Bioshock maps reference many classes in
packages that may not be available to a third-party loader
(`IGSoundEffectsSubsystem`, `ShimmerShader`, `FocalDepth`, etc.). A strict
loader that aborts on the first unresolvable import will fail to load any
map. Caching failed import resolutions with a sentinel index avoids retry
loops over thousands of references.

### B.5 Bulk data — TLazyArray on disk

UE2.5's `TLazyArray<T>` is the format used for inlined-but-skippable bulk data
(legacy mip pixels, geometry buffers). Bioshock uses TLazyArray for a small
set of in-package buffers but moves the heavy data (texture pixels) into
external `.blk` files indexed by `Catalog.bdc` (Part C.3). UE3's
`FUntypedBulkData` is *not* present.

---

## Part C — Per-class binary layouts

For each class, the body that follows the tagged-property block in the
export's serial range has a custom binary layout. This part documents every
class layout that has been reverse-engineered and validated.

### C.1 UModel + BSP (Vengeance sv = 7)

**Status:** Validated (283/284 byte-exact on `0-Lighthouse.bsm`).

**Sources:** `UModel::Serialize` and sub-functions:
- `FBspNode::Serialize` (100 B fixed)
- `FBspSurf::Serialize` (52 B + 8 B per-element Vengeance header)
- `FVert::Serialize` (8 B, inlined)
- `FLeaf::Serialize` (12 B)
- `FBox::Serialize`
- `LeafHulls::Serialize`

#### C.1.1 UModel::Serialize layout

```
Super::Serialize(Ar)                          // UObject — Vengeance hdr + tagged props
8 B    Vengeance class header (check=4, sv=7)

TArray<FVector>           Vectors             // standard UE2.5 layout
TArray<FVector>           Points              // standard UE2.5 layout

CI    NumNodes
loop NumNodes {
    100 B FBspNode                            // see C.1.2
}

CI    NumSurfs
loop NumSurfs {
    8 B   per-element Vengeance header (check, sub_ver)
    52 B  FBspSurf body                       // see C.1.3
}

CI    NumVerts
loop NumVerts {
    8 B   FVert                               // see C.1.4
}

INT32 NumSharedSides
INT32 NumZones                                 // clamped to MAX_ZONES (UE2.5 = 64)
loop NumZones {
    FZoneProperties                            // see C.1.5 — 128-bit zone bitmasks
}

CI    UObject* Polys                           // CI ref to UPolys export (C.2)

TArray<INT>     Bounds                         // standard layout
TArray<INT>     LeafHulls                      // standard layout

CI    NumLeaves
loop NumLeaves {
    12 B  FLeaf                                // see C.1.6
}

CI    NumLights                                // **read and DISCARDED** for portability
loop NumLights {
    CI  UObject* lightref                      // discarded — Bioshock light refs
}                                               //  are incompatible with UE2.5 renderer

[discard one extra CI object ref array — purpose not RE'd]

INT32 RootOutside
INT32 Linked

[remaining rendering/lighting arrays are typically skipped via a SerialSize
 safety seek (B summary). They contain Vengeance-specific lightmap and
 rendering data that a UE2.5 renderer cannot consume.]
```

#### C.1.2 FBspNode (100 bytes, fixed)

```
Offset  Size  Field
0       16    FPlane Plane            (FVector + W float)
16      16    QWORD ZoneMask[2]       (128-bit; truncated to 64-bit single QWORD on load)
32      1    BYTE  NodeFlags
33      ...   (alignment / padding)
        4    INT   iVertPool
        4    INT   iSurf
        4    INT   iVertexIndex
        4    INT   iCollisionBound
        4    INT   iRenderBound
        2    WORD  iZone[2]            (clamped to 0 if >= MAX_ZONES = 64)
        1    BYTE  NumVertices
        1    BYTE  iLeaf[2]
        4    INT   iSection
        4    INT   iFirstVertex
        4    INT   iLightMap
... (100 B total)
```

Bioshock ships **128-bit ZoneMask** because Vengeance's `MAX_ZONES = 128`.
UE2.5 uses a single 64-bit QWORD (MAX_ZONES = 64). A UE2.5 loader should
truncate to the lower 64 bits on load and clamp `iZone[0]/iZone[1]/iLeaf-iZone`
to 0 when `>= 64`.

`iLightMap`, `iSection`, `iFirstVertex` are typically cleared to `INDEX_NONE`
after load — they should be rebuilt by the engine's render-data builder.

#### C.1.3 FBspSurf (52 B body + 8 B per-element Vengeance header)

```
8 B    per-element Vengeance header (check, sub_ver)

CI     UObject* Material              (resolves to UShader/UTexture in package)
CI     UObject* Actor                 (resolves to ABrush instance)
INT32  PolyFlags
INT32  pBase
INT32  vNormal
INT32  vTextureU
INT32  vTextureV
INT32  iLightMap
INT32  iBrushPoly
INT32  iZone[2]
WORD   PanU
WORD   PanV
INT32  iLightmapIndex
... (52 B body total)
```

`Material` resolves through the pointer and is later flattened to a `UTexture`
by a post-load pipeline (D.4) if the target renderer cannot evaluate
Bioshock's shader trees. `Actor` is read but not stored on UModel surfs —
brush ABrush actors aren't wired into Level yet at UModel serialize time.

#### C.1.4 FVert (8 B)

```
INT32   pVertex     (raw INT32 — UE2.5 uses CompactIndex here)
INT32   iSide       (raw INT32)
```

UE2.5's FVert uses CompactIndex for both fields; Bioshock uses raw INT32.

#### C.1.5 FZoneProperties (128-bit bitmasks)

Bioshock zones use **128-bit** `Connectivity` and `Visibility` bitmasks
(Vengeance MAX_ZONES = 128). UE2.5's FZoneProperties uses 64-bit. A UE2.5
loader should read the full 128 bits and truncate to the lower 64.

```
CI    UObject* ZoneActor
QWORD Connectivity[2]   // 16 bytes — truncated to 8 on load
QWORD Visibility[2]     // 16 bytes — truncated to 8 on load
INT32 LastRenderTime
```

#### C.1.6 FLeaf (12 B)

```
INT32   iZone
INT32   iPermeating     // cleared to INDEX_NONE on load (no light lookups)
INT32   iVolumetric     // cleared to INDEX_NONE on load
```

UE2.5's FLeaf has an additional `QWORD VisibleZones` (8 B) bringing it to 20
B. Bioshock removed the field entirely. A UE2.5 loader can set `VisibleZones
= ~0ULL` (all bits set) so per-leaf visibility queries succeed by default.

#### C.1.7 Brush UModel

ABrush actors carry their own `UModel` instance (`Actor.Brush`) for source
geometry. The brush UModel uses the same C.1.1 layout. With UPolys preload
enabled (C.2), the brush's `Polys` reference resolves to a real UPolys export
and `Build → Geometry` works.

### C.2 UPolys + FPoly

**Status:** Validated:
- Lighthouse: 284/284 UPolys byte-exact, 1,687 FPoly elements.
- Fort Frolic: 1,312/1,312 UPolys byte-exact, 6,940 FPoly elements.
- All field-validity checks (unit normals, valid Actor/Material refs, sane
  iLink/iBrushPoly/LightMapScale) pass across all 8,627 elements.

**Sources:** `UPolys::Serialize` and `FPoly::operator<<`. Both decompile
cleanly to byte-for-byte copies of UE2.5's inlines, except for the FName
widening inside FPoly.

#### C.2.1 UPolys::Serialize

```
Super::Serialize(Ar)                          // UObject — 8 B Vengeance hdr (sv=3) + tagged props
if (!Ar.IsTrans()) {
    Ar->CountBytes(Num * 0x14C, Max * 0x14C)  // 0x14C = 332 = sizeof(FPoly) Bioshock
    INT32 DbNum                               // raw INT32, NOT CompactIndex
    INT32 DbMax
    if (Ar.IsLoading()) {
        Element.Empty(DbNum)
        Element.AddZeroed(DbNum)
    }
    for (i = 0; i < DbNum; i++)
        Ar << Element[i]                      // FPoly operator<< (C.2.2)
}
```

**No UPolys-level Vengeance header.** Unlike UModel (sv=7), UTexture,
UStaticMesh (sv=8), and FStateFrame (sv=1), UPolys does *not* call the
Vengeance class-header serializer. The class is format-stable between UE2.5
and Bioshock except for the FName widening inside FPoly.

UPolys has no native script properties; the tagged-property section observed
in Bioshock files is ~15 B (one inherited UObject-base property plus None
terminator).

#### C.2.2 FPoly element

```
CI     AR_INDEX(NumVertices)        // 1–2 B on disk; range 3..16
48 B   FVector Base, Normal, TextureU, TextureV    // 4 × 12 B raw floats
N×12   FVector Vertex[NumVertices]                 // 3 floats each
4 B    DWORD PolyFlags
CI     UObject* Actor                             // CI → ABrush owner
CI     UObject* Material
FName  ItemName                                   // CI NameIndex + INT32 Number
                                                  //  = 5–9 B (depends on NameIndex CI width)
CI     AR_INDEX(iLink)                            // FBspSurf index, or -1 INDEX_NONE
CI     AR_INDEX(iBrushPoly)                       // source editor brush poly index
4 B    FLOAT LightMapScale                        // Ver >= 106; always present (Bioshock = 141)
```

In-memory `sizeof(FPoly)` in Bioshock is **0x14C (332 B)**, 4 B larger than
UE2.5's 0x148 (328 B). The extra 4 B is the widening of `FName ItemName` from
4 B (CompactIndex only) to 8 B (CompactIndex + INT32 Number). The disk layout
reflects this widening transparently because UE2.5's `FArchive
operator<<(FName&)` routes through the linker's FName operator, which reads
the extra Number on `Ar.Ver() >= 141`.

`Ar << Poly.Actor` and `Ar << Poly.Material` go through the `UObject*&`
operator which uses CompactIndex object refs (B.4) — same as UE2.5.

Observed FPoly distribution across 1,596 UPolys exports on two test maps:
6-vertex polys dominate (axis-aligned cube brushes = 6 quads), with occasional
5, 10, and 32-vertex elements from complex CSG brush geometry. Single-poly
UPolys (147 B total) appear for triangle brushes.

#### C.2.3 Implication for a UE2.5-based loader

UE2.5's existing UPolys code path loads Bioshock UPolys correctly **without
modification**, provided the FName operator handles the v141 Number widening
(A.9). Adding `"Polys"` to any Preload allowlist unblocks brush source
geometry loading and enables `Build → Geometry` / CSG rebuild.

### C.3 UTexture + Catalog.bdc + .blk bulk data

**Status:** Validated end-to-end. 1,470 textures populated on Fort Frolic;
2,352 Shader→Texture flattenings; renders cleanly in editor.

#### C.3.1 UTexture::Serialize

```
Super::Serialize(Ar)                          // UObject — 8 B Vengeance hdr (sv=3) + tagged props
8 B    Vengeance class header (check=4, sub_ver)
[if sub_ver >= 1:]
INT64  CachedBulkDataSize                      // hint for the external bulk loader
return                                         // do NOT call SerializeMips
```

The remaining bytes inside the export's serial range are skipped by the
SerialSize safety seek. Unlike UE2.5, Bioshock does NOT inline the mipmap
pixel payload — all pixel data lives in external `.blk` files and is loaded
by the post-load pass (C.3.3).

**Tagged-property defaults that need post-load fixup.** Bioshock writes:
- `MinLOD = 4` or `5` (Bioshock uses high values intentionally; the engine's
  `DefaultLOD` clamps to skip lower mips for streaming purposes).
- `MaxLOD = -1` (sentinel meaning "no maximum").
- `UClamp = 0`, `VClamp = 0` (uninitialised; engine derives from USize/VSize
  at runtime).

UE2.5's `DefaultLOD()` returns `Clamp(NormalLOD, MinLOD, MaxLOD)`. With
`MinLOD = 4` and `Mips.Num() = 1` post-load, this evaluates to 4, so the
renderer asks D3D for `NumMips - FirstMip = -3` mip levels and crashes in
texture cache creation.

Format **12** is Bioshock's DXT5N (DXT5-encoded normal maps, channel-swizzled).
A FF renderer can map it to `TEXF_DXT5` for rendering — the swizzle is
handled in the shader (or accepted as visually-incorrect but non-crashing).

#### C.3.2 Catalog.bdc

The `.blk` bulk data files are indexed by `Catalog.bdc`, a binary file at the
game root (or alongside the `.bsm` files). One entry per cached texture:

```
CompactIndex   Count
loop Count {
    FString    TextureName             (full FQN — uses Bioshock's reversed sign convention)
    CompactIndex / FString  BlkFile    (e.g. "DynamicBulkFileTextures.blk")
    INT32      DataOffset              (byte offset into the .blk file)
    INT32      DataSize                (byte count)
}
```

A loader implementing this should provide helpers for:
- A variable-length int reader from raw buffer (for CompactIndex outside an
  archive context).
- An FString reader applying the reversed sign convention (A.7).
- A DXT/uncompressed mip byte-size calculator (DXT1: U*V/2; DXT3/5: U*V;
  uncompressed: depends on format).
- A name→entry map for the loaded catalog.

#### C.3.3 Bulk data loader (post-load pass)

After all UTexture objects are constructed and have their tagged properties:

```
For each .blk file referenced by any texture in this map:
    Open file once
    For each texture whose catalog entry points at this file:
        Seek to DataOffset; read DataSize bytes into a reusable buffer
        Split bulk data into mip chain (largest first, halving dimensions)
        For each mip level:
            new(Tex->Mips) FMipmap(UBits, VBits, MipSize)   // placement-new — see runtime fix
            copy MipSize bytes into the FMipmap's TLazyArray<BYTE>
        Reset MinLOD = 0; MaxLOD = NumMips - 1; NormalLOD = 0
        UClamp = USize; VClamp = VSize
    Close file
```

**Why placement-new and not `Mips.Add(N)`.** `FMipmap` contains
`TLazyArray<BYTE>` which inherits from `FLazyLoader` (has virtual functions).
`Add(N)` raw-grows memory then *copy-assigns* via the compiler-generated
`operator=`, which never runs the FMipmap constructor — the vtable stays
zero-initialised (or `0xFEFEFEFE` under FMallocDebug). When the D3D renderer
later calls `DataArray.Load()` it dispatches through a NULL/garbage vtable and
AVs. Placement-new (`new(Tex->Mips) FMipmap(...)`) constructs the FMipmap in
place with a real vtable.

**Per-blk-file open instead of per-texture.** Opening the `.blk` file once per
texture (1,135 file opens on `3-Market.bsm`) and allocating a fresh
`TArray<BYTE>` per texture (1,135 700KB–1.4MB allocations) can, on Win11
native, fragment 32-bit process VM enough that the ~1,100th
`BulkData.Add(960992)` in Release builds hits "out of virtual memory" — a
heisenbug that disappears under the debugger (NT Heap doesn't fragment the
same way as Segment Heap). The fix:

1. **Set the `LARGE_ADDRESS_AWARE` flag on the editor executable.** PE32
   Characteristics ORed with `0x0020` at offset `PE_header + 22`; PE checksum
   zeroed. Doubles the VM ceiling from 2 GB to 4 GB on 64-bit Windows.
2. **Refactor to a two-pass loop:** Pass 1 collects eligible textures and
   tracks `MaxDataSize`. Pass 2 walks unique `.blk` filenames; for each, opens
   once, processes every matching work entry, closes once. A single reusable
   `TArray<BYTE> BulkData` is pre-sized to `MaxDataSize` and reused across all
   reads. Market drops from 1,135 opens / 1,135 VirtualAllocs to **16 opens /
   1 persistent buffer**.

**MSVC 2003 sharp edges encountered during the refactor:**
- `TArray<WorkItem>::AddItem(W)` raw-grows then copy-assigns via
  compiler-generated `operator=`, chaining into `FString::operator=` on
  uninitialised string fields (whose `Data` is whatever garbage the pool
  returned). Replace with placement-new:
  `auto& W = *new(Work) WorkItem;` — grows the array AND default-constructs
  the slot.
- `TArray<FString>::AddUniqueItem` has the same trap. Replace with manual
  scan + `new(UniqueBlks) FString(Name)`.
- Work item structs must be hoisted to file scope — MSVC 2003 rejects local
  structs as template arguments (C2926: "types with no linkage").

#### C.3.4 Defensive LOD sanitization

Some textures end up with non-empty `Mips` through paths the bulk loader
doesn't control (e.g. UCubemap's separate format). They miss the per-texture
LOD reset above and still crash at `CreateTexture`. A final sweep over all
UTexture instances after the bulk loader should:

- Clamp `MinLOD` to `[0, Mips.Num() - 1]`.
- Clamp `MaxLOD` to `[0, Mips.Num() - 1]`.
- If `NormalLOD` out of range, set to 0.
- Fill `UClamp`/`VClamp` from `USize`/`VSize` if zero.
- For empty-mips textures (`Mips.Num() == 0`), force
  `MinLOD = MaxLOD = NormalLOD = 0` so downstream arithmetic stays
  non-negative.

### C.4 UStaticMesh (Vengeance sv = 8)

**Status:** Validated (100 % across all tested maps; 610 UStaticMesh objects
on Fort Frolic = 411,235 verts / 1,131,573 indices).

Layout cross-validated against UEViewer's `UnMeshBioshock.cpp`.

#### C.4.1 Layout

```
Super::Serialize(Ar)         // UPrimitive: tagged props + FBox + FSphere
                             // (UPrimitive base writes the bounds again — see below)

8 B    Vengeance class header (check=4, sv=8 in observed maps)

CI     NumSections
loop NumSections {           // 14 B per section
    INT32  f4              // always 0 → mapped to UE2.5 IsStrip BOOL
    WORD   FirstIndex
    WORD   FirstVertex     // → UE2.5 MinVertexIndex
    WORD   LastVertex      // → UE2.5 MaxVertexIndex
    WORD   fE              // alias for NumFaces in some sub-paths
    WORD   NumFaces        // → UE2.5 NumTriangles AND NumPrimitives
}

25 B   FBox BoundingBox       // serialized AGAIN (UPrimitive field repeated by Vengeance)

CI     NumVerts
loop NumVerts {              // 24 B per vertex
    12 B  FVector Position
    4 B   FPackedNormal Tangent     // DWORD: X/Y/Z signed bytes in [-127, 127] mapped to [-1, 1]
    4 B   FPackedNormal Binormal    //   (W byte unused or sign indicator)
    4 B   FPackedNormal Normal      // <- this is the actual normal (per UEViewer)
}

CI     NumUVStreams
loop NumUVStreams {
    CI    NumUVs
    loop NumUVs {            // 8 B per UV
        FLOAT U
        FLOAT V
    }
    INT32 CoordinateIndex
}

CI     NumIndices
loop NumIndices {
    WORD  Index              // 2 B each
}
INT32  IndexBuffer.Revision

[remaining bytes — kDOP / collision / lighting GUIDs / etc. — skipped by the
 SerialSize safety seek (B summary). Not yet RE'd; not needed for editor render.]
```

**Section field mapping.** Bioshock's 14-byte section happens to map to
UE2.5's section fields byte-for-byte:
- `INT32 f4` (always 0) → `UBOOL IsStrip`
- `WORD FirstIndex` → `FirstIndex`
- `WORD FirstVertex` → `MinVertexIndex`
- `WORD LastVertex` → `MaxVertexIndex`
- `WORD fE` / `WORD NumFaces` → `NumTriangles` / `NumPrimitives` (both
  populated to `NumFaces`)

**Vertex normal extraction.** Each `FPackedNormal` is a DWORD packing X/Y/Z as
signed bytes in `[-127, 127]` mapped to `[-1, 1]`. UEViewer takes `Normal[2]`
(the third packed normal, after tangent and binormal) as the actual surface
normal. The other two are tangent-space basis vectors used by Bioshock's
pixel shaders.

**Materials are populated automatically** via UStaticMesh's registered
`Materials` array property. For Bioshock LicenseeVer=56 ≥ 0x1C, struct
elements use recursive tagged-property serialization which handles
Bioshock-specific fields (`HavokCollisionTypeStatic`, etc.) via the
unknown-property skip path.

**`InternalVersion` should be set to `STATICMESH_VERSION` after read** so
PostLoad's `if(InternalVersion < STATICMESH_VERSION) Build()` does NOT fire —
the render data is already populated directly. PostLoad becomes a no-op for
Bioshock meshes.

#### C.4.2 Required runtime fixups

Discovered iteratively after initial mesh load implementation:

1. **UVStreams need real construction (not `Add(N)`).** `FStaticMeshUVStream`
   has a non-trivial constructor (sets `CacheId` via `MakeCacheID`) and an
   inner `TArray<FStaticMeshUV> UVs` whose data pointer must be NULL before
   `Empty()` can free anything. `TArray::Add(N)` returns N raw uninitialised
   slots (`0xFEFEFEFE` under FMallocDebug) and the first `UVs.Empty(N)` AVs
   trying to free a wild pointer. Use `new(UVStreams) FStaticMeshUVStream()`
   per element. Same pattern for Sections. POD inner arrays (Vertices,
   Indices) don't need this because every byte is overwritten by `Ar << ...`
   reads.
2. **Vengeance check guard.** Bail with a warn if the 8 B `(check, sub_ver)`
   header doesn't read `check == 4`. Prevents reading garbage Section/Vertex
   counts (and trying to allocate gigabytes) when tagged-prop deserialization
   went off the rails (e.g., Bioshock-only struct field tag overshooting size).
3. **Count sanity guards.** NumSections > 1024, NumVerts > 5M, NumUVStreams >
   16, NumUVs > 5M, NumIndices > 10M → bail with a warn.
4. **WireframeIndexBuffer required for editor viewports.** UnrealEd's default
   4-pane layout has 3 wireframe orthographic + 1 textured perspective. The
   wireframe path binds `&StaticMesh->WireframeIndexBuffer` unconditionally
   and the D3D render interface's `SetIndexBuffer` asserts on size mismatch
   with the cached D3D resource. Leaving it empty triggers the assertion
   immediately on map open. Derive WireframeIndexBuffer from IndexBuffer as 3
   line-list pairs per triangle (no edge-dedup; the standard
   `UnStaticMeshBuild` path does dedup but we don't have RawTriangles to walk).
5. **Vertex color streams need white fill.** The render path binds either
   `StaticMesh->ColorStream` or `AlphaStream` as a per-vertex FVF color. Both
   inherit `FRawColorStream` which declares `FVF_Diffuse` / `FVF_Specular`.
   With empty `Colors` arrays the pipeline reads zero per vertex and the FF
   modulation computes `texel * 0 = black`. After populating
   `VertexStream.Vertices`, fill both `AlphaStream.Colors` and
   `ColorStream.Colors` with `FColor(255, 255, 255, 255)` per vertex — opaque
   white is the neutral element under modulation. BSP rendering doesn't hit
   this because surf rendering uses a different shader path.

### C.5 ULevel

**Status:** Documented but not preloaded.

ULevel's stream format **matches UE2.5 exactly**: `DbNum`/`DbMax` +
CompactIndex actor refs, no Vengeance per-class header. Loading it directly
works: ULevel loads successfully with 11,460 actors and Model set. *However*,
the loaded actors cause cascading crashes downstream — non-AActor objects in
the Actors array (wrong vtable), NULL StateFrames in zone events, NULL
DefaultPhysicsVolume, freed render data.

A working approach is **selective scaffolding**:

1. After `LoadPackage` returns, detect "this is a Bioshock map" via
   `Level->Model == NULL` (no UE2.5-format ULevel was loaded).
2. Build minimal scaffolding matching the `ULevel::ULevel` constructor:
   - Empty Model.
   - LevelInfo at `Level->Actors(0)`.
   - Default Brush at `Level->Actors(1)` (with its own empty Model).
3. Find the largest UModel object in the package by node count and assign it
   as `Level->Model` (this is the main BSP).
4. Iterate all AActor instances over the package and add valid ones to
   `Level->Actors`. Set `XLevel = Level` and `Level = LevelInfo` on each so
   zone/physics queries don't crash on NULL pointers.
5. **Skip `PostEditLoad()`** for Bioshock actors. PostEditLoad on AProjector
   (and many other types) calls `SetZone → AZoneInfo::eventActorLeaving →
   IsProbing` on NULL zone actors and crashes. Detect Bioshock maps and skip.

The trade-off: any Vengeance-specific ULevel fields are lost (their content
isn't known) but the result is a clean, working `Level->Actors` array with
known-good actor instances.

### C.6 Light actor — property-encoding quirks

**Status:** Validated end-to-end. Lights bake correctly in-editor after
`Build → Lighting`.

Bioshock writes light parameters with **different types and field names** than
UE2.5 expects. A loader can intercept during tagged-property deserialization.

| Field | Bioshock | UE2.5 | Result without fixup |
|---|---|---|---|
| Brightness | `LightBrightness` **FloatProperty** (0.0–3.1, median 1.0) | `LightBrightness` **BYTE** (0–255, default 64) | Type mismatch → property skipped → stays at default |
| Color | `LightColor` **StructProperty:Color** (FColor BGRA) | `LightHue` + `LightSaturation` **BYTEs** | Property name not on class → silently dropped |
| Radius | `LightRadius` **FloatProperty** (0–120 000 world units, median 2048) | `LightRadius` **BYTE** with `WorldLightRadius = 25 * (byte + 1)` | Type mismatch → skipped |

**Conversion handling in `UStruct::SerializeTaggedProperties`:**

1. **Property-not-found + StructProperty:Color named "LightColor"** — read 4 B
   as FColor BGRA, convert RGB → standard HSV, then map to UE2.5's HSV bytes:
   - Hue: 0=red, 85=green, 170=blue piecewise.
   - Saturation: **inverted** (0 = full color, 255 = white).
   Walk the class's PropertyLink chain to find `LightHue` + `LightSaturation`
   properties and write the converted bytes directly to those offsets. Works
   for any Bioshock-derived class with those fields (Light, BathLight_8,
   DynamicLight_Camera, etc.).

2. **Type-mismatch FloatProperty (file) → ByteProperty (class)** — read 4 B
   as float, scale to 0–255 byte. Scale factor depends on property name:
   - `LightRadius`: `byte = F / 25 - 1` (UE2.5's `WorldLightRadius = 25 *
     (LightRadius + 1)` formula inverted).
   - `LightBrightness`: `byte = F * 255` (Bioshock 1.0 = "normal" intensity →
     UE2.5 max so the renderer's `(LightBrightness/255)` multiplier is 1.0;
     an earlier `* 64` mapping produces ~0.25 multiplier and bakes all
     interior lights dim).
   - Other float→byte targets: pass through clamped to `[0, 255]`.

#### C.6.1 `bStatic` not written

A probe across all 184 Light actors on Lighthouse showed `bStatic` and
`bNoDelete` are **never written by Bioshock to disk**. The class-default
`bStatic = 1` from UE2.5's `ALight` should apply, but a rollup path (D.2)
doesn't reliably preserve it (defaults are normally written per-instance
during construction; `bStatic` happens to be on AActor, not ALight). Result:
every Bioshock light fails the visibility builder's filter:

```cpp
Actor->LightType != LT_None && (Actor->bStatic || Actor->bNoDelete) && !IsHiddenEd
```

`Model->Lights` stays empty, no per-leaf permeation, no bake.

**Fix:** in the actor-wiring loop, when `IsA(ALight)` and neither `bStatic`
nor `bNoDelete` is set, force `bStatic = 1`.

#### C.6.2 Workflow

`Build → Lighting` calls `TestVisibility` internally, so the visibility
builder runs as part of lighting bake. Bioshock interior lights bake
correctly after this.

#### C.6.3 Cycle guard required for portal-DFS

`Build → Lighting` on Bioshock maps can stack-overflow in the editor
visibility recursive portal-DFS lightsource-permeation test. For a light at
`Y = 10655.999` against a clipper plane at `Y ≈ 10656.015` — gap ~0.015 world
units — the `SplitWithPlaneFast` clipping step can produce a degenerate
near-zero-length plane normal and the recursion has no natural termination.
Bioshock maps are especially prone because their BSP was built for
Vengeance's visibility scheme, not UE2.5's portal builder.

**Fix:** cycle protection via per-leaf "on current DFS path" bit array. Mark
entering, clear returning. Caps recursion depth at `Model->Leaves.Num()` and
makes any portal-graph cycle harmless. This is correctness for any BSP, not
specific to Bioshock.

### C.7 dyn_* designer-placed physics props

**Status:** Validated. 163/168 dyn_* instances on Fort Frolic get meshes; 5
miss because their sidecar FQN doesn't resolve against this map's loaded mesh
set. The 8 mesh-less `dyn_*` classes in the sidecar stay invisible (no
regression).

Bioshock maps contain hundreds of designer-placed `dyn_*` props
(`dyn_cafe_table`, `dyn_debris_luggageA`, `dyn_Mannequin`, `dyn_grate_64`, …)
— subclasses of `ShockGame.PhysicalReactiveActor` whose **class definition**
lives inline in the `.bsm` file. Each class's `StaticMesh` default is
authored in the editor and stored in the class's default-properties block.
Without intervention, a class rollup (D.2) collapses these to bare `Actor`
and the instances land in `Level->Actors` with correct
`Location`/`Rotation` but no visible geometry.

A working approach is an **offline sidecar** generated by extracting class
defaults from the map's UClass exports:

#### C.7.1 Sidecar format

```
ClassName=StaticMesh_FQN | CollisionRadius | CollisionHeight | DrawScale | HavokDataClass
```

Examples:

```
dyn_cafe_table=Gen_Decor.cafe_table||||HavokPhysics.Aluminum10pc
dyn_debris_luggageA=Int_Debris.debris_luggageA||||HavokPhysics.Leather20pc
dyn_grate_64=Gen_Decor.Grate_64||||HavokPhysics.Steel20pc
dyn_Low_Rent_TV=||||                # no StaticMesh default — skip spawn
```

Fields are pipe-separated; empty fields are allowed. Leading map-package
segments are stripped from object refs
(`4-Recreation_decompressed.Gen_Decor.cafe_table` →
`Gen_Decor.cafe_table`) so the same sidecar works for any map.

#### C.7.2 Generator

UELib's `BulkDecompile` can extract class defaults from each map's UClass
exports. Two UELib bugs had to be fixed first:

1. **Cross-package import resolution.** UELib's `FindProperty` walks the
   super chain looking for inherited array/struct property types; without
   the ref packages loaded, the chain breaks at `PhysicalReactiveActor`
   (which lives in `ShockGame.U`, not the map). A `--map <out> <map.bsm>
   <ref1.u> <ref2.u> …` mode that loads refs first, walks the map's import
   table, and patches each `UImportTableItem.Object` to point at the real
   object in the appropriate ref package resolves 371/1378 imports on Fort
   Frolic. Case-insensitive package-name matching that strips
   `_decompressed` suffixes is needed.
2. **UELib's Vengeance `UClass::Serialize` post-header block has wrong bytes
   for Bioshock's `LicenseeVersion = 56`.** Binary analysis of
   `UClass::Serialize` shows the `LicVer >= 30` branch reads
   `INT32 + INT32 + FName (CompactIndex + INT32 Number)`. UELib reads
   `INT32 + INT32 + INT32 + CompactIndex` and asserts the last is zero — a
   3-byte over/under-read that misaligns every subsequent tagged-property
   walk. Fix to `int + int + _Buffer.ReadNameReference()`. Coverage jumps
   from 121/185 (65 %) to 177/185 (96 %) across the 9-map corpus. The
   remaining 8 classes (`dyn_sign`, `dyn_billiard_ball`, `dyn_Low_Rent_TV`,
   …) genuinely have no `StaticMesh` in their class defaults.

A generator can union records across a BulkDecompile run of all 9 maps so
the sidecar works for any of them.

#### C.7.3 Loader wiring

1. **Sidecar loader.** Lazy-init. Loads `dyn_defaults.txt` from the same
   directory as the `.bsm`. Exposes `LookupDynDefault(FName, FDynDefault&)`
   plus a registered-actor queue.
2. **CreateExport rollup override.** After the super-walk + `rollup.txt`
   fallback computes a `RollupClass`, check whether the loaded class is a
   map-embedded `dyn_*` class. If yes and the sidecar has a record with a
   `StaticMesh` FQN: override `RollupClass` to `StaticMeshActor` (found by
   name in `ANY_PACKAGE`, since Core typically can't reference Engine classes
   by symbol). If the sidecar record has no mesh, return NULL to skip the
   export. If no sidecar record, fall through to the existing `Actor`
   rollup.
3. **Post-construct registration.** After `StaticConstructObject` succeeds
   and `SetLinker` wires the new actor to its linker, push the
   `(Actor*, OriginalClassName, FDynDefault)` triple onto the
   registered-actor queue.
4. **Post-load mesh assignment** (after UShader sanitization + mesh material
   flattening, before `Level->ReconcileActors()`). Iterate the queue,
   resolve each mesh FQN to a `UStaticMesh*` via
   `StaticFindObject(ANY_PACKAGE, Leaf)` (Bioshock `.bsm` mesh leaf names
   are unique per map, so the bare-name lookup is safe), assign
   `StaticMesh` + call `SetDrawType(DT_StaticMesh)`, copy
   `DrawScale`/`CollisionRadius`/`CollisionHeight` if non-zero in the
   sidecar.

**Important: do not warn per-actor on missed meshes.** A per-actor `debugf`
warning at this site reliably corrupts the heap on the MSVC 2003 toolchain
(suspected interaction with va_args + FString temporaries; reproduces even
when using `FString::Printf` + the no-EName overload). A summary-only count
is sufficient. To debug specific missing-mesh cases, re-enable the warning
locally and tolerate the crash.

**What's not done:** Havok physics binding (props are static — see E.2).
`DamagedReactions[]` / `BumpedReactions[]` / `DestroyedReactions[]` replay.
Other map-embedded class families outside `dyn_*` (pickup subclasses, door
variants, FXClass emitters — each is a separate follow-on with the same
sidecar shape).

---

## Part D — Class identity, rollup, and material flattening

Bioshock packages embed class definitions for game-specific subclasses
(`dyn_*` props, `FoamIntro`, custom Emitter subtypes). These embedded UClass
objects use UE3's UStruct/UClass serialization format, which is
**fundamentally incompatible** with UE2.5 — field order, field count, and
semantics all differ. A UE2.5-based loader cannot deserialize them. This part
covers the machinery that lets the loader work around that.

### D.1 Map-embedded UClass — preload skip

For UStruct-derived objects (UClass, UFunction, UProperty, …) in Bioshock
packages, a UE2.5 loader should **skip preload entirely**. The body would
otherwise deserialize Bioshock's UE3-style UClass via UE2.5's UE2.5 layout
and crash on `assert(GetPropertiesSize() >= sizeof(UObject))`.

Skipping preload for these objects has knock-on effects:

- **Clear `RF_NeedPostLoad`** along with `RF_NeedLoad` so PostLoad doesn't
  run on partially-initialised classes.
- **Set `FriendlyName` to the object's own name** so GC's
  `UStruct::Serialize` reachability test doesn't assert.
- **Null `SuperField`** so `TFieldFlagIterator` during GC doesn't walk the
  super chain into UE2.5 native classes and call `SerializeItem` with a NULL
  base pointer.
- **Skip exports whose class was map-embedded.** When `CreateExport` reaches
  an instance of a class that has no `ClassConstructor`, or whose linker is
  this package (i.e. the class was defined in the Bioshock map and never
  properly linked), skip the instance gracefully with a warning. This loses
  the actor unless rollup catches it (D.2).
- **NULL-parent fallback.** When an export's parent was skipped,
  `IndexToObject` returns NULL. `StaticAllocateObject` then fails with
  "Object is not packaged." Fall back to `LinkerRoot` (the package itself)
  when the parent resolves to NULL.

### D.2 Class rollup

When `CreateExport` would otherwise skip an export because the class is
map-embedded (D.1), a rollup walker can try to find a UE2.5-native ancestor
class and construct the object as that ancestor instead. This loses
Bioshock-specific behavior but lets the instance land in `Level->Actors`
with its tagged properties (Location, Rotation, DrawScale, StaticMesh, etc.)
intact.

Two-tier resolution:

1. **Export-table SuperIndex walk.** Start from
   `LoadClass->_LinkerIndex + 1` (NOT `Export.ClassIndex`, which may be a
   negative import that resolved to a local export). Follow each export's
   `SuperIndex` field. When `SuperIdx` is a negative import that can't be
   resolved (Bioshock imports from non-standard packages like `FocalDepth`),
   fall back to `StaticFindObject(UClass, ANY_PACKAGE, ImportName)`. Handles
   261 rollups on Fort Frolic.
2. **Pre-computed rollup map** (`rollup.txt`). Generated from Bioshock `.U`
   packages. Loaded on first use from the same directory as the `.bsm`. Maps
   each Bioshock-specific class to its nearest UE2.5-native ancestor.
   Handles classes whose super chain passes through unresolvable imports.
   1,121 total rollups on Fort Frolic; 52 remaining map-only debris classes
   are skipped.

**Abstract class bypass.** `CLASS_Abstract` should be temporarily cleared
during `StaticConstructObject` for v141 rollup objects. Abstract is a
gameplay constraint, not structural — safe for editor-only loading.

**Tagged-property tolerance.** Properties the UE2.5 ancestor class knows
about (Location, Rotation, DrawScale, StaticMesh, …) load normally. Unknown
Bioshock-specific properties are skipped via the size in the info byte
(B.3). Serial boundary protection (D.5) handles any overreads.

#### D.2.1 `rollup.txt` format

One mapping per line: `BioshockClass=UE25AncestorClass`. Generated by parsing
all decompressed `.U` packages and walking the class hierarchy (3,733 classes
total).

### D.3 Native shells for Bioshock-specific classes

Some Bioshock-specific classes can't safely go through rollup because the
type-safety violations they introduce (D.5) crash at downstream sites that
can't be defensively filtered. A loader can register **empty native UClass
shells** for those classes so that `StaticFindObject(UClass, ANY_PACKAGE,
"ClassName")` resolves to the native class instead of falling through to
rollup. The resulting object has correctly-paired `obj->Class` field and
vtable.

#### D.3.1 Material shells

Surf-facing Bioshock material classes — i.e., the ones that can appear on
`FBspSurf.Material` directly:

```cpp
// UnBioshockMaterial.h
class ENGINE_API UFluidShader        : public URenderedMaterial
    { DECLARE_CLASS(UFluidShader,        URenderedMaterial, 0, Engine) };
class ENGINE_API ULayeredShader      : public URenderedMaterial
    { DECLARE_CLASS(ULayeredShader,      URenderedMaterial, 0, Engine) };
class ENGINE_API UFluidSurfaceShader : public URenderedMaterial
    { DECLARE_CLASS(UFluidSurfaceShader, URenderedMaterial, 0, Engine) };
```

Matching `IMPLEMENT_CLASS` in the matching `.cpp`. No fields, no virtual
overrides — a FF-D3D renderer cannot evaluate Bioshock's real shader
semantics, and the BSP/mesh material flatten passes (D.4) NULL these out or
extract a primary diffuse UTexture before the render path ever dispatches a
virtual on them. Tagged-property deserialization skips the Bioshock-specific
properties via the unknown-property path.

**Selection criteria:** classes that appear directly on FBspSurf.Material,
UStaticMesh.Materials, or UActor.Skins across the 9-map corpus:

| Class | Surf instances | Notes |
|---|---|---|
| Shader | 11,751 | UE2.5 native |
| Texture | 2,809 | UE2.5 native |
| **FluidShader** | 104 | **Bioshock-only — needs shell** |
| MaterialSwitch | 101 | UE2.5 native (inheritance differs but tolerant) |
| **LayeredShader** | 11 | **Bioshock-only — needs shell** |
| **FluidSurfaceShader** | 4 | **Bioshock-only — needs shell** |

**Deferred (Tier 2):** `FacingShader`, `LightBeamShader`, `PlantShader`,
`TargetedShader`, `CausticShader`, `ShimmerShader` — appear in mesh materials
and nested shader sub-refs but never on surfs directly. Defer until a
concrete crash motivates them.

**Deferred (Tier 3):** `BitmapMaterial` descendants (`ScriptedTexture`,
`FlashTexture`, `ProxyBitmapMaterial`) — zero usage in tested maps.

#### D.3.2 Emitter shells

Bioshock-specific particle emitter subclasses that appear inside
`AEmitter.Emitters[]`:

```cpp
// UnBioshockEmitter.h
class ENGINE_API URibbonEmitter         : public UParticleEmitter
    { DECLARE_CLASS(URibbonEmitter,         UParticleEmitter, 0, Engine) };
class ENGINE_API UMultipleRibbonEmitter : public UParticleEmitter
    { DECLARE_CLASS(UMultipleRibbonEmitter, UParticleEmitter, 0, Engine) };
```

30–35 RibbonEmitter and 37–44 MultipleRibbonEmitter instances per map across
the corpus. Without the shells, the rollup path leaves them with mismatched
`Class`/vtable pairs and the renderer's `RenderParticles` virtual call
indirects into the wrong vtable slot (typical access violation reading
`NULL + 0x1A8` inside whatever function it accidentally invoked).

**Field bodies are not RE'd.** The shells deserialize their tagged properties
into the unknown-property skip path. Particles render as no-ops (the base
`UParticleEmitter::RenderParticles` is a no-op for particle types the FF
renderer doesn't know how to render). For visual fidelity work, the property
bodies need to be disassembled and documented (E.3).

**Non-goals:** `DamageEmitter`, `ReactionStopEmitter`,
`ReactionStopAllAttachedEmitters` are `ShockGame`/`VengeanceShared` gameplay
action classes, not `UParticleEmitter` subclasses. They can't appear inside
`AEmitter.Emitters[]` and don't need shells.

### D.4 Material flattening (Shader → Texture)

UE2.5's fixed-function D3D renderer cannot evaluate Bioshock's multi-pass
material trees (UShader sub-materials: Diffuse, Opacity, Specular,
SpecularityMask, SelfIllumination, SelfIlluminationMask, Detail). A post-load
pipeline should replace every Shader reference with its primary `Diffuse`
UTexture so the FF renderer can display a plausible flat texture.

Three flattening passes (in order):

#### D.4.1 UShader sub-material sanitization

Walk all UShader instances. For every shader:

- NULL `Opacity`, `Specular`, `SpecularityMask`, `SelfIllumination`,
  `SelfIlluminationMask`, `Detail`, `FallbackMaterial`.
- Clear `UseFallback`.
- Validate `Diffuse` with `IsA(UTexture::StaticClass())`; NULL if it's not a
  UTexture, OR if the underlying texture has zero mips (`Mips.Num() == 0`).

Rationale: keeping only Diffuse gives Texture Browser thumbnails without
PreSetMaterial recursion hazards. A UE2.5 FF D3D renderer can't use any of
the 6 sub-fields regardless of Bioshock's original intent.

On Fort Frolic, **181 of 314 shaders (58 %)** had at least one sub-material
field populated and required sanitization.

#### D.4.2 BSP surf flattening

Walk all FBspSurfs in `Level->Model->Surfs`:

- If `Material` is a `UShader`, follow `Diffuse` to get a UTexture.
- Replace surf Material with the UTexture directly.
- Clear Material to NULL for textures with no Mips (prevents renderer AV).
- Apply the type-safety filter (D.5) — NULL the slot if the Material's
  `Class` field is a UMaterial subclass but its actual layout is
  UClass/UStruct.

Call `ClearRenderData` afterward to force a rebuild with the new materials.

#### D.4.3 Mesh material flattening

Walk all UStaticMesh instances. For each mesh's `Materials` array:

- `UShader` → replace with `Shader->Diffuse` if it's a UTexture with
  `Mips.Num() > 0`; otherwise NULL.
- `UTexture` with mips → keep.
- Anything else → NULL.

Apply the type-safety filter (D.5).

#### D.4.4 Actor `Skins` filter

`UStaticMesh::GetSkin` first checks `Owner->GetSkin(Index)` which returns
`Actor->Skins(Index)`; only if that's NULL does it fall back to
`Mesh->Materials`. Apply the type-safety filter to every entry in
`Actor->Skins` in the actor-wiring loop, NULLing corrupt entries so
`GetSkin` falls through to the (already-sanitized) `Mesh->Materials` slot.

### D.5 Type-safety filter for downstream sites

The rollup path (D.2) can leave objects in `GObjObjects` where `obj->Class`
is some UMaterial-subclass (or UParticleEmitter-subclass) but `obj`'s actual
memory layout is UClass/UStruct-derived. Virtual dispatch then lands in
`UStruct::SerializeExpr` with a junk archive (`Ar.ArVer = -653399808` was
observed). Exact mechanism still not fully root-caused (eliminated for
surf-facing material/emitter classes by D.3 shells, but other paths can still
exhibit the pattern).

**Three guards** — apply before any virtual call on a candidate object pulled
from `GObjObjects` / `TObjectIterator`:

```cpp
inline UBOOL IsSafeBioshockMaterial(UObject* Obj)
{
    return Obj
        && !Obj->IsA(UField::StaticClass())                       // not a meta-object
        && Obj->GetClass()->ClassConstructor != NULL              // class properly loaded
        && !(Obj->GetClass()->GetFlags() & RF_NeedLoad);          // class not marked NeedLoad
}
```

Applied at:
- Texture Browser collection + immediately before `SetMaterial`.
- BSP surf flattening (D.4.2).
- Mesh material flattening (D.4.3).
- Actor `Skins` filter (D.4.4).

Filtered objects are NULLed at their slot. `GetSkin` falls back to
`DefaultMaterial`; surfs render with the texture browser's default. Editor
remains responsive.

### D.6 Serial boundary protection

When tagged-property parsing goes off the rails (mismatched class, wrong type
on a property), reads can run past the export's declared `SerialOffset +
SerialSize` and either crash on FArchiveFileReader's fatal EOF or pull
unrelated bytes from the next export.

**Three-part defense:**

1. **Boundary tracking variables** — set before each v141 object's
   `Serialize` to `SerialOffset + SerialSize`. **Overread flag** — set on
   the first boundary violation. All subsequent reads inside this object
   **zero-fill** without touching the Loader (until the flag is cleared on
   the next object's serialize entry). Both saved/restored across nested
   Preloads.
2. **Route reads through `Serialize`.** Change `*Loader << AR_INDEX(...)` to
   `Self << AR_INDEX(...)` in both the FName and UObject operators so reads
   go through `ULinkerLoad::Serialize` and hit the boundary check.
3. **Garbage-size guard.** In `SerializeTaggedProperties`, break when
   `Tag.Size > 1 MB` for v141 (boundary hit mid-tag produces garbage sizes,
   causing a multi-billion iteration skip loop) — see B.3.

**Trade-off:** the zero-fill behavior masks original misalignments for a
long time (B.2 was a notable example). Don't rely on the safety net for
parser correctness — it's a crash-avoidance backstop, not a correctness
guarantee.

---

## Part E — Known divergences, gaps, and incompatibilities

### E.1 SkeletalMesh — gap

Not yet RE'd. Bioshock SkeletalMesh uses Havok 4.1.0 packfile format for
skinning data, which is a binary container distinct from UE2.5's
`USkeletalMesh::Serialize`. Blocks character rendering. UEViewer has a
working parser at `Unreal/UnMesh3.cpp` (Bioshock branch) — the most likely
starting point.

### E.2 Havok rigid-body / physics — gap

Not yet RE'd. `dyn_*` props (C.7) ship with Havok rigid-body data referenced
via the sidecar's `HavokDataClass` field (e.g.,
`HavokPhysics.Aluminum10pc`). Without parsing this, `dyn_*` props are static
— they appear at their designer placement but don't react to forces or
collide realistically. Bioshock used Havok 4.1.0; the on-disk format is the
Havok packfile binary (not text). UEViewer has Havok parsing for related
games.

### E.3 Particle subclass property bodies — partial

`URibbonEmitter` and `UMultipleRibbonEmitter` can exist as native shells
(D.3.2) that allow particle actors to load without crashing, but their
Bioshock-specific properties are not RE'd. Particles render as no-ops. To
restore visual fidelity, disassemble `RibbonEmitter::Serialize` /
`MultipleRibbonEmitter::Serialize` and document the property list.

The Tier 2 / Tier 3 material classes (D.3.1 — `FacingShader`,
`LightBeamShader`, `PlantShader`, `TargetedShader`, `CausticShader`,
`ShimmerShader`, `ScriptedTexture`, `FlashTexture`, `ProxyBitmapMaterial`)
are in the same boat — deferred until a concrete crash motivates a shell,
and full property bodies not RE'd at all.

### E.4 Material modifier classes

`MaterialColorFromLight`, `MaterialAnimatedColor`, `MaterialAnimatedFloat`,
`MaterialBool`, `MaterialCascadingWater` — extend `UObject` rather than
`UMaterial`, so no surf risk and no shell needed. Shipped binary format
unknown; appear to be transient at runtime in Bioshock.

### E.5 Why `FName == FName` fails for inherited properties

The Bioshock linker's `NameMap` and a native UE2.5 `FName(TEXT(...),
FNAME_Add)` registration end up with different name-table indexes for some
properties. For example: `LightBrightness` on a rolled-up Bioshock Light
class fails the `Node->GetFName() == Tag.Name` lookup even though both
sides print the same string.

A string-compare fallback after the FName lookup (B.3) works in practice.
The root cause has not been chased — the most likely culprit is the Bioshock
linker's name-loading path producing entries that don't match the native
`FNAME_Add` interning. Worth a debugger session comparing
`Bioshock_LightBrightness_FName.GetIndex()` to
`Native_LightBrightness_FName.GetIndex()` if you find a property inheritance
issue not covered by the string fallback.

### E.6 ~12 % tagged-property parse failure

2,319 of 19,949 non-stack objects in the test corpus fail full property
parsing (B.3.1). Doesn't block map loading. Likely root causes:
Bioshock-specific property types in ArrayProperty containers, class-specific
binary payloads following the property block where the parser keeps reading,
or RF_HasStack flag missed on a handful of objects.

### E.7 PackageFlags `0x00360000` / `0x00270000` bit meanings

Only the `0x00200000` bit is identified (compression). Other bits in
`0x00360000` (maps) and `0x00270000` (scripts) are not yet decoded. Doesn't
block loading.

### E.8 SWAT 4 incompatibility

SWAT 4 (Epic 129) UnrealEd cannot load Bioshock packages even with the
format deltas above patched in:

```
ReadFile failed: Count=0 BufferCount=-35233896 Error=Invalid access to memory location.
History: ULinkerLoad::Serialize <- FCompactIndex<< <- FString<< <- FNameEntry<<
         <- LoadNames <- ULinkerLoad::ULinkerLoad
```

What happens:
1. SWAT 4 reads the header correctly (identical format).
2. SWAT 4 seeks to `NameOffset = 79,435,974` in the raw file.
3. The file is only 44.2 MB. SWAT 4 doesn't know the data is compressed.
4. `BufferCount = Min(..., Size - Pos)`. Since `Pos (79M) > Size (44M)`,
   `BufferCount` underflows: `-35,233,896`.
5. Windows `ReadFile` treats this as a ~4 GB DWORD and fails.

Even with decompression, SWAT 4 would fail because it reads FString with
positive=ANSI (producing garbage from Unicode data), reads 4-byte FNameEntry
flags (causing every subsequent name to misalign), and has no FName
instance number support.

### E.9 UE3 mainline differences

Quick reference for anyone porting between Bioshock and 2012 UE3:

| Feature | Bioshock | 2012 UE3 |
|---|---|---|
| Header trailing fields | None | EngineVersion, FolderName, DependsOffset, etc. |
| Compression | Custom zlib chunks | `PKG_StoreCompressed` + `FCompressedChunk` |
| FString sign | +Unicode/−ANSI | +ANSI/−Unicode |
| FNameEntry flags | QWORD | DWORD |
| FName | CompactIndex + INT32 Number | INT32 NameIndex + INT32 Number |
| Object index | CompactIndex | INT32 |
| Export field order | Archetype before ObjectName | Archetype after ObjectName |
| Per-object header | 8 B Vengeance `(check, sub_ver)` | None |
| State frame | Vengeance, ~46–52 B | StateStack TArray |
| FPropertyTag | UE2.5 packed info byte | FName-based |
| Bulk data | TLazyArray + external `.blk` | FUntypedBulkData |

### E.10 Remaining noise during load

Not blocking; documented in case it shows up during debugging:

- ~300 `Warning: Skipping export … no native ancestor` for Bioshock
  decorative props (`dyn_crate`, `dyn_bible`, `dyn_Rake`,
  `SmugglersCrateOpen`, `ShojiDoors`, `ScienceDoors`, `Lockbox`,
  `KeypadWallsafe`, …). These don't render. Most resolve to one-line
  `rollup.txt` additions (`dyn_crate=Decoration`, `ShojiDoors=Door`, etc.).
- ~40 `Warning: Missing Class Class Engine.*` for Bioshock-only classes
  whose references appear in import tables: `AnimationPackageWrapper`,
  `HkMeshProxy`, `SharedSkeletonAnimationMetadata`, several `Havok*`,
  effect classes (`DoubleVisionEffect`, `DepthOfFieldEffect`,
  `GlowSettings`, `ColorCycle`), `FluidVolume`, `CubemapProbe`,
  `CausticShader`, `FacingShader`, various `AnimNotify_*`, `ActorGroup`,
  `TexturePanner`, `PlayerPathNode`, `PathBlockingVolume`. Native-shell
  fixes in the D.3 shape are the playbook if any surface a crash.
- ~293 `Warning: Type mismatch in Opacity of Shader` — Bioshock's
  `Shader.Opacity` has a different field type than UE2.5's
  `UShader::Opacity` (`UMaterial*`). The tagged-property deserializer skips
  the field correctly, and D.4.1 sanitization NULLs the slot anyway.
- CSG rebuild cleanup warnings (`FPoly::Fix: Collapsed a point`,
  `bspAddNode: Infinitesimal polygon`, `SplitWithPlane: Ignored sliver`) —
  normal numerical-precision cleanup when rebuilding Bioshock brush
  geometry through UE2.5's tolerances.

---

## Appendix — Validation evidence

| Validator | What it tests | Coverage |
|---|---|---|
| Full package parser | Header, decompress, name/import/export tables, tagged properties on all exports | Reference parser; canonical for table layouts |
| Decompressor | Zlib chunk decompression | All 9 maps + all `.U` packages decompress losslessly |
| Format-hypothesis comparator | 9 format hypotheses: header, FString convention, FNameEntry flags size, FName encoding, export field encoding, 8 B prefix, state frame, tagged properties, object refs | Establishes Bioshock differs from both UE2.5 and UE3 mainline on the documented points |
| Export-field probe | Deep analysis of the "Unknown" export field, 8 B prefix, FNameEntry QWORD flags. Cross-checks `.U` script packages for format consistency | Resolved the QWORD-ObjectFlags-vs-DWORD-Unknown ambiguity |
| BSP validator | UModel/BSP layout: FBspNode (100 B), FBspSurf (52 B + 8 B Vengeance hdr), FVert (8 B), FLeaf (12 B), FZoneProperties (128-bit), Bounds (FBox 25 B), LeafHulls (INT32), full UModel::Serialize field order | **283/284 EXACT BYTE MATCH** on `0-Lighthouse.bsm` |
| Polys validator | UPolys + FPoly format | **284/284 byte-exact** on Lighthouse (1,687 FPolys); **1,312/1,312 byte-exact** on Fort Frolic (6,940 FPolys); all field-validity checks pass on 8,627 elements |
| StaticMesh probe | UStaticMesh Vengeance layout: header + sections + bounds + verts + UV streams + index buffer | **100 % validation** on every UStaticMesh in every tested map |
| Catalog parser | `Catalog.bdc` parser: name-to-bulk-entry index | Powers texture loading; validated by successful texture rendering on all 9 maps |
| Class hierarchy extractor | Class hierarchy extraction from `.U` packages | 3,733 classes mapped → `rollup.txt` |
| dyn_* defaults extractor | `dyn_*` class default extraction via UELib `BulkDecompile` | 177/185 (96 %) coverage across 9-map corpus → `dyn_defaults.txt` |
| EOF simulator | Simulates the C++ object serialization path to detect EOF/overread conditions | Catches D.6 boundary violations during parser development |

### Per-map load stats (Fort Frolic)

Loading `5-Hephaestus.bsm` (Fort Frolic) with the full set of fixes:

- 6,734 BSP nodes
- 3,195 surfs
- 125,544 verts
- 1,470 textures populated
- 2,352 Shader→Texture flattenings
- 1,121 class rollups
- 9,286 actors placed at their real Location/Rotation
- 181 UShader objects sanitized
- 610 UStaticMesh objects deserialized (411,235 verts / 1,131,573 indices total)
- 163/168 dyn_* instances mesh-assigned
- Texture Browser stable post-load
- `Build → Lighting` produces working interior lighting

Load + render confirmed working across **Arcadia, Market, Fort Frolic,
Hephaestus, Lighthouse, Medical, Welcome, Ryan, Resi** on both Windows XP and
Windows 11, Debug and Release builds, with and without a debugger attached.