// Find functions that access BSP nodes (stride 100 = 0x64) and decompile them
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class FindNodeAccess extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        
        Listing listing = currentProgram.getListing();
        
        // Strategy 1: Find functions containing "imul" with immediate 0x64 (100)
        // This is how BSP node arrays are indexed: nodePtr + index * 100
        println("=== Strategy 1: Search for imul with 0x64 (stride 100) ===");
        
        Set<String> foundFuncs = new HashSet<String>();
        InstructionIterator instIter = listing.getInstructions(true);
        int checked = 0;
        
        while (instIter.hasNext() && checked < 5000000) {
            Instruction inst = instIter.next();
            checked++;
            String mnemonic = inst.getMnemonicString();
            if (mnemonic.equals("IMUL")) {
                // Check if any operand is 0x64
                int numOps = inst.getNumOperands();
                for (int i = 0; i < numOps; i++) {
                    String opStr = inst.getDefaultOperandRepresentation(i);
                    if (opStr.equals("0x64")) {
                        Function func = listing.getFunctionContaining(inst.getAddress());
                        if (func != null && !foundFuncs.contains(func.getName())) {
                            foundFuncs.add(func.getName());
                            println("IMUL*0x64 in: " + func.getName() + " at " + func.getEntryPoint() + " (inst at " + inst.getAddress() + ")");
                        }
                    }
                }
            }
        }
        println("Checked " + checked + " instructions, found " + foundFuncs.size() + " functions with IMUL*0x64");
        
        // Strategy 2: Find xrefs to "Zone %d %d: BSP Surf" and decompile the CALLER chain
        println("\n=== Strategy 2: Decompile Zone BSP Surf callers ===");
        Address zoneSurfAddr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x116e7ac8L);
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        Iterator<Reference> refIter = refMgr.getReferencesTo(zoneSurfAddr).iterator();
        Set<String> decompiled = new HashSet<String>();
        
        while (refIter.hasNext()) {
            Reference ref = refIter.next();
            Function func = listing.getFunctionContaining(ref.getFromAddress());
            if (func != null && !decompiled.contains(func.getName())) {
                decompiled.add(func.getName());
                // Get callers of this function
                Iterator<Reference> callerIter = refMgr.getReferencesTo(func.getEntryPoint()).iterator();
                while (callerIter.hasNext()) {
                    Reference callerRef = callerIter.next();
                    Function caller = listing.getFunctionContaining(callerRef.getFromAddress());
                    if (caller != null && !decompiled.contains(caller.getName())) {
                        decompiled.add(caller.getName());
                        println("\nCaller of zone builder: " + caller.getName() + " at " + caller.getEntryPoint());
                    }
                }
            }
        }
        
        // Decompile the IMUL*0x64 functions (most likely BSP node accessors)
        println("\n=== Decompiling IMUL*0x64 functions ===");
        int decompCount = 0;
        for (String funcName : foundFuncs) {
            if (decompCount >= 8) break; // limit output
            FunctionIterator funcIter = listing.getFunctions(true);
            while (funcIter.hasNext()) {
                Function f = funcIter.next();
                if (f.getName().equals(funcName)) {
                    // Only decompile if function is reasonably sized
                    long size = f.getBody().getNumAddresses();
                    if (size > 20 && size < 5000) {
                        println("\n--- Decompiling " + funcName + " at " + f.getEntryPoint() + " (size=" + size + ") ---");
                        DecompileResults result = decomp.decompileFunction(f, 60, monitor);
                        if (result != null && result.getDecompiledFunction() != null) {
                            String code = result.getDecompiledFunction().getC();
                            String[] lines = code.split("\n");
                            for (int i = 0; i < Math.min(120, lines.length); i++) {
                                println("  " + lines[i]);
                            }
                            if (lines.length > 120) println("  ... (" + lines.length + " total lines)");
                        }
                        decompCount++;
                    }
                    break;
                }
            }
        }
        
        decomp.dispose();
        println("\n=== FindNodeAccess Done ===");
    }
}
