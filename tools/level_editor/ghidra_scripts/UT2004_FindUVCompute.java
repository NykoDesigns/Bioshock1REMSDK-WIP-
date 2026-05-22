// Find BSP UV/texture coordinate computation in UT2004 Engine.dll
// UE2 computes UVs from: TextureU/V vectors (from FBspSurf), vertex position, and PanU/PanV.
// Formula: U = dot(vertex - base, TextureU) / texWidth + PanU
//          V = dot(vertex - base, TextureV) / texHeight + PanV
//
// We're looking for functions that:
//   1. Access FBspSurf fields (vTextureU at +offset, vTextureV, pBase)
//   2. Do dot products with vertex positions
//   3. Divide by texture dimensions
//   4. Apply PanU/PanV offsets
//
// FBspSurf standard UE2 layout (fixed-size, ~64B):
//   +0x00: UMaterial* Material (pointer, 4B in 32-bit)
//   +0x04: DWORD PolyFlags
//   +0x08: INT pBase      - index into Points
//   +0x0C: INT vNormal    - index into Vectors
//   +0x10: INT vTextureU  - index into Vectors
//   +0x14: INT vTextureV  - index into Vectors
//   +0x18: INT iBrushPoly
//   +0x1C: AActor* Actor  (pointer, 4B)
//   +0x20: FLightMapIndex LightMap (variable)
//   +0x??: INT PanU, PanV
//
// Also search for the Vectors/Points array access pattern:
//   Vectors[surf.vTextureU] * 12 to get float3
//
// @category BSP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class UT2004_FindUVCompute extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        
        println("=== UT2004 Engine.dll UV Computation Finder ===");
        println("Looking for patterns: IMUL*0x0C (12-byte vector stride) + dot products...\n");
        
        Set<Function> candidates = new HashSet<>();
        Address searchAddr = mem.getMinAddress();
        Address maxAddr = mem.getMaxAddress();
        
        // Pattern: IMUL by 0x0C (12 bytes = sizeof(FVector)) - used to index Vectors/Points arrays
        int found = 0;
        while (searchAddr != null && searchAddr.compareTo(maxAddr) < 0 && found < 500) {
            searchAddr = mem.findBytes(searchAddr, maxAddr, new byte[] { 0x6B }, null, true, monitor);
            if (searchAddr == null) break;
            try {
                byte imm = mem.getByte(searchAddr.add(2));
                if (imm == 0x0C) {
                    Function func = listing.getFunctionContaining(searchAddr);
                    if (func != null) candidates.add(func);
                    found++;
                }
            } catch (Exception e) {}
            searchAddr = searchAddr.add(1);
        }
        
        println("Found " + candidates.size() + " functions with IMUL*0x0C (FVector stride)\n");
        
        // Decompile and look for UV-related patterns
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        
        int decompCount = 0;
        for (Function func : candidates) {
            if (decompCount >= 25) break;
            long size = func.getBody().getNumAddresses();
            if (size < 100 || size > 8000) continue;
            
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result == null || result.getDecompiledFunction() == null) continue;
            
            String code = result.getDecompiledFunction().getC();
            
            // Score based on UV computation indicators
            int score = 0;
            boolean hasMul12 = code.contains("* 0xc") || code.contains("* 12") || code.contains("*0xc");
            boolean hasFloatMul = code.contains("fmul") || code.contains("* (float") || code.contains("(float)");
            boolean hasSurfOffset10 = code.contains("0x10"); // vTextureU
            boolean hasSurfOffset14 = code.contains("0x14"); // vTextureV
            boolean hasSurfOffset08 = code.contains("0x8");  // pBase
            boolean hasDivide = code.contains("/ (float") || code.contains("fdiv") || code.contains("1.0");
            boolean hasPan = code.contains("Pan") || code.contains("pan");
            boolean hasWidth = code.contains("USize") || code.contains("VSize") || code.contains("Width") || code.contains("Height");
            // Also look for the dot product pattern: a*b + c*d + e*f
            boolean hasDot = code.contains("+") && code.contains("*") && (code.split("\\*").length >= 4);
            
            if (hasMul12) score += 2;
            if (hasSurfOffset10) score += 2;
            if (hasSurfOffset14) score += 2;
            if (hasSurfOffset08) score += 1;
            if (hasDivide) score += 1;
            if (hasFloatMul) score += 1;
            if (hasDot) score += 1;
            
            if (score >= 4) {
                println("\n=== " + func.getName() + " at " + func.getEntryPoint() + " (size=" + size + ", score=" + score + ") ===");
                println("  Indicators: " +
                    (hasMul12 ? "IMUL*12 " : "") +
                    (hasSurfOffset10 ? "vTexU(+0x10) " : "") +
                    (hasSurfOffset14 ? "vTexV(+0x14) " : "") +
                    (hasSurfOffset08 ? "pBase(+0x08) " : "") +
                    (hasDivide ? "DIVIDE " : "") +
                    (hasPan ? "PAN " : "") +
                    (hasWidth ? "SIZE " : ""));
                
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(200, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 200) println("... (" + lines.length + " total lines)");
                decompCount++;
            }
        }
        
        println("\n=== Done: " + decompCount + " UV-relevant functions found ===");
        println("Look for the canonical UV formula:");
        println("  U = dot(vertex_pos - base_pos, TextureU_vec) + PanU");
        println("  V = dot(vertex_pos - base_pos, TextureV_vec) + PanV");
        println("Note: PanU/PanV may be INT shifted to float, divided by texture dimensions");
        
        decomp.dispose();
    }
}
