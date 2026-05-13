-- BS1SDK Autorun Script
-- This script runs automatically when Lua engine is initialized.
-- Place in: <GameDir>\BS1SDK_Scripts\autorun.lua

sdk.log("=== BS1SDK Lua Engine Loaded ===")

-- Example: Read player stats
local function showStats()
    local adam = sdk.getProperty("ShockPlayer", "ADAM")
    local credits = sdk.getProperty("ShockPlayer", "Credits")
    local health = sdk.getProperty("ShockPlayer", "Health")
    local eve = sdk.getProperty("ShockPlayer", "BioAmmo")
    
    if adam then
        sdk.log(string.format("Player Stats: ADAM=%d Credits=%d Health=%.0f EVE=%.1f",
            adam, credits or 0, health or 0, eve or 0))
    else
        sdk.log("Player not spawned yet")
    end
end

-- Example: Give player resources
function giveAll(amount)
    amount = amount or 999
    sdk.setProperty("ShockPlayer", "ADAM", amount)
    sdk.setProperty("ShockPlayer", "Credits", amount)
    sdk.setProperty("ShockPlayer", "BioAmmo", 100.0)
    sdk.setProperty("ShockPlayer", "Health", 500.0)
    sdk.log("Gave all resources: " .. tostring(amount))
end

-- Example: God mode via ProcessEvent hook
-- Blocks the TakeDamage function
local godHookId = nil
function toggleGod()
    if godHookId then
        sdk.off(godHookId)
        godHookId = nil
        sdk.log("God mode OFF")
    else
        sdk.hookpe()
        godHookId = sdk.on("TakeDamage", function(objName, funcName)
            -- Block damage on the player
            if objName == "ShockPlayer_0" then
                return true  -- block the call
            end
            return false
        end)
        sdk.log("God mode ON (blocking TakeDamage)")
    end
end

-- Example: Log specific events
function watchEvents(filter)
    filter = filter or "Tick"
    sdk.hookpe()
    local id = sdk.on(filter, function(objName, funcName)
        sdk.log("EVENT: " .. objName .. "." .. funcName)
        return false
    end)
    sdk.log("Watching events: " .. filter .. " (hook id: " .. tostring(id) .. ")")
    return id
end

-- Show initial stats
showStats()

sdk.log("Available functions: giveAll(n), toggleGod(), watchEvents(filter)")
sdk.log("Type 'lua showStats()' in console to check stats")
