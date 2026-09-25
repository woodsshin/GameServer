#include "AbilitySystem/KiraverseGameplayTags.h"

namespace KiraverseGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Team_Attackers, "Team.Attackers", "Granted to every attacker on team assignment.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Team_Defenders, "Team.Defenders", "Granted to every defender on team assignment.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Jump, "Ability.Jump", "Grants and activates the jump ability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Dash, "Ability.Dash", "Grants and activates the dash ability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Fire, "Ability.Fire", "Shared tag for every weapon fire ability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Zoom, "Ability.Zoom", "Grants and activates the zoom ability.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Bomb_PickUp, "Ability.Bomb.PickUp", "Picks up a dropped or site-spawned bomb.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Bomb_Drop, "Ability.Bomb.Drop", "Drops the currently carried bomb.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Bomb_Plant, "Ability.Bomb.Plant", "Channeled plant at a bomb site; attackers only.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Bomb_Defuse, "Ability.Bomb.Defuse", "Channeled defuse of a planted bomb; defenders only.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Dash, "Cooldown.Dash", "Blocks Dash while its cooldown effect is active.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_BombAction, "Cooldown.BombAction", "Blocks re-activating any bomb ability while one is mid-channel.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dashing, "State.Dashing", "Active for the short duration of a dash.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Firing, "State.Firing", "Active while an auto-fire loop is running.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Zooming, "State.Zooming", "Active while the zoom ability is held.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Planting, "State.Planting", "Active while a plant channel is running.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Defusing, "State.Defusing", "Active while a defuse channel is running.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_CarryingBomb, "State.CarryingBomb", "Active while this character holds the bomb.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Dead", "Granted on death; blocks every ability for the rest of the round.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Damage", "SetByCaller key for runtime damage magnitude.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Cost_Ammo, "Data.Cost.Ammo", "SetByCaller key for per-shot ammo cost.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Cost_Stamina, "Data.Cost.Stamina", "SetByCaller key for per-shot stamina cost.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Weapon_Hitscan_Single, "Weapon.Hitscan.Single", "Single-shot hitscan weapon.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Weapon_Hitscan_Auto, "Weapon.Hitscan.Auto", "Full-auto hitscan weapon.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Weapon_Projectile, "Weapon.Projectile", "Projectile-based weapon.");
}
