# Reading BioShock 1 Materials & Shaders — Format Specification

A guide for the material and shader system of BioShock 1 (the
"Vengeance" engine — Irrational's fork of Unreal Engine 2.5).

Materials and shaders are **three separate layers**, and it helps to keep them apart from the
start:

1. **Material objects** live inside packages as ordinary UE2 objects (tagged properties). A
   material is a *description* — "diffuse = this texture, specular color = that, two-sided,
   masked" — not a program.
2. **Shader programs** are HLSL "material factories" that ship as **source** in a separate
   pack file, plus their **compiled permutations** in cache files. The factory is the actual
   GPU program; the material only names which factory it wants and supplies the parameters.
3. **The binder** is the glue: at draw time the engine takes a material's texture/scalar
   properties and binds them into the chosen factory's sampler slots and constant registers.

This doc covers all three. Texture pixel data itself (the `UTexture` mip/`.blk` format) is a
separate document — materials only *reference* textures by object reference.

> **Decode status.** Layers 1 and 3 and the shader **source pack** are fully understood. The
> **compiled** shader caches are characterized (you can identify and slice them) but their
> exact record framing was not needed and is not fully reversed — called out in §6.

---

## Conventions

Standard UE2 primitives, unchanged in BioShock unless noted:

- **`CI`** — `FCompactIndex`, the variable-length signed int (1–5 bytes).
- **`FString`** — length-prefixed string; a negative length flags UTF-16, positive flags ANSI.
- **`FName`** — interned name, on the wire as two `CI`s `{ index, number }`.
- **objref** — a `CI` reference into the package's export/import table; `0` = none.
- **Tagged properties** — the normal UE2 name/type/size-tagged property block every `UObject`
  serializes. Materials use **only** this; there is no custom material binary blob.

All integers little-endian (PC build).

---

## 1. The material class tree

Every material derives from the abstract base `Material` (`UMaterial`). The tree splits into
three functional branches plus modifiers:

```
Material (abstract)
├── Modifier               wrappers that adjust another material
│   ├── FinalBlend             frame-buffer blend / Z control over a child
│   ├── TexModifier / *        UV + color modifiers
│   └── MaterialSequence/Switch  animated / selectable sub-material lists
├── RenderedMaterial       things that actually shade a surface  (see §3)
│   ├── Shader                 the main lit surface shader
│   └── *Shader                specialised shaders (Fluid, Layered, Facing, Window, …)
└── BitmapMaterial         image sources
    ├── Texture                a sampled 2D texture (the UTexture format)
    └── Cubemap                6-face cube
```

### 1.1 The base `Material` fields

`Material` carries the fields common to the whole tree. The two that matter for decoding:

| Field | Type | Meaning |
| --- | --- | --- |
| `MaterialType` | byte enum `EMaterialType` | **the runtime type tag** — identifies what kind of material this is without RTTI (see below) |
| `MaterialVisualType` | byte enum `EMaterialVisualType` | physical-surface class (Stone, Glass, Flesh, Water, …) — drives footstep/impact/decal selection, **not** rendering |
| `DefaultMaterial` | objref | fallback material (defaults to `Engine.Engine_res.DefaultTexture`) |
| `AcceptProjectors` | bool | receives decal/projector passes |
| `Keywords` | array&lt;name&gt; | editor tags |

`EMaterialType` is a flat ordinal that the engine uses as a fast class discriminator. The
ordinals are fixed and worth having verbatim, because the `*Start`/`*End` sentinels let the
engine test "is this a RenderedMaterial?" as a single range check:

```
 0 Material            9 Shader              18 PlantShader        27 ScriptedTexture
 1 MaterialSequence   10 FluidShader         19 WindowShader       28 TextureStart
 2 MaterialSwitch     11 FluidSurfaceShader  20 PondScumFluidSh.   29 Texture
 3 ModifierStart      12 TerrainMaterial     21 ShimmerShader      30 Cubemap
 4 Modifier           13 ScriptSideShader    22 TargetedShader     31 TextureEnd
 5 FinalBlend         14 BlenderShader       23 CausticShader      32 BitmapMaterialEnd
 6 ModifierEnd        15 LayeredShader       24 RippleShader       33 RenderedMaterialEnd
 7 RenderedMatStart   16 FacingShader        25 BitmapMaterialStart
 8 RenderedMaterial   17 LightBeamShader     26 BitmapMaterialStart...
```

(e.g. a material is "rendered" if `RenderedMaterialStart < MaterialType < RenderedMaterialEnd`.)

---

## 2. On-disk format: materials are plain tagged-property objects

This is the key simplification: **a material has no custom binary serialization.** It is a
standard UE2 object — object header + tagged property block — and nothing else. To read a
material you only need a working UE2 tagged-property parser; there is no per-class blob to
reverse like there is for meshes or textures.

Consequences for a reader:

- Every interesting value (diffuse texture, colors, flags, blend mode, masks) is a **named
  tagged property**. Read them by name; you do not need to know a struct layout.
- **Texture references are objrefs**, resolved through the package's import/export table the
  normal way. The referenced object is a `Texture`/`Cubemap` (`BitmapMaterial`), whose pixel
  data follows the separate texture format.
- **Nested materials** (masks, modifiers, animated sub-materials) are either objrefs to other
  exports or **inline subobjects** (`editinline`), again handled by the stock UE2 object/
  subobject machinery — no BioShock-specific framing.

So: parse object → read tagged properties → follow objrefs. That is the entire material
"format." The complexity is not in the bytes; it is in *what the properties mean*, which §3–§4
cover.

### 2.1 The `Shader` material — the important property set

`Shader` (`MaterialType == 9`) is the workhorse lit material. Its tagged properties are the
parameter set fed to the `MaterialFactory_Shader` program (§4). The notable ones:

| Property | Type | Role |
| --- | --- | --- |
| `Diffuse` | Material (objref) | base color map |
| `DiffuseColor` | Color | diffuse tint (default white) |
| `NormalMap` | Material | tangent-space normal map |
| `SpecularColor` / `SpecularColorMap` / `SpecularMask` / `GlossinessMask` | Color / Material / MaskMaterial | specular inputs |
| `Emissive` / `EmissiveColor` | Material / Color | self-illumination |
| `Subsurface` / `SubsurfaceColor2x` / `SubsurfaceMask` | Material / Color / MaskMaterial | subsurface term |
| `Opacity` / `Masked` / `OutputBlending` | MaskMaterial / bool / enum | alpha & blend mode |
| `ClipMask` + clip values | MaskMaterial | alpha-test clipping |
| `ReflectionCubemap` / `ReflectionMask` / `ReflectionBrightness` / `UseSpecularCubemaps` | Cubemap / Mask / byte / bool | environment reflection |
| `NormalMap`, `HeightMap` | Material | bump / parallax |
| `TwoSided`, `ForceTransparentSorting`, `DistortionStrength` | bool / byte | misc render state |

A **`MaskMaterial`** is a small atomic struct: `{ Material Material; EMaskChannel Channel }`
— i.e. "sample this texture, take channel A/R/G/B." This is how a single grayscale-in-one-
channel texture drives opacity, gloss, specular masks, etc.

`RenderedMaterial` (the parent of every `*Shader`) adds one field that bridges to the program
layer: **`ShaderTag`** (a `name`). See §4.

---

## 3. The runtime shader system

BioShock's renderer is a programmable DX9 SM3 pipeline (plus a DX10 path), not UE2's stock
fixed-function/`xShader` system. Surfaces are drawn by **material factory** programs written in
HLSL. The mapping from a material object to a program is by **name**, via `ShaderTag`.

### 3.1 `ShaderTag` → factory

Every `RenderedMaterial` defines `ShaderTag` in its defaults, e.g. `Shader` sets:

```
ShaderTag = "MaterialFactory_Shader"
```

That tag is the base name of an HLSL source file (`MaterialFactory_Shader.hlsl`). Each material
*class* maps to one factory:

| Material class | `ShaderTag` / factory |
| --- | --- |
| `Shader` | `MaterialFactory_Shader` |
| `BitmapMaterial` (direct draw) | `MaterialFactory_BitmapMaterial` |
| `BlenderShader` | `MaterialFactory_Blender` |
| `LayeredShader` | `MaterialFactory_Layered` |
| `FacingShader` | `MaterialFactory_Facing` |
| `FluidShader` / `FluidSurfaceShader` | `MaterialFactory_Fluid` / `_FluidSurface` |
| `WindowShader`, `PlantShader`, `CausticShader`, `RippleShader`, `ShimmerShader`, `TargetedShader`, `LightBeamShader`, `PondScumFluidShader` | `MaterialFactory_<Name>` |
| (script-driven) | `MaterialFactory_ExampleScriptShader` |

### 3.2 What a factory looks like

A factory is a set of small HLSL functions the renderer calls to produce each surface term —
`materialDiffuse`, `materialNormal`, `materialAlpha`, `materialSpecularColor`,
`materialEmissive`, etc. — plus a per-vertex `materialGenerateData`. The engine supplies the
lighting/shadowing scaffolding (`PixelData`, `VertexData`, the lighting passes); the factory
only fills in the material-specific look. Defaults for any term a factory doesn't need come
from `MaterialFactory_Template.hlsl` (`DEFAULT_DIFFUSE`, `DEFAULT_NORMAL`, …).

`MaterialFactory_BitmapMaterial.hlsl` is the minimal example — diffuse + alpha straight from
one texture. `MaterialFactory_Shader.hlsl` is the full lit material consuming the whole
`Shader` property set from §2.1.

---

## 4. The texture binder (`mtb`) — how properties reach the GPU

The factories never hard-code which sampler holds the diffuse map. Instead BioShock uses a
**material texture binder** abstraction (`mtb*`, `MaterialTextureBinder.hlsl`):

- The shader declares **12 generic samplers** (`mtbSampleSlot1..12`).
- Each texture input is a small **bind-info literal**:
  - `mtbBindInfo { int samplerSlot }` — which of the 12 slots the texture is in (`0` = unbound).
  - `mtbMaskBindInfo { int samplerSlot; int channelID }` — same, plus which channel (A/R/G/B)
    for a `MaskMaterial`.
- Sampling helpers (`mtbSampleRGB`, `mtbSampleMask`, `mtbSampleNormal`, `mtbSampleRGBCube`, …)
  switch on `samplerSlot` to read the right sampler, and **return a default when the slot is
  `0`** — so an unbound input costs nothing and contributes its neutral value.

At runtime the engine, for a given material instance:

1. Decides which of the material's texture properties are present.
2. Assigns each present texture to a concrete sampler slot (1–12) and sets that sampler state.
3. Specializes the factory by giving each bind-info literal its real value (slot + channel),
   and uploads the scalar/color constants (`diffuseColor`, `specularPower`, the tex-transform
   matrices, etc.).

### 4.1 Permutation literals (`LITERAL` / `ULITERAL`)

The specialization in step 3 is done through the `LITERAL`/`ULITERAL` macros (defined in
`Common.hlsl`):

```hlsl
// DX9 path:
#define LITERAL(_type, _name, _default)  const _type _name = _default;
// DX10 path:
#define LITERAL(_type, _name, _default)  static _type _name = _default;
```

A factory declares, e.g. `LITERAL(mtbBindInfo, DiffuseBind, {0});`. The default `{0}` means
"unbound." When the engine compiles the permutation for a specific material, it overrides those
literals with the real bind values (and, on the DX10 path, injects them via the
`DEFINE_LITERALS` hook). Because the bind-info is a compile-time constant, the big
`if (samplerSlot == N)` ladders in the binder collapse to a single texture fetch — every
material/texture-set combination becomes its own tiny optimized shader. That is why there are
*many* compiled permutations per factory (§6).

---

## 5. Shader source pack — `shaders.spk` (fully decoded)

The HLSL factory **source** ships in one flat pack file, `shaders.spk` (alongside the game
binary). It is a simple concatenation of named source files:

```
ShaderPack:
    i32   Version        (== 1)
    i32   FileCount      (== 40 on retail)
    i32   Flags          (== 1)
    i32   Cooker         (cooker hash / timestamp)
    Entry[FileCount]:
        i32    NameLen    number of UTF-16 code units in the name
        WCHAR  Name[NameLen]      e.g. "AmbientDistortion.hlsl"  (UTF-16, no NUL)
        i32    SourceLen  byte length of the source text
        char   Source[SourceLen]  the raw HLSL (ANSI, with #includes)
```

The names are the `.hlsl` filenames; the bodies are the literal HLSL (the includes like
`StandardInputs.hlsl`, `MaterialTextureBinder.hlsl`, `Common.hlsl` are pack entries too, pulled
in by `#include`). Walk `FileCount` entries to recover the entire shader source tree. Re-reading
the pack and listing its 40 entries reproduces the full factory + include set exactly.

---

## 6. Compiled shader caches — `ShaderCache.pcs` / `.pcs10` (characterized)

The engine compiles each needed factory **permutation** (per material/texture-set, per the
`LITERAL` specialization in §4) and caches the resulting GPU bytecode so it doesn't recompile
at load. Two caches ship:

| File | Contents |
| --- | --- |
| `ShaderCache.pcs`   | **Direct3D 9** compiled shaders (SM3 — `vs_3_0`/`ps_3_0`). Each blob is the classic D3D9 bytecode: a version token (`\x00\x03\xFE\xFF` etc.) followed by a `CTAB` constant table. |
| `ShaderCache.pcs10` | **Direct3D 10** compiled shaders — `DXBC`-container bytecode (each blob starts with the literal magic `DXBC`). The DX10 render path. |

Both are keyed by a **permutation name string** built from the source factory and the entry
point, e.g. `ImageSpace.hlslBlurVertexShader` (UTF-16, with a leading discriminator). Each
record is `name + compiled bytecode blob`, concatenated; there is one record per compiled
permutation, which is why these files are large (tens of MB) — hundreds of specialized variants
per factory.

> **What's not reversed:** the exact per-record framing of the `.pcs`/`.pcs10` caches (the
> integer header before each name, length fields, any flags) was not needed for understanding
> the material→shader pipeline and is **not** fully decoded here. If you need to *extract*
> individual compiled shaders, you can slice on the recognizable blob magics (`CTAB`-bearing
> D3D9 version tokens, or `DXBC`) rather than trusting a record header. For *understanding* or
> *re-deriving* a material's look, prefer the HLSL **source** in `shaders.spk` (§5) — the cache
> is just a compile artifact of it.

---

## 7. Reading a material, end to end

To understand how material *M* shades a surface:

1. Parse *M* as a standard UE2 object; read its **tagged properties** (§2). No custom blob.
2. Read `MaterialType` to know the class; if it's a `Modifier`/`FinalBlend`/`Sequence`/`Switch`,
   follow it to the wrapped child material(s).
3. For a `RenderedMaterial`, read **`ShaderTag`** → the `MaterialFactory_<Tag>` program (§3).
4. Read the material's texture properties as **objrefs**; each resolves to a `Texture`/`Cubemap`
   whose pixels come from the texture/`.blk` format. Read scalar/color properties directly.
5. The factory's `material*` functions, fed those textures (via the `mtb` sampler slots) and
   constants, produce the surface terms. The source is in `shaders.spk` (§5); the precompiled
   GPU variant is in the `ShaderCache.*` files (§6).

---

## 8. Summary

- **Materials are plain tagged-property UE2 objects** — no custom binary format. Read
  properties by name and follow objrefs. `MaterialType` is the class discriminator;
  `MaterialVisualType` is gameplay surface class, not rendering.
- **Materials are descriptions; shaders are programs.** A `RenderedMaterial`'s `ShaderTag`
  names its HLSL **material factory** (`MaterialFactory_<Tag>`).
- **The `mtb` binder** maps a material's texture/mask properties to 12 generic sampler slots;
  `LITERAL`/`ULITERAL` specialize each factory into a per-material permutation at compile time.
- **Shader source** ships fully in `shaders.spk` (simple `Version/Count/[name+source]` pack,
  decoded above). **Compiled permutations** sit in `ShaderCache.pcs` (D3D9) and `.pcs10`
  (D3D10 `DXBC`) — identifiable and sliceable, record framing not fully reversed.
</content>
