-- debug_coop.lua — Quick diagnostic script for co-op development
-- Run with: lua debug_coop.lua (or luafile debug_coop.lua in console)

sdk.log("═══ Co-op Debug Script Starting ═══")

-- 1. Level info
local info = sdk.getLevelInfo()
sdk.log(string.format("Level: %s  Actors: %d", info.name, info.actorCount))

-- 2. Player position
local x, y, z = sdk.getPlayerPos()
if x then
    sdk.log(string.format("Player pos: (%.0f, %.0f, %.0f)", x, y, z))
else
    sdk.log("WARNING: Player position unavailable")
end

-- 3. Tick rate
sdk.log(string.format("Tick rate: %.1f FPS", sdk.getTickRate()))

-- 4. Actor class breakdown (top 10)
local actors = sdk.getActors()
local classCounts = {}
for _, a in ipairs(actors) do
    classCounts[a.class] = (classCounts[a.class] or 0) + 1
end

-- Sort by count
local sorted = {}
for cls, count in pairs(classCounts) do
    table.insert(sorted, {cls = cls, count = count})
end
table.sort(sorted, function(a, b) return a.count > b.count end)

sdk.log("─── Top 10 Actor Classes ───")
for i = 1, math.min(10, #sorted) do
    sdk.log(string.format("  %4dx %s", sorted[i].count, sorted[i].cls))
end

-- 5. Nearby enemies (within 2000 units)
if x then
    local nearby = sdk.getActorsInRadius(x, y, z, 2000)
    local enemies = {}
    for _, a in ipairs(nearby) do
        -- Filter for pawn-like classes
        local cls = a.class
        if cls:find("Thug") or cls:find("Aggressor") or cls:find("Crawler")
            or cls:find("Assassin") or cls:find("Grenadier") or cls:find("BigDaddy")
            or cls:find("SecurityBot") or cls:find("Turret") then
            table.insert(enemies, a)
        end
    end
    sdk.log(string.format("─── Nearby enemies (%d within 2000u) ───", #enemies))
    for _, e in ipairs(enemies) do
        local dist = 0
        if e.x and x then
            local dx, dy, dz = e.x - x, e.y - y, e.z - z
            dist = math.sqrt(dx*dx + dy*dy + dz*dz)
        end
        sdk.log(string.format("  [%s] %s (%.0f units)", e.class, e.name, dist))
    end
end

-- 6. Native function stats
sdk.log(string.format("GNatives: %d functions populated", sdk.getNativeCount()))

sdk.log("═══ Co-op Debug Script Complete ═══")
