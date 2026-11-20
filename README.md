# mod-temucraft-ranged-autoattack

The hunter's right-click Auto Shot, for every class and every ranged weapon.

- **Right-click to shoot.** Right-click a target that is out of melee range while you hold a wand, thrown weapon, bow, gun or crossbow, and the melee auto-attack turns into the matching ranged auto-attack: wand Shoot (5019), Auto Throw (95001) or Auto Shot (95002 for the other classes, 75 for hunters).
- **Close range.** With no minimum range, a ranged auto-attack keeps firing in melee range. By default you switch to melee while the target is in melee range and back to the ranged auto-attack when it leaves. Each character can keep shooting instead.
- **No ammo.** Gives every player "Requires No Ammo" (46699). The core already honours it: no ammo check, and the ammo slot is emptied (its damage bonus is lost).
- **Custom spells.** The spell for each weapon kind is set in the config. When a player doesn't know that spell, any auto-repeat ranged spell they know that fits the weapon is used, so new spells work without a config change.

## Requirements

1. **The spells**, from `retail_changes.sql` section 32 (Echo Flint Engine). That section:
   - adds 95001 Auto Throw and 95002 Auto Shot, learned with the Thrown / Bows / Guns / Crossbows skills;
   - clears the movement interrupt flag on 75, 5019, 95001 and 95002, so the client stops cancelling them when you move.

   Export them to the client DBCs with Anarres, like the other rows.
2. Optional: **a core change** (not in the Temucraft core yet). The module builds and works without it. It would add two worldserver.conf switches:

   ```
   RangedAutoAttack.LikeAutoShot = 1   # every auto-repeat ranged spell pauses for casts/movement like Auto Shot, no 0.5 s first-shot delay
   RangedAutoAttack.WhileMoving = 1    # keep firing while moving
   ```

   The module logs a warning at startup when `LikeAutoShot` is off.
3. Optional: `EnablePlayerSettings = 1` in worldserver.conf, so `.rangedauto` choices are kept after logout.

## Commands (any player)

| Command | What it does |
|---|---|
| `.rangedauto` | Show your settings |
| `.rangedauto rightclick [on\|off]` | Right-click out of melee range starts shooting (no argument toggles) |
| `.rangedauto melee [on\|off]` | Switch to melee when the target is in melee range (no argument toggles) |

## Config

See `conf/mod-temucraft-ranged-autoattack.conf.dist`:
- `Enable`, `Announce`
- `RightClick` and `CloseRange.Melee` (defaults for characters that haven't chosen)
- `NoAmmo`, `Bots`, `Interval`
- `Spell.Wand|Thrown|Ranged|Hunter`

## Notes

- The client does not know about a ranged auto-attack the server started, so its action button doesn't light up. To stop it, select another target or clear your target. Starting it from the action bar works as normal.
- **Playerbots:** the Temucraft playerbots branch makes bots pick the auto-repeat spell they know for their weapon (instead of the single Shoot / Throw), and lets them shoot while moving when `RangedAutoAttack.WhileMoving` is on. `RangedAutoAttack.Bots` stays off, so the module doesn't fight the bots' AI.

Part of the Temucraft modules for AzerothCore 3.3.5a.
