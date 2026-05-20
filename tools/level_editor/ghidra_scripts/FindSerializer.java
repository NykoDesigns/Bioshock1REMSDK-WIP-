// Find FBspNode serialization function - it reads sequential fields from archive
// In UE2: operator<<(FArchive&, FBspNode&) 
// The serializer for a 100-byte struct will call FArchive::operator<< for each field.
// Look for the FBspNode constructor that initializes a zeroed 100-byte block.
// Also decompile FUN_10cfb9e0 helper functions to understand field layout.
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;

public class FindSerializer extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        
        // Decompile FUN_10cfb9e0 (BSP node serializer) and its helper functions
        // FUN_10b34990 reads 4 bytes from archive
        // FUN_10ce5c30, FUN_10cffda0, FUN_10cea5f0 are called after initial reads
        String[] addrs = {
            "10cfb9e0",  // Main serializer (reads nodes with stride 100)
            "10cffda0",  // Called during node serialization
            "10cea5f0",  // Called during node serialization  
            "10cffb00",  // Called conditionally based on +0x14 > 2
            "10ce5c30",  // Called during node serialization
            "10b34990",  // Archive reader (4 bytes)
        };
        
        for (String addrStr : addrs) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(addrStr, 16));
            Function func = listing.getFunctionAt(addr);
            if (func == null) {
                println("No function at " + addrStr);
                continue;
            }
            long size = func.getBody().getNumAddresses();
            println("\n=== " + func.getName() + " at " + addrStr + " (size=" + size + ") ===");
            
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result != null && result.getDecompiledFunction() != null) {
                String code = result.getDecompiledFunction().getC();
                String[] lines = code.split("\n");
                for (int i = 0; i < Math.min(150, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 150) println("... (" + lines.length + " total lines)");
            }
        }
        
        decomp.dispose();
        println("\n=== FindSerializer Done ===");
    }
}
