// Ghidra script to find BSP/zone related functions
// @category BSP
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class FindBSP extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("=== BSP/Zone Function Search ===");
        FunctionIterator iter = currentProgram.getListing().getFunctions(true);
        int count = 0;
        while (iter.hasNext()) {
            Function func = iter.next();
            String name = func.getName().toLowerCase();
            if (name.contains("bsp") || name.contains("zone") || name.contains("leaf") || 
                name.contains("pointregion") || name.contains("umodel") ||
                name.contains("node") || name.contains("traverse")) {
                println("FUNC: " + func.getName() + " at " + func.getEntryPoint());
                count++;
            }
        }
        println("Found " + count + " BSP/zone/node functions");
        
        // Also search for string references
        println("\n=== String Search for BSP terms ===");
        SymbolTable symTable = currentProgram.getSymbolTable();
        SymbolIterator symIter = symTable.getAllSymbols(true);
        int strCount = 0;
        while (symIter.hasNext() && strCount < 50) {
            Symbol sym = symIter.next();
            String sn = sym.getName().toLowerCase();
            if (sn.contains("bsp") || sn.contains("zone") || sn.contains("izone") ||
                sn.contains("pointregion") || sn.contains("fleaf")) {
                println("SYM: " + sym.getName() + " at " + sym.getAddress() + " type=" + sym.getSymbolType());
                strCount++;
            }
        }
        println("=== Done ===");
    }
}
