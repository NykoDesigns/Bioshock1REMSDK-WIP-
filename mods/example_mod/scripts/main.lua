-- Example Mod for BS1SDK
-- Demonstrates the scripting API

-- Called when the mod is loaded
function OnLoad()
    print("[ExampleMod] Loaded!")
    
    -- Register a tick callback
    SDK.RegisterTick(OnTick)
    
    -- Register a console command
    SDK.RegisterCommand("hello", function(args)
        print("Hello, BioShock modding world!")
    end)
end

-- Called every game frame
function OnTick(deltaTime)
    -- Example: check for a key press
    if SDK.IsKeyPressed(SDK.Keys.F5) then
        print("[ExampleMod] F5 pressed!")
        DumpPlayerInfo()
    end
end

-- Example: dump player information
function DumpPlayerInfo()
    local player = SDK.GetPlayer()
    if player then
        print(string.format("  Position: %.1f, %.1f, %.1f", 
            player.x, player.y, player.z))
        print(string.format("  Health: %d / %d", 
            player.health, player.maxHealth))
        print(string.format("  EVE: %d / %d",
            player.eve, player.maxEve))
    else
        print("  Player not available")
    end
end

-- Called when the mod is about to be unloaded
function OnUnload()
    print("[ExampleMod] Unloading...")
end
