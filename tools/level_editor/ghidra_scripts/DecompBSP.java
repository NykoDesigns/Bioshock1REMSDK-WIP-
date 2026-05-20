// Decompile functions that reference BSP zone strings
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;
import java.util.Iterator;

public class DecompBSP extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        
        // Key addresses: "Zone %d BSP Surf" string, and "StreamingBSPWeight"
        String[] targets = {"116e7ac8", "11711cb4"};
        String[] names = {"Zone BSP Surf", "StreamingBSPWeight"};
        
        for (int t = 0; t < targets.length; t++) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(targets[t], 16));
            println("\n=== Xrefs to '" + names[t] + "' at " + addr + " ===");
            
            ReferenceManager refMgr = currentProgram.getReferenceManager();
            Iterator<Reference> refIter = refMgr.getReferencesTo(addr).iterator();
            while (refIter.hasNext()) {
                Reference ref = refIter.next();
                Address fromAddr = ref.getFromAddress();
                Function func = currentProgram.getListing().getFunctionContaining(fromAddr);
                if (func != null) {
                    println("Referenced from: " + func.getName() + " at " + func.getEntryPoint());
                    DecompileResults result = decomp.decompileFunction(func, 60, monitor);
                    if (result != null && result.getDecompiledFunction() != null) {
                        String code = result.getDecompiledFunction().getC();
                        String[] lines = code.split("\n");
                        for (int i = 0; i < Math.min(100, lines.length); i++) {
                            println("  " + lines[i]);
                        }
                        if (lines.length > 100) println("  ... (" + lines.length + " total lines)");
                    }
                }
            }
        }
        
        decomp.dispose();
        println("\n=== Done ===");
    }
}
