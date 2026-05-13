-- BS1SDK Weapon Modification Scripts
-- Usage: lua dofile("weapon_mods.lua") or place in BS1SDK_Scripts/

sdk.log("Loading weapon_mods.lua...")

-- Modify a weapon's base stats
function modWeapon(weaponClass, stat, value)
    local ok, err = sdk.setProperty(weaponClass, stat, value)
    if ok then
        sdk.log(weaponClass .. "." .. stat .. " = " .. tostring(value))
    else
        sdk.log("Failed: " .. tostring(err))
    end
end

-- Quick presets
function pistolRapidFire()
    modWeapon("Pistol", "BaseFireRate", 10.0)
    modWeapon("Pistol", "BaseMagazineSize", 99)
    modWeapon("Pistol", "RoundsRemaining", 99)
    sdk.log("Pistol: Rapid Fire mode enabled")
end

function shotgunPower()
    modWeapon("Shotgun", "BaseFireRate", 5.0)
    modWeapon("Shotgun", "BaseMagazineSize", 50)
    modWeapon("Shotgun", "RoundsRemaining", 50)
    modWeapon("Shotgun", "BaseAccuracy", 0.1)
    sdk.log("Shotgun: Power mode enabled")
end

function infiniteAmmoAll()
    local weapons = {"Pistol", "Shotgun", "MachineGun", "ChemThrower", "Crossbow", "GrenadeLauncher"}
    for _, w in ipairs(weapons) do
        modWeapon(w, "RoundsRemaining", 9999)
        modWeapon(w, "BaseMagazineSize", 9999)
    end
    sdk.log("All weapons: Infinite ammo")
end

function resetWeapon(weaponClass)
    modWeapon(weaponClass, "BaseFireRate", 1.0)
    modWeapon(weaponClass, "BaseAccuracy", 1.0)
    modWeapon(weaponClass, "BaseReloadRate", 1.0)
    sdk.log(weaponClass .. ": Reset to defaults")
end

sdk.log("weapon_mods.lua loaded!")
sdk.log("Functions: pistolRapidFire(), shotgunPower(), infiniteAmmoAll(), resetWeapon(class)")
