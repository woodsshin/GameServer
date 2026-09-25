#pragma once

#include "NativeGameplayTags.h"

// Central registry of every native Gameplay Tag used across Kiraverse.
namespace KiraverseGameplayTags
{
	// Team identity tags, granted by GameMode and used to gate Plant/Defuse.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Team_Attackers);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Team_Defenders);

	// Ability identifier tags, used to activate abilities by tag from input.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Jump);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Dash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Fire);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Zoom);

	// Bomb ability tags: pick up, drop, plant, defuse.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Bomb_PickUp);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Bomb_Drop);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Bomb_Plant);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Bomb_Defuse);

	// Applied as a duration GameplayEffect right after a dash starts.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Dash);

	// Blocks starting a second bomb channel while one is already running.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_BombAction);

	// State tags added/removed while an ability is running, used to gate other abilities.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dashing);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Firing);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Zooming);

	// Active while a plant or defuse channel is running.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Planting);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Defusing);

	// Active while this character is carrying the bomb.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_CarryingBomb);

	// Granted on death; blocks every ability and marks the character as out of the round.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);

	// SetByCaller keys: magnitudes injected at runtime by the firing ability.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Cost_Ammo);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Cost_Stamina);

	// Weapon category tags, useful for UI, inventory filtering and analytics.
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Weapon_Hitscan_Single);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Weapon_Hitscan_Auto);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Weapon_Projectile);
}
