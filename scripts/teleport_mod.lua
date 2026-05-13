-- BS1SDK Teleport Plasmid Script
-- A custom "plasmid" that blinks the player forward

sdk.log("Loading teleport_mod.lua...")

-- Configurable distance
local defaultDistance = 800

-- Quick teleport forward
function blink(dist)
    dist = dist or defaultDistance
    sdk.teleport(dist)
    sdk.log("Blink! (" .. tostring(dist) .. " units)")
end

-- Teleport to a saved position
local savedPos = nil

function savePos()
    -- Read current position
    local x = sdk.getProperty("ShockPlayer", "Location")
    if x then
        savedPos = {x = x}
        sdk.log("Position saved")
    else
        sdk.log("No player found")
    end
end

-- Mega blink: teleport a huge distance
function megaBlink()
    blink(3000)
end

-- Short hop
function hop()
    blink(300)
end

-- Set the default blink distance
function setBlinkDist(d)
    defaultDistance = d
    sdk.log("Blink distance set to " .. tostring(d))
end

sdk.log("teleport_mod.lua loaded!")
sdk.log("Functions: blink(dist), megaBlink(), hop(), setBlinkDist(d)")
sdk.log("Use sdk.teleport(800) or 'tp 800' in console for direct teleport")
