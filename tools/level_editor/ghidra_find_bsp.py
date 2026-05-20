# Simple Ghidra script - find BSP/zone related functions by name
# @category BSP

from ghidra.program.model.listing import *

program = getCurrentProgram()
listing = program.getListing()

print("=== BSP/Zone Function Search ===")
func_iter = listing.getFunctions(True)
count = 0

for func in func_iter:
    name = func.getName().lower()
    if any(x in name for x in ['bsp', 'zone', 'leaf', 'pointregion', 'model']):
        print("FUNC: {} at {}".format(func.getName(), func.getEntryPoint()))
        count += 1

print("\nFound {} BSP/zone functions".format(count))
print("\n=== Done ===")
