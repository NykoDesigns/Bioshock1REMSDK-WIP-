// Find PointRegion-like function: traverses BSP tree using plane dot product
// and reads iZone from the node. Search for functions that access node+0x3C (iZone)
// and also access node+0x28/0x2C (iBack/iFront) and node+0x00 (Plane)
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class FindPointRegion extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        
        // Decompile all IMUL*0x64 functions, look for ones that access 
        // both iBack(+0x28) and iFront(+0x2C) along with plane-related ops
        // These are the BSP traversal candidates
        String[] addrs = {
            "10ca9af0",  // Near zone builder  
            "10cd9760",  // 
            "10d73ad0",  //
            "10d742d0",  //
            "10d744d0",  //
            "10d74d40",  //
            "10d74e40",  //
            "10d78970",  //
            "10d7af30",  //
        };
        
        for (String addrStr : addrs) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(addrStr, 16));
            Function func = listing.getFunctionAt(addr);
            if (func == null) continue;
            long size = func.getBody().getNumAddresses();
            if (size < 30 || size > 3000) {
                println("SKIP " + func.getName() + " size=" + size);
                continue;
            }
            
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result == null || result.getDecompiledFunction() == null) continue;
            String code = result.getDecompiledFunction().getC();
            
            // Must access both iBack(+0x28) and iFront(+0x2C) to be a traversal
            if (code.contains("0x28") && code.contains("0x2c")) {
                println("\n=== TRAVERSAL CANDIDATE: " + func.getName() + " at " + addrStr + " (size=" + size + ") ===");
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(150, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 150) println("... (" + lines.length + " total lines)");
            } else if (code.contains("0x3c") || code.contains("0x3C")) {
                // Accesses +0x3C which is predicted iZone offset
                println("\n=== iZone ACCESS: " + func.getName() + " at " + addrStr + " (size=" + size + ") ===");
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(150, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 150) println("... (" + lines.length + " total lines)");
            } else {
                println("NOFIELD " + func.getName() + " at " + addrStr);
            }
        }
        
        decomp.dispose();
        println("\n=== FindPointRegion Done ===");
    }
}
