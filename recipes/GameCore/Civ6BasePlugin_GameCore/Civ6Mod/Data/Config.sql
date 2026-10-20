-- 把 Expansion2 的 GameCore 槽位重定向到薄壳，从而让 Loader 进场。
-- PackageId 必须与本 modinfo 的 <Mod id> 一致。
UPDATE GameCores
SET
    PackageId = 'b7e4c1a9-3f52-4d80-9e61-2a8c5d4f7b03',
    DllPrefix = 'GameCore_XP2_Shell'
WHERE
    GameCore = 'Expansion2';