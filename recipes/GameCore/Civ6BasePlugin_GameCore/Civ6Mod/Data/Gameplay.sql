-- 本文件同时覆盖两组示范数据：
--   A. 两个自定义 Requirement（玩家动态属性阈值 / 地块道路类型）
--   B. 一个自定义 Effect（EFFECT_ADJUST_PLOT_PROPERTY，改地块动态属性）
-- 依赖对应插件模块已注册类型；模块没加载时这些数据行不起作用，但不会报错。

--------------------------------------------------------------------------
-- A1. 玩家属性阈值 Requirement：挂在领袖特性上，阈值 1 / 2 各给 +1 建造者次数
--------------------------------------------------------------------------
INSERT OR IGNORE INTO TraitModifiers (TraitType, ModifierId) VALUES
('TRAIT_LEADER_MAJOR_CIV', 'MODIFIER_EXAMPLE_BUILDER_CHARGE_1'),
('TRAIT_LEADER_MAJOR_CIV', 'MODIFIER_EXAMPLE_BUILDER_CHARGE_2');

INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, RunOnce, Permanent, NewOnly, OwnerRequirementSetId, SubjectRequirementSetId) VALUES
('MODIFIER_EXAMPLE_BUILDER_CHARGE_1', 'MODIFIER_PLAYER_UNITS_ADJUST_BUILDER_CHARGES', 0, 0, 0, 'REQSET_EXAMPLE_HAS_PROPERTY_1', 'UNIT_IS_BUILDER'),
('MODIFIER_EXAMPLE_BUILDER_CHARGE_2', 'MODIFIER_PLAYER_UNITS_ADJUST_BUILDER_CHARGES', 0, 0, 0, 'REQSET_EXAMPLE_HAS_PROPERTY_2', 'UNIT_IS_BUILDER');

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value) VALUES
('MODIFIER_EXAMPLE_BUILDER_CHARGE_1', 'Amount', '1'),
('MODIFIER_EXAMPLE_BUILDER_CHARGE_2', 'Amount', '1');

INSERT OR IGNORE INTO RequirementSets (RequirementSetId, RequirementSetType) VALUES
('REQSET_EXAMPLE_HAS_PROPERTY_1', 'REQUIREMENTSET_TEST_ALL'),
('REQSET_EXAMPLE_HAS_PROPERTY_2', 'REQUIREMENTSET_TEST_ALL');

INSERT OR IGNORE INTO RequirementSetRequirements (RequirementSetId, RequirementId) VALUES
('REQSET_EXAMPLE_HAS_PROPERTY_1', 'REQ_EXAMPLE_HAS_PROPERTY_1'),
('REQSET_EXAMPLE_HAS_PROPERTY_2', 'REQ_EXAMPLE_HAS_PROPERTY_2');

INSERT OR IGNORE INTO Requirements (RequirementId, RequirementType) VALUES
('REQ_EXAMPLE_HAS_PROPERTY_1', 'REQUIREMENT_PLAYER_PROPERTY_MATCHES'),
('REQ_EXAMPLE_HAS_PROPERTY_2', 'REQUIREMENT_PLAYER_PROPERTY_MATCHES');

INSERT OR IGNORE INTO RequirementArguments (RequirementId, Name, Value) VALUES
('REQ_EXAMPLE_HAS_PROPERTY_1', 'PropertyName', 'MY_TEST_PROP'),
('REQ_EXAMPLE_HAS_PROPERTY_1', 'PropertyMinimum', '1'),
('REQ_EXAMPLE_HAS_PROPERTY_2', 'PropertyName', 'MY_TEST_PROP'),
('REQ_EXAMPLE_HAS_PROPERTY_2', 'PropertyMinimum', '2');

--------------------------------------------------------------------------
-- A2. 道路类型 Requirement：远古道路匹配时农田 +10 金币
--------------------------------------------------------------------------
INSERT OR IGNORE INTO ImprovementModifiers (ImprovementType, ModifierId) VALUES
('IMPROVEMENT_FARM', 'MODIFIER_EXAMPLE_ROAD_GOLD'),
('IMPROVEMENT_FARM', 'MODIFIER_EXAMPLE_ADJUST_PLAYER_PROPERTY');

INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, RunOnce, Permanent, NewOnly, OwnerRequirementSetId, SubjectRequirementSetId) VALUES
('MODIFIER_EXAMPLE_ROAD_GOLD', 'MODIFIER_SINGLE_PLOT_ADJUST_PLOT_YIELDS', 0, 0, 0, NULL, 'REQSET_EXAMPLE_ROUTE_TYPE_ROAD'),
('MODIFIER_EXAMPLE_ADJUST_PLAYER_PROPERTY', 'MODIFIER_PLAYER_ADJUST_PROPERTY', 0, 0, 0, NULL, NULL);

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value) VALUES
('MODIFIER_EXAMPLE_ROAD_GOLD', 'YieldType', 'YIELD_GOLD'),
('MODIFIER_EXAMPLE_ROAD_GOLD', 'Amount', '10'),
('MODIFIER_EXAMPLE_ADJUST_PLAYER_PROPERTY', 'Key', 'MY_TEST_PROP'),
('MODIFIER_EXAMPLE_ADJUST_PLAYER_PROPERTY', 'Amount', '1');

INSERT OR IGNORE INTO RequirementSets (RequirementSetId, RequirementSetType) VALUES
('REQSET_EXAMPLE_ROUTE_TYPE_ROAD', 'REQUIREMENTSET_TEST_ALL');

INSERT OR IGNORE INTO RequirementSetRequirements (RequirementSetId, RequirementId) VALUES
('REQSET_EXAMPLE_ROUTE_TYPE_ROAD', 'REQ_EXAMPLE_ROUTE_TYPE_ROAD');

INSERT OR IGNORE INTO Requirements (RequirementId, RequirementType) VALUES
('REQ_EXAMPLE_ROUTE_TYPE_ROAD', 'REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES');

INSERT OR IGNORE INTO RequirementArguments (RequirementId, Name, Value) VALUES
('REQ_EXAMPLE_ROUTE_TYPE_ROAD', 'RouteType', 'ROUTE_ANCIENT_ROAD');

--------------------------------------------------------------------------
-- B. 自定义 Effect：EFFECT_ADJUST_PLOT_PROPERTY
--    先登记 Types / DynamicModifiers，再挂一个给农田加 EXAMPLE_PROPERTY 的 Modifier。
--------------------------------------------------------------------------
INSERT OR IGNORE INTO Types (Type, Kind) VALUES
('EFFECT_ADJUST_PLOT_PROPERTY', 'KIND_EFFECT'),
('MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY', 'KIND_MODIFIER');

INSERT OR IGNORE INTO DynamicModifiers (ModifierType, CollectionType, EffectType) VALUES
('MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY', 'COLLECTION_OWNER', 'EFFECT_ADJUST_PLOT_PROPERTY');

INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, RunOnce, Permanent) VALUES
('MODIFIER_TEST_FARM_PROPERTY', 'MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY', 0, 0);

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value) VALUES
('MODIFIER_TEST_FARM_PROPERTY', 'Key', 'EXAMPLE_PROPERTY'),
('MODIFIER_TEST_FARM_PROPERTY', 'Amount', '2');

INSERT OR IGNORE INTO ImprovementModifiers (ImprovementType, ModifierId) VALUES
('IMPROVEMENT_FARM', 'MODIFIER_TEST_FARM_PROPERTY');