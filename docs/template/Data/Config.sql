-- Replace YOUR-MOD-GUID with the same id as YourDllMod.modinfo
UPDATE GameCores
SET
    PackageId = '00000000-0000-0000-0000-000000000001',
    DllPrefix = 'GameCore_XP2_Shell'
WHERE
    GameCore = 'Expansion2';
