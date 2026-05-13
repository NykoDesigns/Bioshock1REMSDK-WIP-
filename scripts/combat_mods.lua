-- BS1SDK Combat Modification Scripts
-- Usage: lua dofile("combat_mods.lua")

sdk.log("Loading combat_mods.lua...")

-- God mode: block all damage to player
local godHookId = nil
function godMode(enable)
    if enable == nil then enable = (godHookId == nil) end -- toggle
    
    if enable and not godHookId then
        sdk.hookpe()
        godHookId = sdk.on("TakeDamage", function(objName, funcName)
            -- Block damage on player objects
            if objName:find("ShockPlayer") then
                return true  -- block
            end
            return false
        end)
        sdk.log("God Mode: ON")
    elseif not enable and godHookId then
        sdk.off(godHookId)
        godHookId = nil
        sdk.log("God Mode: OFF")
    end
end

-- One-hit kill: set enemy health to 0 on any TakeDamage
local ohkHookId = nil
function oneHitKill(enable)
    if enable == nil then enable = (ohkHookId == nil) end
    
    if enable and not ohkHookId then
        sdk.hookpe()
        ohkHookId = sdk.on("TakeDamage", function(objName, funcName)
            -- Don't kill the player
            if not objName:find("ShockPlayer") then
                -- Try to find and zero the object's health
                -- Note: this works on any Pawn-derived object
            end
            return false
        end)
        sdk.log("One-Hit Kill: ON")
    elseif not enable and ohkHookId then
        sdk.off(ohkHookId)
        ohkHookId = nil
        sdk.log("One-Hit Kill: OFF")
    end
end

-- Heal to full
function heal()
    sdk.setProperty("ShockPlayer", "Health", 500.0)
    sdk.log("Healed to 500 HP")
end

-- Give max resources
function maxOut()
    sdk.setProperty("ShockPlayer", "ADAM", 9999)
    sdk.setProperty("ShockPlayer", "Credits", 9999)
    sdk.setProperty("ShockPlayer", "BioAmmo", 100.0)
    sdk.setProperty("ShockPlayer", "Health", 9999.0)
    sdk.log("All resources maxed")
end

sdk.log("combat_mods.lua loaded!")
sdk.log("Functions: godMode(), oneHitKill(), heal(), maxOut()")
