// Decompile specific BSP node functions by address
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;

public class DecompTargets extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        
        // Small BSP node accessor functions likely to reveal structure layout
        // These use IMUL*0x64 (stride 100) to access BSP node arrays
        String[] addrs = {
            "10c48e80",  // first IMUL*0x64 function
            "10c4a940",  // second
            "10cde480",  // 
            "10cde580",  //
            "10cde770",  //
            "10ce10e0",  //
            "10ce1160",  //
            "10ce1180",  //
            "10ce1190",  //
            "10cfb9e0",  //
            "10cfbbe0",  //
            "10d01c20",  //
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
                for (int i = 0; i < Math.min(80, lines.length); i++) {
                    println(lines[i]);
                }
                if (lines.length > 80) println("... (" + lines.length + " total lines)");
            } else {
                println("Failed to decompile");
            }
        }
        
        decomp.dispose();
        println("\n=== DecompTargets Done ===");
    }
}
