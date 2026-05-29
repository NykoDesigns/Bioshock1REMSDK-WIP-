// GhidraQuery.java - Parameterized headless query script for BioshockHD.exe
//
// Java GhidraScript so it runs under standard analyzeHeadless.bat with no
// PyGhidra / Jython dependency. Invoked by re_tool.py:
//
//   analyzeHeadless <proj> <name> -process BioshockHD.exe -noanalysis \
//       -scriptPath <dir> -postScript GhidraQuery.java <command> <arg..> <outfile>
//
// Commands (each writes JSON to <outfile>):
//   decompile <funcNameOrAddr> <outfile>
//   func      <namePattern>    <outfile>
//   struct    <structName>     <outfile>
//   xref      <symbolName>     <outfile>
//   search    <asciiString>    <outfile>
//   strings   <substr>         <outfile>
//   data      <addr> <count>   <outfile>

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

import java.io.FileWriter;
import java.util.Iterator;

public class GhidraQuery extends GhidraScript {

    private static String esc(String s) {
        if (s == null) return "";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n");  break;
                case '\r': b.append("\\r");  break;
                case '\t': b.append("\\t");  break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
                    else b.append(c);
            }
        }
        return b.toString();
    }

    private void writeOut(String path, String json) throws Exception {
        FileWriter w = new FileWriter(path);
        try { w.write(json); } finally { w.close(); }
        println("[GhidraQuery] wrote " + path);
    }

    private Function findFunction(String nameOrAddr) {
        FunctionManager fm = currentProgram.getFunctionManager();
        FunctionIterator it = fm.getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (f.getName().equals(nameOrAddr)) return f;
        }
        try {
            Address a = currentProgram.getAddressFactory().getAddress(nameOrAddr);
            if (a != null) {
                Function f = fm.getFunctionContaining(a);
                if (f != null) return f;
            }
        } catch (Exception e) { /* ignore */ }
        return null;
    }

    private void cmdDecompile(String arg, String out) throws Exception {
        Function f = findFunction(arg);
        if (f == null) { writeOut(out, "{\"error\":\"function not found: " + esc(arg) + "\"}"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        String code = "<decompile failed>";
        try {
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (r != null && r.getDecompiledFunction() != null)
                code = r.getDecompiledFunction().getC();
        } finally { di.dispose(); }
        StringBuilder b = new StringBuilder();
        b.append("{\"name\":\"").append(esc(f.getName())).append("\",");
        b.append("\"entry\":\"").append(esc(f.getEntryPoint().toString())).append("\",");
        b.append("\"signature\":\"").append(esc(f.getPrototypeString(false, false))).append("\",");
        b.append("\"code\":\"").append(esc(code)).append("\"}");
        writeOut(out, b.toString());
    }

    private void cmdFunc(String pattern, String out) throws Exception {
        String pl = pattern.toLowerCase();
        FunctionManager fm = currentProgram.getFunctionManager();
        FunctionIterator it = fm.getFunctions(true);
        StringBuilder b = new StringBuilder();
        b.append("{\"pattern\":\"").append(esc(pattern)).append("\",\"functions\":[");
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            if (f.getName().toLowerCase().contains(pl)) {
                if (n > 0) b.append(",");
                b.append("{\"name\":\"").append(esc(f.getName())).append("\",");
                b.append("\"entry\":\"").append(esc(f.getEntryPoint().toString())).append("\",");
                b.append("\"signature\":\"").append(esc(f.getPrototypeString(false, false))).append("\"}");
                n++;
                if (n >= 400) break;
            }
        }
        b.append("],\"count\":").append(n).append("}");
        writeOut(out, b.toString());
    }

    private void cmdStruct(String name, String out) throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        Iterator<Structure> it = dtm.getAllStructures();
        String nl = name.toLowerCase();
        StringBuilder b = new StringBuilder();
        b.append("{\"query\":\"").append(esc(name)).append("\",\"structs\":[");
        int n = 0;
        while (it.hasNext()) {
            Structure dt = it.next();
            if (dt.getName().equals(name) || dt.getName().toLowerCase().contains(nl)) {
                if (n > 0) b.append(",");
                b.append("{\"name\":\"").append(esc(dt.getName())).append("\",");
                b.append("\"length\":").append(dt.getLength()).append(",\"components\":[");
                DataTypeComponent[] comps = dt.getComponents();
                for (int i = 0; i < comps.length; i++) {
                    if (i > 0) b.append(",");
                    DataTypeComponent c = comps[i];
                    String fn = c.getFieldName() == null ? "" : c.getFieldName();
                    b.append("{\"offset\":").append(c.getOffset()).append(",");
                    b.append("\"length\":").append(c.getLength()).append(",");
                    b.append("\"type\":\"").append(esc(c.getDataType().getName())).append("\",");
                    b.append("\"field\":\"").append(esc(fn)).append("\"}");
                }
                b.append("]}");
                n++;
                if (n >= 20) break;
            }
        }
        b.append("],\"count\":").append(n).append("}");
        writeOut(out, b.toString());
    }

    private void cmdXref(String name, String out) throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        StringBuilder b = new StringBuilder();
        b.append("{\"query\":\"").append(esc(name)).append("\",\"symbols\":[");
        int sn = 0;
        for (Symbol sym : st.getGlobalSymbols(name)) {
            if (sn > 0) b.append(",");
            Address addr = sym.getAddress();
            b.append("{\"symbol\":\"").append(esc(name)).append("\",");
            b.append("\"addr\":\"").append(esc(addr.toString())).append("\",\"xrefs\":[");
            int rn = 0;
            for (Reference r : rm.getReferencesTo(addr)) {
                if (rn > 0) b.append(",");
                Address fa = r.getFromAddress();
                Function f = fm.getFunctionContaining(fa);
                b.append("{\"from\":\"").append(esc(fa.toString())).append("\",");
                b.append("\"func\":\"").append(esc(f == null ? "<none>" : f.getName())).append("\",");
                b.append("\"type\":\"").append(esc(r.getReferenceType().toString())).append("\"}");
                rn++;
                if (rn >= 200) break;
            }
            b.append("]}");
            sn++;
        }
        b.append("]}");
        writeOut(out, b.toString());
    }

    private void cmdSearch(String text, String out) throws Exception {
        Memory mem = currentProgram.getMemory();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        byte[] ascii = text.getBytes("US-ASCII");
        byte[] utf16 = text.getBytes("UTF-16LE");
        byte[][] needles = { ascii, utf16 };
        String[] encs = { "ascii", "utf16le" };
        int maxNeedle = Math.max(ascii.length, utf16.length);
        StringBuilder b = new StringBuilder();
        b.append("{\"query\":\"").append(esc(text)).append("\",\"hits\":[");
        int hits = 0;
        final int CHUNK = 8 * 1024 * 1024;     // 8 MB window
        final int OVERLAP = maxNeedle - 1;     // so matches spanning chunk edges aren't missed
        for (MemoryBlock block : mem.getBlocks()) {
            if (!block.isInitialized()) continue;
            long size = block.getSize();
            long off = 0;
            while (off < size && hits < 200) {
                int readLen = (int) Math.min((long) CHUNK + OVERLAP, size - off);
                byte[] buf = new byte[readLen];
                Address chunkStart = block.getStart().add(off);
                int got;
                try { got = block.getBytes(chunkStart, buf); }
                catch (Exception e) { break; }
                for (int i = 0; i < got; i++) {
                    int encIdx = -1;
                    for (int ni = 0; ni < needles.length; ni++) {
                        byte[] needle = needles[ni];
                        if (i + needle.length > got) continue;
                        boolean match = true;
                        for (int j = 0; j < needle.length; j++) {
                            if (buf[i + j] != needle[j]) { match = false; break; }
                        }
                        if (match) { encIdx = ni; break; }
                    }
                    if (encIdx < 0) continue;
                    Address addr = chunkStart.add(i);
                    if (hits > 0) b.append(",");
                    b.append("{\"addr\":\"").append(esc(addr.toString())).append("\",");
                    b.append("\"enc\":\"").append(encs[encIdx]).append("\",");
                    b.append("\"block\":\"").append(esc(block.getName())).append("\",\"xrefs\":[");
                    int rn = 0;
                    for (Reference r : rm.getReferencesTo(addr)) {
                        if (rn > 0) b.append(",");
                        Address fa = r.getFromAddress();
                        Function f = fm.getFunctionContaining(fa);
                        b.append("{\"from\":\"").append(esc(fa.toString())).append("\",");
                        b.append("\"func\":\"").append(esc(f == null ? "<none>" : f.getName())).append("\"}");
                        rn++;
                    }
                    b.append("]}");
                    hits++;
                    if (hits >= 200) break;
                }
                off += CHUNK; // advance by CHUNK (OVERLAP keeps edge matches)
            }
            if (hits >= 200) break;
        }
        b.append("],\"count\":").append(hits).append("}");
        writeOut(out, b.toString());
    }

    private void cmdStrings(String substr, String out) throws Exception {
        Listing listing = currentProgram.getListing();
        String sl = substr.toLowerCase();
        DataIterator di = listing.getDefinedData(true);
        StringBuilder b = new StringBuilder();
        b.append("{\"query\":\"").append(esc(substr)).append("\",\"strings\":[");
        int n = 0;
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v == null) continue;
            String sv = v.toString();
            if (sv.toLowerCase().contains(sl)) {
                if (n > 0) b.append(",");
                b.append("{\"addr\":\"").append(esc(d.getAddress().toString())).append("\",");
                b.append("\"value\":\"").append(esc(sv)).append("\"}");
                n++;
                if (n >= 300) break;
            }
        }
        b.append("],\"count\":").append(n).append("}");
        writeOut(out, b.toString());
    }

    private void cmdData(String addrStr, String countStr, String out) throws Exception {
        Address addr;
        try { addr = currentProgram.getAddressFactory().getAddress(addrStr); }
        catch (Exception e) { writeOut(out, "{\"error\":\"bad address: " + esc(addrStr) + "\"}"); return; }
        int count = Integer.parseInt(countStr);
        byte[] buf = new byte[count];
        int got = currentProgram.getMemory().getBytes(addr, buf);
        StringBuilder hx = new StringBuilder();
        for (int i = 0; i < got; i++) {
            if (i > 0) hx.append(" ");
            hx.append(String.format("%02x", buf[i] & 0xff));
        }
        StringBuilder b = new StringBuilder();
        b.append("{\"addr\":\"").append(esc(addr.toString())).append("\",");
        b.append("\"count\":").append(got).append(",\"hex\":\"").append(esc(hx.toString())).append("\"}");
        writeOut(out, b.toString());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("[GhidraQuery] usage: <command> <arg..> <outfile>"); return; }
        String cmd = args[0];
        if (cmd.equals("decompile"))      cmdDecompile(args[1], args[2]);
        else if (cmd.equals("func"))      cmdFunc(args[1], args[2]);
        else if (cmd.equals("struct"))    cmdStruct(args[1], args[2]);
        else if (cmd.equals("xref"))      cmdXref(args[1], args[2]);
        else if (cmd.equals("search"))    cmdSearch(args[1], args[2]);
        else if (cmd.equals("strings"))   cmdStrings(args[1], args[2]);
        else if (cmd.equals("data"))      cmdData(args[1], args[2], args[3]);
        else println("[GhidraQuery] unknown command: " + cmd);
    }
}
