# GhidraQuery.py - Parameterized headless query script for BioshockHD.exe
# Compatible with both Jython and PyGhidra (Ghidra 11.x / 12.x).
#
# Invoked by re_tool.py via:
#   analyzeHeadless <proj> <name> -process BioshockHD.exe -noanalysis \
#       -scriptPath <dir> -postScript GhidraQuery.py <command> <arg> <outfile>
#
# Commands (each writes JSON to <outfile>):
#   decompile <funcNameOrAddr> <outfile>   - decompiled C of a function
#   func      <namePattern>    <outfile>   - list functions matching substring
#   struct    <structName>     <outfile>   - dump a struct/datatype layout
#   xref      <symbolName>     <outfile>   - callers / references to a symbol
#   search    <asciiString>    <outfile>   - find ASCII string in memory + xrefs
#   strings   <substr>         <outfile>   - list defined strings containing substr
#   data      <addr> <count>   <outfile>   - hex dump <count> bytes at <addr>
#
# All results are JSON so re_tool.py can parse and pretty-print them.

import json

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

program = getCurrentProgram()
listing = program.getListing()
memory = program.getMemory()
af = program.getAddressFactory()
fm = program.getFunctionManager()
st = program.getSymbolTable()
refMgr = program.getReferenceManager()
monitor = ConsoleTaskMonitor()


def write_json(path, obj):
    f = open(path, "w")
    try:
        f.write(json.dumps(obj, indent=2))
    finally:
        f.close()
    print("[GhidraQuery] wrote {}".format(path))


def resolve_addr(s):
    try:
        return af.getAddress(s)
    except:
        return None


def find_function(name_or_addr):
    # Try by exact name first
    funcs = []
    it = fm.getFunctions(True)
    for f in it:
        if f.getName() == name_or_addr:
            return f
    # Try by address
    addr = resolve_addr(name_or_addr)
    if addr is not None:
        f = fm.getFunctionContaining(addr)
        if f is not None:
            return f
    return None


def decompile_func(func):
    decomp = DecompInterface()
    decomp.openProgram(program)
    try:
        res = decomp.decompileFunction(func, 60, monitor)
        if res is not None and res.getDecompiledFunction() is not None:
            return res.getDecompiledFunction().getC()
        return None
    finally:
        decomp.dispose()


def cmd_decompile(arg, outfile):
    func = find_function(arg)
    if func is None:
        write_json(outfile, {"error": "function not found: {}".format(arg)})
        return
    code = decompile_func(func)
    write_json(outfile, {
        "name": func.getName(),
        "entry": str(func.getEntryPoint()),
        "signature": func.getPrototypeString(False, False),
        "code": code if code else "<decompile failed>"
    })


def cmd_func(pattern, outfile):
    results = []
    pl = pattern.lower()
    it = fm.getFunctions(True)
    for f in it:
        if pl in f.getName().lower():
            results.append({
                "name": f.getName(),
                "entry": str(f.getEntryPoint()),
                "signature": f.getPrototypeString(False, False)
            })
            if len(results) >= 400:
                break
    write_json(outfile, {"pattern": pattern, "count": len(results), "functions": results})


def dump_datatype(dt):
    info = {"name": dt.getName(), "length": dt.getLength()}
    try:
        comps = []
        for i in range(dt.getNumComponents()):
            c = dt.getComponent(i)
            comps.append({
                "offset": c.getOffset(),
                "length": c.getLength(),
                "type": c.getDataType().getName(),
                "field": c.getFieldName() if c.getFieldName() else ""
            })
        info["components"] = comps
    except:
        info["components"] = []
    return info


def cmd_struct(name, outfile):
    dtm = program.getDataTypeManager()
    matches = []
    it = dtm.getAllStructures()
    for dt in it:
        if dt.getName() == name or name.lower() in dt.getName().lower():
            matches.append(dump_datatype(dt))
            if len(matches) >= 20:
                break
    write_json(outfile, {"query": name, "count": len(matches), "structs": matches})


def cmd_xref(name, outfile):
    results = []
    syms = st.getGlobalSymbols(name)
    for sym in syms:
        addr = sym.getAddress()
        refs = refMgr.getReferencesTo(addr)
        callers = []
        for r in refs:
            from_addr = r.getFromAddress()
            f = fm.getFunctionContaining(from_addr)
            callers.append({
                "from": str(from_addr),
                "func": f.getName() if f is not None else "<none>",
                "type": str(r.getReferenceType())
            })
        results.append({"symbol": name, "addr": str(addr), "xrefs": callers})
    write_json(outfile, {"query": name, "symbols": results})


def cmd_search(text, outfile):
    results = []
    needle = bytearray(text.encode("ascii"))
    for block in memory.getBlocks():
        if not block.isInitialized():
            continue
        start = block.getStart()
        end = block.getEnd()
        size = end.subtract(start) + 1
        if size > 80000000:
            continue
        buf = bytearray(size)
        try:
            block.getBytes(start, buf)
        except:
            continue
        idx = 0
        s = bytes(buf)
        nd = bytes(needle)
        while True:
            pos = s.find(nd, idx)
            if pos < 0:
                break
            addr = start.add(pos)
            entry = {"addr": str(addr), "block": block.getName()}
            # xrefs to this address
            refs = refMgr.getReferencesTo(addr)
            xr = []
            for r in refs:
                fa = r.getFromAddress()
                f = fm.getFunctionContaining(fa)
                xr.append({"from": str(fa), "func": f.getName() if f is not None else "<none>"})
            entry["xrefs"] = xr
            results.append(entry)
            idx = pos + 1
            if len(results) >= 200:
                break
        if len(results) >= 200:
            break
    write_json(outfile, {"query": text, "count": len(results), "hits": results})


def cmd_strings(substr, outfile):
    results = []
    sl = substr.lower()
    di = listing.getDefinedData(True)
    for d in di:
        try:
            val = d.getValue()
            if val is None:
                continue
            sval = str(val)
            if sl in sval.lower():
                results.append({"addr": str(d.getAddress()), "value": sval})
                if len(results) >= 300:
                    break
        except:
            continue
    write_json(outfile, {"query": substr, "count": len(results), "strings": results})


def cmd_data(addr_str, count_str, outfile):
    addr = resolve_addr(addr_str)
    if addr is None:
        write_json(outfile, {"error": "bad address: {}".format(addr_str)})
        return
    count = int(count_str)
    buf = bytearray(count)
    n = memory.getBytes(addr, buf)
    hexstr = " ".join("{:02x}".format(b & 0xff) for b in buf[:n])
    write_json(outfile, {"addr": str(addr), "count": n, "hex": hexstr})


def main():
    args = getScriptArgs()
    if len(args) < 2:
        print("[GhidraQuery] usage: <command> <arg...> <outfile>")
        return
    cmd = args[0]
    if cmd == "decompile":
        cmd_decompile(args[1], args[2])
    elif cmd == "func":
        cmd_func(args[1], args[2])
    elif cmd == "struct":
        cmd_struct(args[1], args[2])
    elif cmd == "xref":
        cmd_xref(args[1], args[2])
    elif cmd == "search":
        cmd_search(args[1], args[2])
    elif cmd == "strings":
        cmd_strings(args[1], args[2])
    elif cmd == "data":
        cmd_data(args[1], args[2], args[3])
    else:
        print("[GhidraQuery] unknown command: {}".format(cmd))


main()
