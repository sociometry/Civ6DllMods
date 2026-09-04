-- Custom effect + smoke-test modifier for farms.
-- DynamicModifiers.ModifierType MUST exist in Types as KIND_MODIFIER.

INSERT OR IGNORE INTO Types (Type, Kind) VALUES
('EFFECT_ADJUST_PLOT_PROPERTY', 'KIND_EFFECT'),
('MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY', 'KIND_MODIFIER');

INSERT OR IGNORE INTO DynamicModifiers (ModifierType, CollectionType, EffectType)
VALUES (
  'MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY',
  'COLLECTION_OWNER',
  'EFFECT_ADJUST_PLOT_PROPERTY'
);

INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, RunOnce, Permanent)
VALUES ('MODIFIER_TEST_FARM_PROPERTY', 'MODIFIER_EXAMPLE_PLOT_ADJUST_PROPERTY', 0, 0);

-- Adjust*Property Initialize expects "Key" / "Amount" (case-sensitive).
INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value) VALUES
('MODIFIER_TEST_FARM_PROPERTY', 'Key', 'EXAMPLE_PROPERTY'),
('MODIFIER_TEST_FARM_PROPERTY', 'Amount', '2');

INSERT OR IGNORE INTO ImprovementModifiers (ImprovementType, ModifierId)
VALUES ('IMPROVEMENT_FARM', 'MODIFIER_TEST_FARM_PROPERTY');
