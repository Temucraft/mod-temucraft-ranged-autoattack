/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * mod-temucraft-ranged-autoattack: the hunter's right-click Auto Shot for every class.
 *
 *  - Right-click a target that is out of melee range while holding a wand, thrown weapon, bow, gun or crossbow,
 *    and the melee auto-attack turns into the matching auto-repeat ranged spell (Shoot, Auto Throw, Auto Shot).
 *  - Close range: per character, either switch to melee while the target is in melee range and back to the
 *    ranged auto-attack when it leaves (default), or keep shooting.
 *  - Optionally gives every player "Requires No Ammo" (46699), which the core already honours.
 *
 * The spells come from retail_changes.sql (95001 Auto Throw, 95002 Auto Shot for the other classes). Any known
 * auto-repeat ranged spell that fits the weapon is used when the configured one is not known, so custom spells
 * work without a config change. The core change RangedAutoAttack.LikeAutoShot lets them pause for casts and
 * movement instead of being cancelled.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* SETTING_SOURCE = "mod-temucraft-ranged-autoattack";

    enum SettingIndex : uint8
    {
        SETTING_RIGHT_CLICK = 0, // 0 = server default, 1 = on, 2 = off
        SETTING_CLOSE_MELEE = 1  // 0 = server default, 1 = switch to melee in melee range, 2 = keep shooting
    };

    constexpr uint32 SPELL_REQUIRES_NO_AMMO = 46699;

    struct Settings
    {
        bool Enable = true;
        bool Announce = true;
        bool RightClick = true;
        bool CloseMelee = true;
        bool NoAmmo = true;
        bool Bots = false;
        uint32 Interval = 250;
        uint32 SpellWand = 5019;
        uint32 SpellThrown = 95001;
        uint32 SpellRanged = 95002;
        uint32 SpellHunter = 75;
    } settings;

    struct RangedState : public DataMap::Base
    {
        uint32 Timer = 0;
        uint32 RetryTimer = 0;
        bool SwappedToMelee = false;   // we switched this ranged auto-attack to melee at close range
        bool StartedByModule = false;  // the running auto-repeat spell was started by the module
        uint32 CachedItemEntry = 0;
        uint32 CachedSpell = 0;
        uint32 CacheAge = 0;
    };

    bool IsBot(Player* player)
    {
        WorldSession* session = player->GetSession();
        return !session || session->IsSocketClosed();
    }

    bool PlayerOption(Player* player, SettingIndex index, bool serverDefault)
    {
        switch (player->GetPlayerSetting(SETTING_SOURCE, index).value)
        {
            case 1: return true;
            case 2: return false;
            default: return serverDefault;
        }
    }

    bool SpellFitsWeapon(SpellInfo const* info, ItemTemplate const* proto)
    {
        if (!info || !info->IsAutoRepeatRangedSpell())
            return false;

        if (info->EquippedItemClass != ITEM_CLASS_WEAPON)
            return false;

        return info->EquippedItemSubClassMask <= 0 || (info->EquippedItemSubClassMask & (1 << proto->SubClass));
    }

    uint32 ConfiguredSpell(Player* player, ItemTemplate const* proto)
    {
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_WAND:
                return settings.SpellWand;
            case ITEM_SUBCLASS_WEAPON_THROWN:
                return settings.SpellThrown;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return player->getClass() == CLASS_HUNTER ? settings.SpellHunter : settings.SpellRanged;
            default:
                return 0;
        }
    }

    // The auto-repeat spell for the equipped ranged weapon, or 0
    uint32 RangedSpellFor(Player* player, Item* weapon, RangedState* state)
    {
        ItemTemplate const* proto = weapon->GetTemplate();

        if (state->CachedItemEntry == proto->ItemId && state->CacheAge < 5000)
            return state->CachedSpell;

        uint32 spellId = ConfiguredSpell(player, proto);
        if (!spellId || !player->HasActiveSpell(spellId) || !SpellFitsWeapon(sSpellMgr->GetSpellInfo(spellId), proto))
        {
            spellId = 0;
            for (auto const& [id, spell] : player->GetSpellMap())
            {
                if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                    continue;

                if (SpellFitsWeapon(sSpellMgr->GetSpellInfo(id), proto))
                {
                    spellId = id;
                    break;
                }
            }
        }

        state->CachedItemEntry = proto->ItemId;
        state->CachedSpell = spellId;
        state->CacheAge = 0;
        return spellId;
    }

    void Tick(Player* player, RangedState* state)
    {
        if (settings.NoAmmo && player->IsAlive() && !player->HasAura(SPELL_REQUIRES_NO_AMMO))
            player->AddAura(SPELL_REQUIRES_NO_AMMO, player);

        if (!player->IsAlive() || player->IsInFlight() || player->HasUnitState(UNIT_STATE_CONTROLLED))
            return;

        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!weapon || weapon->IsBroken())
            return;

        uint32 const spellId = RangedSpellFor(player, weapon, state);
        if (!spellId)
            return;

        bool const closeMelee = PlayerOption(player, SETTING_CLOSE_MELEE, settings.CloseMelee);

        // Shooting: switch to melee once the target is in melee range
        if (Spell* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            Unit* target = autoRepeat->m_targets.GetUnitTarget();

            // The client does not know about an auto-repeat spell the server started, so it cannot cancel it:
            // stop it when the player selects something else
            if (state->StartedByModule && (!target || player->GetTarget() != target->GetGUID()))
            {
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
                state->StartedByModule = false;
                return;
            }

            if (closeMelee && target && target->IsAlive() && player->IsWithinMeleeRange(target) && player->IsValidAttackTarget(target))
            {
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
                state->StartedByModule = false;
                if (player->Attack(target, true))
                    state->SwappedToMelee = true;
            }

            return;
        }

        state->StartedByModule = false;

        // Melee: switch to the ranged auto-attack when the target is out of melee range
        Unit* victim = player->GetVictim();
        if (!victim)
        {
            state->SwappedToMelee = false;
            return;
        }

        if (player->IsWithinMeleeRange(victim))
            return;

        if (!state->SwappedToMelee && !PlayerOption(player, SETTING_RIGHT_CLICK, settings.RightClick))
            return;

        if (state->RetryTimer || player->IsNonMeleeSpellCast(false))
            return;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !victim->IsAlive() || !player->IsValidAttackTarget(victim))
            return;

        // Moving only stops spells that still have the movement interrupt flag (section 32 clears it)
        if (player->isMoving() && (info->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
            return;

        if (!player->IsWithinCombatRange(victim, info->GetMaxRange(false, player)) || !player->IsWithinLOSInMap(victim))
            return;

        player->AttackStop();
        if (player->CastSpell(victim, spellId, false) == SPELL_CAST_OK && player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            state->StartedByModule = true;
            state->SwappedToMelee = false;
        }
        else
        {
            // Could not shoot: go back to melee and try again a little later
            player->Attack(victim, true);
            state->RetryTimer = 2000;
        }
    }

    void LoadSettings()
    {
        settings.Enable = sConfigMgr->GetOption<bool>("RangedAutoAttack.Enable", true);
        settings.Announce = sConfigMgr->GetOption<bool>("RangedAutoAttack.Announce", true);
        settings.RightClick = sConfigMgr->GetOption<bool>("RangedAutoAttack.RightClick", true);
        settings.CloseMelee = sConfigMgr->GetOption<bool>("RangedAutoAttack.CloseRange.Melee", true);
        settings.NoAmmo = sConfigMgr->GetOption<bool>("RangedAutoAttack.NoAmmo", true);
        settings.Bots = sConfigMgr->GetOption<bool>("RangedAutoAttack.Bots", false);
        settings.Interval = std::max<uint32>(100, sConfigMgr->GetOption<uint32>("RangedAutoAttack.Interval", 250));
        settings.SpellWand = sConfigMgr->GetOption<uint32>("RangedAutoAttack.Spell.Wand", 5019);
        settings.SpellThrown = sConfigMgr->GetOption<uint32>("RangedAutoAttack.Spell.Thrown", 95001);
        settings.SpellRanged = sConfigMgr->GetOption<uint32>("RangedAutoAttack.Spell.Ranged", 95002);
        settings.SpellHunter = sConfigMgr->GetOption<uint32>("RangedAutoAttack.Spell.Hunter", 75);
    }
}

class RangedAutoAttack_World : public WorldScript
{
public:
    RangedAutoAttack_World() : WorldScript("RangedAutoAttack_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
    }

    void OnStartup() override
    {
        if (!settings.Enable)
            return;

        if (!sConfigMgr->GetOption<bool>("RangedAutoAttack.LikeAutoShot", false, false))
            LOG_WARN("module", "mod-temucraft-ranged-autoattack: RangedAutoAttack.LikeAutoShot is off in worldserver.conf, so wand Shoot, "
                "Auto Throw and the other new auto-attacks are cancelled by casting and moving instead of pausing.");

        if (!sConfigMgr->GetOption<bool>("EnablePlayerSettings", false, false))
            LOG_INFO("module", "mod-temucraft-ranged-autoattack: EnablePlayerSettings is off, so .rangedauto choices last until logout.");

        for (uint32 spellId : { settings.SpellWand, settings.SpellThrown, settings.SpellRanged, settings.SpellHunter })
        {
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (spellId && (!info || !info->IsAutoRepeatRangedSpell()))
                LOG_WARN("module", "mod-temucraft-ranged-autoattack: spell {} is not an auto-repeat ranged spell in spell_dbc. Apply retail_changes.sql "
                    "or change RangedAutoAttack.Spell.*.", spellId);
        }
    }
};

class RangedAutoAttack_Player : public PlayerScript
{
public:
    RangedAutoAttack_Player() : PlayerScript("RangedAutoAttack_Player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!settings.Enable)
            return;

        if (settings.NoAmmo && !player->HasAura(SPELL_REQUIRES_NO_AMMO))
            player->AddAura(SPELL_REQUIRES_NO_AMMO, player);

        if (settings.Announce && !IsBot(player))
            ChatHandler(player->GetSession()).PSendSysMessage("This server has |cff4cff00Ranged Auto-Attack|r: right-click a target out of "
                "melee range to shoot with any ranged weapon. Type .rangedauto to change it.");
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!settings.Enable || (!settings.Bots && IsBot(player)))
            return;

        RangedState* state = player->CustomData.GetDefault<RangedState>("RangedAutoAttack");
        state->CacheAge += diff;
        state->RetryTimer = state->RetryTimer > diff ? state->RetryTimer - diff : 0;

        state->Timer += diff;
        if (state->Timer < settings.Interval)
            return;

        state->Timer = 0;
        Tick(player, state);
    }
};

class RangedAutoAttack_Commands : public CommandScript
{
public:
    RangedAutoAttack_Commands() : CommandScript("RangedAutoAttack_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable rangedTable =
        {
            { "rightclick", HandleRightClick, SEC_PLAYER, Console::No },
            { "melee",      HandleMelee,      SEC_PLAYER, Console::No },
            { "",           HandleStatus,     SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "rangedauto", rangedTable },
        };

        return commandTable;
    }

    static char const* OnOff(bool on) { return on ? "|cff4cff00on|r" : "|cffff4c4coff|r"; }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        handler->PSendSysMessage("Ranged auto-attack:");
        handler->PSendSysMessage("  .rangedauto rightclick on|off - right-click out of melee range shoots: {}",
            OnOff(PlayerOption(player, SETTING_RIGHT_CLICK, settings.RightClick)));
        handler->PSendSysMessage("  .rangedauto melee on|off - switch to melee when the target is in melee range: {}",
            OnOff(PlayerOption(player, SETTING_CLOSE_MELEE, settings.CloseMelee)));
        return true;
    }

    static bool Set(ChatHandler* handler, SettingIndex index, Optional<bool> value)
    {
        Player* player = handler->GetPlayer();
        bool const serverDefault = index == SETTING_RIGHT_CLICK ? settings.RightClick : settings.CloseMelee;
        bool const on = value ? *value : !PlayerOption(player, index, serverDefault);
        player->UpdatePlayerSetting(SETTING_SOURCE, index, on ? 1 : 2);
        return HandleStatus(handler);
    }

    static bool HandleRightClick(ChatHandler* handler, Optional<bool> value) { return Set(handler, SETTING_RIGHT_CLICK, value); }
    static bool HandleMelee(ChatHandler* handler, Optional<bool> value) { return Set(handler, SETTING_CLOSE_MELEE, value); }
};

void AddSC_mod_ranged_autoattack()
{
    new RangedAutoAttack_World();
    new RangedAutoAttack_Player();
    new RangedAutoAttack_Commands();
}
