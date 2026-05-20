// Decompile BSP traversal functions - ones that access iFront(+0x2C)/iBack(+0x28) 
// and do plane dot products. Also look at the zone builder callers.
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;

public class DecompTraversal extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        
        // BSP model/zone functions from the 10d5xxxx and 10d7xxxx range
        // Also the serializer helper functions
        String[] addrs = {
            "10d54ba0",  // IMUL*0x64
            "10d54fe0",  // IMUL*0x64
            "10d55480",  // IMUL*0x64
            "10d73ad0",  // IMUL*0x64  
            "10d742d0",  // IMUL*0x64
            "10d744d0",  // IMUL*0x64 - likely zone builder
            "10d74d40",  // IMUL*0x64
            "10d74e40",  // IMUL*0x64
            "10d77850",  // IMUL*0x64
            "10d77a90",  // IMUL*0x64
            "10d78970",  // IMUL*0x64
            "10d7a510",  // IMUL*0x64
            "10d7af30",  // IMUL*0x64
            "10ca9af0",  // IMUL*0x64 near zone builder
            "10ced460",  // IMUL*0x64
        };
        
        for (String addrStr : addrs) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(addrStr, 16));
            Function func = listing.getFunctionAt(addr);
            if (func == null) continue;
            long size = func.getBody().getNumAddresses();
            // Only decompile functions between 50 and 2000 bytes (skip tiny helpers and huge builders)
            if (size < 50 || size > 2000) {
                println("SKIP " + func.getName() + " at " + addrStr + " (size=" + size + ")");
                continue;
            }
            
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result == null || result.getDecompiledFunction() == null) continue;
            
            String code = result.getDecompiledFunction().getC();
            // Only print if it accesses offsets 0x28 or 0x2C (iFront/iBack) or has float ops
            if (code.contains("0x28") || code.contains("0x2c") || code.contains("0x2C") ||
                code.contains("0x3c") || code.contains("0x3C") ||
                code.contains("0x58") || code.contains("0x5c") || code.contains("0x5C") ||
                code.contains("0x60")) {
                println("\n=== " + func.getName() + " at " + addrStr + " (size=" + size + ") ===");
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(120, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 120) println("... (" + lines.length + " total lines)");
            } else {
                println("NOFIELD " + func.getName() + " at " + addrStr + " (no relevant offsets)");
            }
        }
        
        decomp.dispose();
        println("\n=== DecompTraversal Done ===");
    }
}
