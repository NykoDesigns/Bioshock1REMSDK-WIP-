# Ghidra headless script to find FBspNode structure usage
# Run with: analyzeHeadless <project_dir> <project_name> -import <exe> -postScript this_script.py

from ghidra.program.model.symbol import SymbolType
from ghidra.program.model.data import *
from ghidra.program.model.listing import *
from ghidra.app.decompiler import DecompInterface
import re

# Search for functions that reference "zone" related patterns
# In UE2, the zone lookup function is typically called "pointZone" or accesses iZone fields

program = getCurrentProgram()
listing = program.getListing()
memory = program.getMemory()
addr_factory = program.getAddressFactory()

# Find all string references to "Zone" in the binary
print("=== Searching for Zone-related strings ===")
string_table = program.getListing()

# Search memory for "iZone" or "ZoneMask" ASCII strings
import ghidra.program.model.mem as mem

results = []
search_bytes = b"Zone"
block = memory.getBlock(".rdata")
if block is None:
    block = memory.getBlock(".text")

# Search all memory blocks for BSP-related strings
for block in memory.getBlocks():
    if not block.isInitialized():
        continue
    start = block.getStart()
    end = block.getEnd()
    size = end.subtract(start) + 1
    if size > 50000000:  # skip blocks > 50MB
        continue
    
    buf = bytearray(size)
    try:
        block.getBytes(start, buf)
    except:
        continue
    
    buf_str = buf.decode('ascii', errors='replace')
    
    # Look for "pointZone" or "PointZone" - the function that determines actor zone
    for match in re.finditer(r'pointZone|PointZone|pointRegion|PointRegion', buf_str):
        offset = match.start()
        addr = start.add(offset)
        print("Found '{}' at {}".format(match.group(), addr))
        results.append(addr)

# Try to find functions that access offset 96 of a structure (our suspected iZone)
# Look for decompiled code with patterns like "*(param + 0x60)" which is +96 decimal
print("\n=== Looking for zone-related decompiled functions ===")

decomp = DecompInterface()
decomp.openProgram(program)

# Find functions that reference our known BSP addresses
func_iter = listing.getFunctions(True)
count = 0
zone_funcs = []

for func in func_iter:
    if count > 5000:
        break
    count += 1
    
    name = func.getName()
    # Look for functions with BSP/Zone/Model in their name
    if any(x in name.lower() for x in ['bsp', 'zone', 'model', 'node', 'leaf']):
        print("Interesting function: {} at {}".format(name, func.getEntryPoint()))
        zone_funcs.append(func)
        
        # Decompile it
        result = decomp.decompileFunction(func, 30, monitor)
        if result and result.getDecompiledFunction():
            code = result.getDecompiledFunction().getC()
            # Print first 50 lines
            lines = code.split('\n')[:50]
            for line in lines:
                print("  " + line)

decomp.dispose()
print("\n=== Analysis complete ===")
print("Found {} zone-related functions".format(len(zone_funcs)))
