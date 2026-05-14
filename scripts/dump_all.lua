-- dump_all.lua — One-shot comprehensive data dump for post-test analysis
-- Writes everything to SDK log. Run after each test session.

sdk.log("═══════════════════════════════════════")
sdk.log("  FULL DATA DUMP — " .. os.date("%Y-%m-%d %H:%M:%S"))
sdk.log("═══════════════════════════════════════")

-- Engine state
sdk.log("")
sdk.log("─── Engine ───")
sdk.log(string.format("  Tick rate: %.1f FPS", sdk.getTickRate()))
sdk.log(string.format("  Natives: %d", sdk.getNativeCount()))

local info = sdk.getLevelInfo()
sdk.log(string.format("  Level: %s", info.name))
sdk.log(string.format("  Actors: %d", info.actorCount))

-- Player
sdk.log("")
sdk.log("─── Player ───")
local px, py, pz = sdk.getPlayerPos()
if px then
    sdk.log(string.format("  Position: (%.1f, %.1f, %.1f)", px, py, pz))
    local hp = sdk.getProperty("ShockPlayer", "Health")
    if hp then sdk.log(string.format("  Health: %.0f", hp)) end
    local adam = sdk.getProperty("ShockPlayer", "ADAM")
    if adam then sdk.log(string.format("  ADAM: %d", adam)) end
end

-- All actors grouped by class
sdk.log("")
sdk.log("─── All Actors By Class ───")
local actors = sdk.getActors()
local byClass = {}
for _, a in ipairs(actors) do
    byClass[a.class] = byClass[a.class] or {}
    table.insert(byClass[a.class], a)
end

local classes = {}
for cls, list in pairs(byClass) do
    table.insert(classes, {cls = cls, count = #list, actors = list})
end
table.sort(classes, function(a, b) return a.count > b.count end)

for _, c in ipairs(classes) do
    sdk.log(string.format("  %4dx %s", c.count, c.cls))
end

-- Functions on key classes
sdk.log("")
sdk.log("─── Key Class Functions ───")
local keyClasses = {"ShockPlayer", "ShockAI", "AIController", "Pawn", "GameInfo"}
for _, cls in ipairs(keyClasses) do
    local funcs = sdk.getFunctions(cls)
    if funcs then
        local tickFuncs = {}
        for _, f in ipairs(funcs) do
            local fn = f.name:lower()
            if fn:find("tick") or fn:find("timer") or fn:find("think")
                or fn:find("spawn") or fn:find("damage") then
                table.insert(tickFuncs, f)
            end
        end
        if #tickFuncs > 0 then
            sdk.log(string.format("  %s (%d tick/timer funcs):", cls, #tickFuncs))
            for _, f in ipairs(tickFuncs) do
                local tags = ""
                if f.isNative then tags = tags .. " [Native]" end
                if f.isEvent then tags = tags .. " [Event]" end
                sdk.log(string.format("    %s (flags=0x%04X)%s", f.name, f.flags, tags))
            end
        end
    end
end

-- Enums
sdk.log("")
sdk.log("─── Key Enums ───")
local enums = {"EPhysics", "ENetRole", "ENetMode", "EMovementMode"}
for _, name in ipairs(enums) do
    local vals = sdk.getEnum(name)
    if vals then
        sdk.log(string.format("  %s: %d values", name, #vals))
        for i, v in ipairs(vals) do
            sdk.log(string.format("    %d = %s", i-1, v))
        end
    end
end

sdk.log("")
sdk.log("═══ DUMP COMPLETE ═══")
