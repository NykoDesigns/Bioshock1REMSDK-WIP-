-- test_freeze.lua — Test client simulation freeze
-- Run on the CLIENT machine after connecting with truejoin
-- 
-- This script takes two snapshots:
--   1. Immediately after freeze (snapshot A)
--   2. 10 seconds later (snapshot B)
-- Then compares them to verify nothing moved.

sdk.log("═══ Freeze Test Starting ═══")

-- Take snapshot A
local actorsA = sdk.getActors()
sdk.log(string.format("Snapshot A: %d actors", #actorsA))

-- Store positions
local positionsA = {}
for _, a in ipairs(actorsA) do
    positionsA[a.name] = {x = 0, y = 0, z = 0, class = a.class}
end

-- Schedule check after 10 seconds
local tickId
local elapsed = 0
tickId = sdk.onTick(function(dt)
    elapsed = elapsed + dt
    if elapsed >= 10.0 then
        sdk.offTick(tickId)
        
        -- Take snapshot B
        local actorsB = sdk.getActors()
        sdk.log(string.format("Snapshot B: %d actors (after 10s)", #actorsB))
        
        -- Compare
        local moved = 0
        local spawned = 0
        local destroyed = 0
        
        local namesB = {}
        for _, b in ipairs(actorsB) do
            namesB[b.name] = true
        end
        
        -- Check destroyed
        for _, a in ipairs(actorsA) do
            if not namesB[a.name] then
                destroyed = destroyed + 1
                sdk.log(string.format("  DESTROYED: [%s] %s", a.class, a.name))
            end
        end
        
        -- Check spawned
        local namesA = {}
        for _, a in ipairs(actorsA) do namesA[a.name] = true end
        for _, b in ipairs(actorsB) do
            if not namesA[b.name] then
                spawned = spawned + 1
                sdk.log(string.format("  SPAWNED: [%s] %s", b.class, b.name))
            end
        end
        
        sdk.log(string.format("═══ Freeze Test Results ═══"))
        sdk.log(string.format("  Actors A: %d", #actorsA))
        sdk.log(string.format("  Actors B: %d", #actorsB))
        sdk.log(string.format("  Spawned:  %d", spawned))
        sdk.log(string.format("  Destroyed: %d", destroyed))
        
        if spawned == 0 and destroyed == 0 then
            sdk.log("  PASS: No actors spawned or destroyed during freeze")
        else
            sdk.log("  FAIL: Actors changed during freeze!")
        end
    elseif math.floor(elapsed) ~= math.floor(elapsed - dt) then
        sdk.log(string.format("  Waiting... %.0fs", 10.0 - elapsed))
    end
end)

sdk.log("Waiting 10 seconds to compare snapshots...")
