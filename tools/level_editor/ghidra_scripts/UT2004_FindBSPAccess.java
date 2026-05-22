// Find BSP node access patterns in UT2004 Engine.dll
// Standard UE2 FBspNode is 64 bytes (0x40), vs Vengeance/BioShock's 100 bytes (0x64)
// This script finds all IMUL*0x40 uses and maps field offset accesses.
//
// Known UE2 FBspNode layout (64 bytes):
//   +0x00: FPlane (16B) - plane equation (X,Y,Z,W)
//   +0x10: QWORD ZoneMask (8B) - 64-bit zone visibility (64 zones max)
//   +0x18: INT32 iVertPool
//   +0x1C: INT32 iSurf
//   +0x20: INT32 iBack
//   +0x24: INT32 iFront
//   +0x28: INT32 iPlane
//   +0x2C: INT32 iCollisionBound
//   +0x30: INT32 iRenderBound
//   +0x34: BYTE iZone[0], BYTE iZone[1], BYTE NumVertices, BYTE NodeFlags
//   +0x38: INT32 iLeaf[0]
//   +0x3C: INT32 iLeaf[1]
//
// Vengeance changes vs standard UE2:
//   - ZoneMask expanded: 8B -> 16B (64 -> 128 zones), shifts everything after +16 by 8
//   - Added: BoundOrigin (12B float3) + BoundRadius (4B float) = 16B bounding sphere
//   - Added: iContentBound (4B) + iRenderZone (4B) = 8B at end
//   - NumVertices moved from packed BYTE to separate field
//   Total: 64 -> 100 bytes
//
// @category BSP

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class UT2004_FindBSPAccess extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Listing listing = currentProgram.getListing();
        
        println("=== UT2004 Engine.dll BSP Node Access Finder ===");
        println("Looking for IMUL by 0x40 (64-byte FBspNode stride)...\n");
        
        // Search for IMUL reg, reg, 0x40 instructions
        // x86 encoding: 6B xx 40 (IMUL r32, r/m32, 0x40) - 3 bytes
        // or: 69 xx 40 00 00 00 (IMUL r32, r/m32, 0x00000040) - 6 bytes
        
        Set<Function> bspFunctions = new HashSet<>();
        
        // Search pattern 1: 6B xx 40 (imul r32, r/m32, 0x40)
        byte[] pattern1 = new byte[] { 0x6B };
        Address searchAddr = mem.getMinAddress();
        Address maxAddr = mem.getMaxAddress();
        
        int found = 0;
        while (searchAddr != null && searchAddr.compareTo(maxAddr) < 0 && found < 500) {
            searchAddr = mem.findBytes(searchAddr, maxAddr, new byte[] { 0x6B }, null, true, monitor);
            if (searchAddr == null) break;
            
            try {
                byte nextByte = mem.getByte(searchAddr.add(2));
                if (nextByte == 0x40) {
                    Function func = listing.getFunctionContaining(searchAddr);
                    if (func != null) {
                        bspFunctions.add(func);
                        found++;
                    }
                }
            } catch (Exception e) {}
            searchAddr = searchAddr.add(1);
        }
        
        // Search pattern 2: 69 xx 40 00 00 00 (imul r32, r/m32, 0x40 as dword)
        searchAddr = mem.getMinAddress();
        while (searchAddr != null && searchAddr.compareTo(maxAddr) < 0 && found < 500) {
            searchAddr = mem.findBytes(searchAddr, maxAddr, new byte[] { 0x69 }, null, true, monitor);
            if (searchAddr == null) break;
            
            try {
                int imm32 = mem.getInt(searchAddr.add(2));
                if (imm32 == 0x40) {
                    Function func = listing.getFunctionContaining(searchAddr);
                    if (func != null) {
                        bspFunctions.add(func);
                        found++;
                    }
                }
            } catch (Exception e) {}
            searchAddr = searchAddr.add(1);
        }
        
        // Also look for SHL by 6 (equivalent to *64)
        searchAddr = mem.getMinAddress();
        while (searchAddr != null && searchAddr.compareTo(maxAddr) < 0 && found < 500) {
            searchAddr = mem.findBytes(searchAddr, maxAddr, new byte[] { (byte)0xC1 }, null, true, monitor);
            if (searchAddr == null) break;
            
            try {
                byte modRM = mem.getByte(searchAddr.add(1));
                byte imm = mem.getByte(searchAddr.add(2));
                // C1 Ex 06 = SHL reg, 6 (where E0-E7 are different regs)
                if ((modRM & 0xF8) == 0xE0 && imm == 0x06) {
                    Function func = listing.getFunctionContaining(searchAddr);
                    if (func != null) {
                        bspFunctions.add(func);
                        found++;
                    }
                }
            } catch (Exception e) {}
            searchAddr = searchAddr.add(1);
        }
        
        println("Found " + bspFunctions.size() + " unique functions with *0x40 or SHL 6 patterns\n");
        
        // Decompile each and look for node field accesses
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        
        int decompiled = 0;
        for (Function func : bspFunctions) {
            if (decompiled >= 30) break; // limit output
            long size = func.getBody().getNumAddresses();
            if (size < 30 || size > 5000) continue;
            
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result == null || result.getDecompiledFunction() == null) continue;
            
            String code = result.getDecompiledFunction().getC();
            
            // Check for BSP-relevant field offsets
            boolean hasPlane = code.contains("0x0") || code.contains("0x4") || code.contains("0x8") || code.contains("0xc");
            boolean hasZoneMask = code.contains("0x10");
            boolean hasVertPool = code.contains("0x18");
            boolean hasSurf = code.contains("0x1c") || code.contains("0x1C");
            boolean hasBackFront = code.contains("0x20") || code.contains("0x24");
            boolean hasPlaneIdx = code.contains("0x28");
            boolean hasCollision = code.contains("0x2c") || code.contains("0x2C");
            boolean hasRender = code.contains("0x30");
            boolean hasZoneVerts = code.contains("0x34") || code.contains("0x36") || code.contains("0x37");
            boolean hasLeaf = code.contains("0x38") || code.contains("0x3c") || code.contains("0x3C");
            
            // Only show functions that access multiple BSP-relevant offsets
            int score = 0;
            if (hasVertPool) score += 2;
            if (hasSurf) score += 2;
            if (hasBackFront) score += 3;
            if (hasZoneMask) score += 1;
            if (hasLeaf) score += 1;
            if (hasZoneVerts) score += 2;
            
            if (score >= 3) {
                println("\n=== " + func.getName() + " at " + func.getEntryPoint() + " (size=" + size + ", score=" + score + ") ===");
                println("  Fields: " + 
                    (hasVertPool ? "iVertPool(+0x18) " : "") +
                    (hasSurf ? "iSurf(+0x1C) " : "") +
                    (hasBackFront ? "iBack/iFront(+0x20/+0x24) " : "") +
                    (hasPlaneIdx ? "iPlane(+0x28) " : "") +
                    (hasZoneMask ? "ZoneMask(+0x10) " : "") +
                    (hasZoneVerts ? "Zone/NumVerts(+0x34-0x37) " : "") +
                    (hasLeaf ? "iLeaf(+0x38/+0x3C) " : ""));
                
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(150, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 150) println("... (" + lines.length + " total lines)");
                decompiled++;
            }
        }
        
        println("\n=== Summary ===");
        println("Total functions with IMUL*0x40 or SHL 6: " + bspFunctions.size());
        println("Decompiled with BSP field accesses: " + decompiled);
        println("\nCompare these offsets against BioShock's Vengeance layout:");
        println("  UE2 iVertPool=+0x18  vs  Vengeance=+0x20 (shifted by +8 from expanded ZoneMask)");
        println("  UE2 iSurf=+0x1C      vs  Vengeance=+0x24");
        println("  UE2 iBack=+0x20      vs  Vengeance=+0x28");
        println("  UE2 iFront=+0x24     vs  Vengeance=+0x2C");
        println("  UE2 NumVerts=+0x36   vs  Vengeance=+0x4E (byte at +78)");
        
        decomp.dispose();
    }
}
