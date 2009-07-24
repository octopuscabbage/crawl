/*
 *  File:       spells2.cc
 *  Summary:    Implementations of some additional spells.
 *              Mostly Necromancy and Summoning.
 *  Written by: Linley Henzell
 *
 *  Modified for Crawl Reference by $Author$ on $Date$
 */

#include "AppHdr.h"
REVISION("$Rev$");

#include "spells2.h"

#include <stdio.h>
#include <string.h>
#include <sstream>
#include <algorithm>
#include <queue>

#include "externs.h"

#include "artefact.h"
#include "beam.h"
#include "cloud.h"
#include "delay.h"
#include "directn.h"
#include "dungeon.h"
#include "effects.h"
#include "invent.h"
#include "itemname.h"
#include "itemprop.h"
#include "items.h"
#include "it_use2.h"
#include "message.h"
#include "misc.h"
#include "monplace.h"
#include "monstuff.h"
#include "mon-util.h"
#include "ouch.h"
#include "player.h"
#include "religion.h"
#include "spells4.h"
#include "spl-mis.h"
#include "stuff.h"
#include "tiles.h"
#include "terrain.h"
#include "traps.h"
#include "view.h"
#include "xom.h"

int detect_traps(int pow)
{
    pow = std::min(50, pow);

    int traps_found = 0;

    const int range = 8 + random2(8) + pow;

    for (int i = 0; i < MAX_TRAPS; i++)
    {
        trap_def& trap = env.trap[i];

        if (!trap.active())
            continue;

        if (grid_distance(you.pos(), trap.pos) < range && !trap.is_known())
        {
            traps_found++;
            trap.reveal();
            set_envmap_obj(trap.pos, grd(trap.pos));
            set_terrain_mapped(trap.pos);
        }
    }

    return (traps_found);
}

int detect_items(int pow)
{
    int items_found = 0;
    const int map_radius = 8 + random2(8) + pow;

    for (radius_iterator ri(you.pos(), map_radius, true, false); ri; ++ri)
    {
        // Don't expose new dug out areas:
        // Note: assumptions are being made here about how
        // terrain can change (eg it used to be solid, and
        // thus item free).
        if (is_terrain_changed(*ri))
            continue;

        if (igrd(*ri) != NON_ITEM
            && (!get_envmap_obj(*ri) || !is_envmap_item(*ri)))
        {
            items_found++;

            set_envmap_obj(*ri, DNGN_ITEM_DETECTED);
            set_envmap_detected_item(*ri);
#ifdef USE_TILE
            // Don't replace previously seen items with an unseen one.
            if (!is_terrain_seen(*ri) && !is_terrain_mapped(*ri))
                env.tile_bk_fg[ri->x][ri->y] = TILE_UNSEEN_ITEM;
#endif
        }
    }

    return (items_found);
}

static void _fuzz_detect_creatures(int pow, int *fuzz_radius, int *fuzz_chance)
{
#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "dc_fuzz: Power is %d", pow);
#endif

    pow = std::max(1, pow);

    *fuzz_radius = pow >= 50 ? 1 : 2;

    // Fuzz chance starts off at 100% and declines to a low of 10% for
    // obscenely powerful castings (pow caps around the 60 mark).
    *fuzz_chance = 100 - 90 * (pow - 1) / 59;
    if (*fuzz_chance < 10)
        *fuzz_chance = 10;
}

static bool _mark_detected_creature(coord_def where, const monsters *mon,
                                    int fuzz_chance, int fuzz_radius)
{
#ifdef USE_TILE
    // Get monster index pre-fuzz.
    int idx = mgrd(where);
#endif

    bool found_good = false;

    if (fuzz_radius && x_chance_in_y(fuzz_chance, 100))
    {
        const int fuzz_diam = 2 * fuzz_radius + 1;

        coord_def place;
        // Try five times to find a valid placement, else we attempt to place
        // the monster where it really is (and may fail).
        for (int itry = 0; itry < 5; ++itry)
        {
            place.set(where.x + random2(fuzz_diam) - fuzz_radius,
                      where.y + random2(fuzz_diam) - fuzz_radius);

            // If the player would be able to see a monster at this location
            // don't place it there.
            if (see_grid(place))
                continue;

            // Try not to overwrite another detected monster.
            if (is_envmap_detected_mons(place))
                continue;

            // Don't print monsters on terrain they cannot pass through,
            // not even if said terrain has since changed.
            if (map_bounds(place) && !is_terrain_changed(place)
                && mon->can_pass_through_feat(grd(place)))
            {
                found_good = true;
                break;
            }
        }

        if (found_good)
            where = place;
    }

    set_envmap_obj(where, mon->type + DNGN_START_OF_MONSTERS);
    set_envmap_detected_mons(where);

#ifdef USE_TILE
    tile_place_monster(where.x, where.y, idx, false, true);
#endif

    return (found_good);
}

int detect_creatures(int pow, bool telepathic)
{
    int fuzz_radius = 0, fuzz_chance = 0;
    if (!telepathic)
        _fuzz_detect_creatures(pow, &fuzz_radius, &fuzz_chance);

    int creatures_found = 0;
    const int map_radius = 8 + random2(8) + pow;

    // Clear the map so detect creatures is more useful and the detection
    // fuzz is harder to analyse by averaging.
    if (!telepathic)
        clear_map(false);

    for (radius_iterator ri(you.pos(), map_radius, true, false); ri; ++ri)
    {
        if (monsters *mon = monster_at(*ri))
        {
            creatures_found++;

            _mark_detected_creature(*ri, mon, fuzz_chance, fuzz_radius);

            // Assuming that highly intelligent spellcasters can
            // detect scrying. -- bwr
            if (mons_intel(mon) == I_HIGH
                && mons_class_flag(mon->type, M_SPELLCASTER))
            {
                behaviour_event(mon, ME_DISTURB, MHITYOU, you.pos());
            }
        }
    }

    return (creatures_found);
}

void corpse_rot()
{
    for (radius_iterator ri(you.pos(), 6); ri; ++ri)
    {
        if (see_grid_no_trans(*ri) && !is_sanctuary(*ri)
            && env.cgrid(*ri) == EMPTY_CLOUD)
        {
            for (stack_iterator si(*ri); si; ++si)
            {
                if (si->base_type == OBJ_CORPSES && si->sub_type == CORPSE_BODY)
                {
                    // Found a corpse.  Skeletonise it if possible.
                    if (!mons_skeleton(si->plus))
                        destroy_item(si->index());
                    else
                        turn_corpse_into_skeleton(*si);

                    place_cloud(CLOUD_MIASMA, *ri, 4+random2avg(16, 3), KC_YOU);

                    // Don't look for more corpses here.
                    break;
                }
            }
        }
    }

    if (player_can_smell())
        mpr("You smell decay.");

    // Should make zombies decay into skeletons?
}

bool brand_weapon(brand_type which_brand, int power)
{
    if (!you.weapon())
        return (false);

    const bool temp_brand = you.duration[DUR_WEAPON_BRAND];
    item_def& weapon = *you.weapon();

    // Can't brand non-weapons.
    if (weapon.base_type != OBJ_WEAPONS || is_range_weapon(weapon))
        return (false);

    // Can't brand artefacts.
    if (is_artefact(weapon))
        return (false);

    // Can't brand already-branded items.
    if (!temp_brand && get_weapon_brand(weapon) != SPWPN_NORMAL)
        return (false);

    // Can't rebrand a temporarily-branded item to a different brand.
    if (temp_brand && (get_weapon_brand(weapon) != which_brand))
        return (false);

    std::string msg = weapon.name(DESC_CAP_YOUR);
    const int wpn_type = get_vorpal_type(weapon);

    bool emit_special_message = !temp_brand;
    int duration_affected = 0;
    switch (which_brand)
    {
    case SPWPN_FLAMING:
        msg += " bursts into flame!";
        duration_affected = 7;
        break;

    case SPWPN_FREEZING:
        msg += " glows blue.";
        duration_affected = 7;
        break;

    case SPWPN_VENOM:
        if (wpn_type == DVORP_CRUSHING)
            return (false);

        msg += " starts dripping with poison.";
        duration_affected = 15;
        break;

    case SPWPN_DRAINING:
        msg += " crackles with unholy energy.";
        duration_affected = 12;
        break;

    case SPWPN_VORPAL:
        if (wpn_type != DVORP_SLICING)
            return (false);

        msg += " glows silver and looks extremely sharp.";
        duration_affected = 10;
        break;

    case SPWPN_DISTORTION:      //jmf: Added for Warp Weapon.
        msg += " seems to ";
        msg += random_choose_string("twist", "bend", "vibrate",
                                    "flex", "wobble", "twang", NULL);
        msg += (coinflip() ? " oddly." : " strangely.");
        duration_affected = 5;

        // [dshaligram] Clamping power to 2.
        power = 2;

        // This brand is insanely powerful, this isn't even really
        // a start to balancing it, but it needs something. -- bwr
        // [dshaligram] At level 7 it's costly enough to experiment
        // with removing the miscast effect. We may need to revise the spell
        // to level 8 or 9. XXX.
        // miscast_effect(SPTYP_TRANSLOCATION,
        //                9, 90, 100, "distortion branding");
        break;

    case SPWPN_PAIN:
        // Well, in theory, we could be silenced, but then how are
        // we casting the brand spell?
        msg += " shrieks in agony.";
        noisy(15, you.pos());
        duration_affected = 8;
        // We must repeat the special message here (as there's a side effect.)
        emit_special_message = true;
        break;

    case SPWPN_DUMMY_CRUSHING:  //jmf: Added for Maxwell's Silver Hammer.
        if (wpn_type != DVORP_CRUSHING)
            return (false);

        which_brand = SPWPN_VORPAL;
        msg += " glows silver and feels heavier.";
        duration_affected = 7;
        break;

    default:
        break;
    }

    if (!temp_brand)
    {
        set_item_ego_type(weapon, OBJ_WEAPONS, which_brand);
        you.wield_change = true;
    }

    if (emit_special_message)
        mpr(msg.c_str());
    else
        mprf("%s flashes.", weapon.name(DESC_CAP_YOUR).c_str());

    const int dur_change = duration_affected + roll_dice(2, power);

    you.duration[DUR_WEAPON_BRAND] += dur_change;

    if (you.duration[DUR_WEAPON_BRAND] > 50)
        you.duration[DUR_WEAPON_BRAND] = 50;

    return (true);
}

// Restore the stat in which_stat by the amount in stat_gain, displaying
// a message if suppress_msg is false, and doing so in the recovery
// channel if recovery is true.  If stat_gain is 0, restore the stat
// completely.
bool restore_stat(unsigned char which_stat, unsigned char stat_gain,
                  bool suppress_msg, bool recovery)
{
    bool stat_restored = false;

    // A bit hackish, but cut me some slack, man! --
    // Besides, a little recursion never hurt anyone {dlb}:
    if (which_stat == STAT_ALL)
    {
        for (unsigned char loopy = STAT_STRENGTH; loopy < NUM_STATS; ++loopy)
            if (restore_stat(loopy, stat_gain, suppress_msg))
                stat_restored = true;

        return (stat_restored);                // early return {dlb}
    }

    // The real function begins here. {dlb}
    char *ptr_stat = NULL;
    char *ptr_stat_max = NULL;
    bool *ptr_redraw = NULL;

    std::string msg = "You feel your ";

    if (which_stat == STAT_RANDOM)
        which_stat = random2(NUM_STATS);

    switch (which_stat)
    {
    case STAT_STRENGTH:
        msg += "strength";

        ptr_stat = &you.strength;
        ptr_stat_max = &you.max_strength;
        ptr_redraw = &you.redraw_strength;
        break;

    case STAT_INTELLIGENCE:
        msg += "intelligence";

        ptr_stat = &you.intel;
        ptr_stat_max = &you.max_intel;
        ptr_redraw = &you.redraw_intelligence;
        break;

    case STAT_DEXTERITY:
        msg += "dexterity";

        ptr_stat = &you.dex;
        ptr_stat_max = &you.max_dex;
        ptr_redraw = &you.redraw_dexterity;
        break;
    }

    if (*ptr_stat < *ptr_stat_max)
    {
        msg += " returning.";
        if (!suppress_msg)
            mpr(msg.c_str(), (recovery) ? MSGCH_RECOVERY : MSGCH_PLAIN);

        if (stat_gain == 0 || *ptr_stat + stat_gain > *ptr_stat_max)
            stat_gain = *ptr_stat_max - *ptr_stat;

        if (stat_gain != 0)
        {
            *ptr_stat += stat_gain;
            *ptr_redraw = true;
            stat_restored = true;

            if (ptr_stat == &you.strength)
                burden_change();
        }
    }

    return (stat_restored);
}

typedef std::pair<const monsters*,int> counted_monster;
typedef std::vector<counted_monster> counted_monster_list;
static void _record_monster_by_name(counted_monster_list &list,
                                    const monsters *mons)
{
    const std::string name = mons->name(DESC_PLAIN);
    for (counted_monster_list::iterator i = list.begin(); i != list.end(); ++i)
    {
        if (i->first->name(DESC_PLAIN) == name)
        {
            i->second++;
            return;
        }
    }
    list.push_back(counted_monster(mons, 1));
}

static int _monster_count(const counted_monster_list &list)
{
    int nmons = 0;
    for (counted_monster_list::const_iterator i = list.begin();
         i != list.end(); ++i)
    {
        nmons += i->second;
    }
    return (nmons);
}

static std::string _describe_monsters(const counted_monster_list &list)
{
    std::ostringstream out;

    description_level_type desc = DESC_CAP_THE;
    for (counted_monster_list::const_iterator i = list.begin();
         i != list.end(); desc = DESC_NOCAP_THE)
    {
        const counted_monster &cm(*i);
        if (i != list.begin())
        {
            ++i;
            out << (i == list.end() ? " and " : ", ");
        }
        else
            ++i;

        const std::string name =
            cm.second > 1 ? pluralise(cm.first->name(desc))
                          : cm.first->name(desc);
        out << name;
    }
    return (out.str());
}

// Poisonous light passes right through invisible players
// and monsters, and so, they are unaffected by this spell --
// assumes only you can cast this spell (or would want to).
void cast_toxic_radiance()
{
    mpr("You radiate a sickly green light!");

    you.flash_colour = GREEN;
    viewwindow(true, false);
    more();
    mesclr();

    // Determine whether the player is hit by the radiance. {dlb}
    if (you.duration[DUR_INVIS])
    {
        mpr("The light passes straight through your body.");
    }
    else if (!player_res_poison())
    {
        mpr("You feel rather sick.");
        poison_player(2);
    }

    counted_monster_list affected_monsters;
    // determine which monsters are hit by the radiance: {dlb}
    for (int i = 0; i < MAX_MONSTERS; ++i)
    {
        monsters* const monster = &menv[i];

        if (monster->alive() && mons_near(monster) && !monster->submerged())
        {
            // Monsters affected by corona are still invisible in that
            // radiation passes through them without affecting them. Therefore,
            // this check should not be !monster->invisible().
            if (!monster->has_ench(ENCH_INVIS))
            {
                bool affected =
                    poison_monster(monster, KC_YOU, 1, false, false);

                if (coinflip() && poison_monster(monster, KC_YOU, false, false))
                    affected = true;

                if (affected)
                    _record_monster_by_name(affected_monsters, monster);
            }
            else if (player_see_invis())
            {
                // message player re:"miss" where appropriate {dlb}
                mprf("The light passes through %s.",
                     monster->name(DESC_NOCAP_THE).c_str());
            }
        }
    }

    if (!affected_monsters.empty())
    {
        const std::string message =
            make_stringf("%s %s poisoned.",
                         _describe_monsters(affected_monsters).c_str(),
                         _monster_count(affected_monsters) == 1? "is" : "are");
        if (static_cast<int>(message.length()) < get_number_of_cols() - 2)
            mpr(message.c_str());
        else
        {
            // Exclamation mark to suggest that a lot of creatures were
            // affected.
            mpr("The monsters around you are poisoned!");
        }
    }
}

void cast_refrigeration(int pow)
{
    mpr("The heat is drained from your surroundings.");

    you.flash_colour = LIGHTCYAN;
    viewwindow(true, false);
    more();
    mesclr();

    // Handle the player.
    const dice_def dam_dice(3, 5 + pow / 10);
    const int hurted = check_your_resists(dam_dice.roll(), BEAM_COLD);

    if (hurted > 0)
    {
        mpr("You feel very cold.");
        ouch(hurted, NON_MONSTER, KILLED_BY_FREEZING);

        // Note: this used to be 12!... and it was also applied even if
        // the player didn't take damage from the cold, so we're being
        // a lot nicer now.  -- bwr
        expose_player_to_element(BEAM_COLD, 5);
    }

    // Now do the monsters.

    // First build the message.
    counted_monster_list affected_monsters;

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        const monsters* const monster = &menv[i];
        if (monster->alive() && you.can_see(monster))
            _record_monster_by_name(affected_monsters, monster);
    }

    if (!affected_monsters.empty())
    {
        const std::string message =
            make_stringf("%s %s frozen.",
                         _describe_monsters(affected_monsters).c_str(),
                         _monster_count(affected_monsters) == 1? "is" : "are");
        if (static_cast<int>(message.length()) < get_number_of_cols() - 2)
            mpr(message.c_str());
        else
        {
            // Exclamation mark to suggest that a lot of creatures were
            // affected.
            mpr("The monsters around you are frozen!");
        }
    }

    // Now damage the creatures.

    // Set up the cold attack.
    bolt beam;
    beam.flavour = BEAM_COLD;
    beam.thrower = KILL_YOU;

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        monsters* const monster = &menv[i];
        // Note that we *do* hurt monsters which you can't see
        // (submerged, invisible) even though you get no information
        // about it.
        if (monster->alive() && mons_near(monster))
        {
            // Calculate damage and apply.
            int hurt = mons_adjust_flavoured(monster, beam, dam_dice.roll());
            monster->hurt(&you, hurt, BEAM_COLD);

            // Cold-blooded creatures can be slowed.
            if (monster->alive()
                && mons_class_flag(monster->type, M_COLD_BLOOD)
                && coinflip())
            {
                monster->add_ench(ENCH_SLOW);
            }
        }
    }
}

void drain_life(int pow)
{
    mpr("You draw life from your surroundings.");

    // Incoming power to this function is skill in INVOCATIONS, so
    // we'll add an assert here to warn anyone who tries to use
    // this function with spell level power.
    ASSERT(pow <= 27);

    you.flash_colour = DARKGREY;
    viewwindow(true, false);
    more();
    mesclr();

    int hp_gain = 0;

    for (int i = 0; i < MAX_MONSTERS; ++i)
    {
        monsters* monster = &menv[i];

        if (!monster->alive()
            || mons_holiness(monster) != MH_NATURAL
            || mons_res_negative_energy(monster))
        {
            continue;
        }

        if (mons_near(monster))
        {
            mprf("You draw life from %s.",
                 monster->name(DESC_NOCAP_THE).c_str());

            const int hurted = 3 + random2(7) + random2(pow);
            behaviour_event(monster, ME_WHACK, MHITYOU, you.pos());
            if (!mons_is_summoned(monster))
                hp_gain += hurted;

            monster->hurt(&you, hurted);

            if (monster->alive())
                print_wounds(monster);
        }
    }

    hp_gain /= 2;

    hp_gain = std::min(pow * 2, hp_gain);

    if (hp_gain)
    {
        mpr("You feel life flooding into your body.");
        inc_hp(hp_gain, false);
    }
}

bool vampiric_drain(int pow, const dist &vmove)
{
    monsters *monster = monster_at(you.pos() + vmove.delta);

    if (monster == NULL)
    {
        mpr("There isn't anything there!");
        return (false);
    }

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    const bool success = !stop_attack_prompt(monster, false, you.pos());

    if (success)
    {
        set_attack_conducts(conducts, monster);

        behaviour_event(monster, ME_WHACK, MHITYOU, you.pos());
    }

    enable_attack_conducts(conducts);

    if (success)
    {
        if (!monster->alive())
        {
            canned_msg(MSG_NOTHING_HAPPENS);
            return (false);
        }

        if (mons_is_unholy(monster))
        {
            mpr("Aaaarggghhhhh!");
            dec_hp(random2avg(39, 2) + 10, false, "vampiric drain backlash");
            return (false);
        }

        if (mons_holiness(monster) != MH_NATURAL
            || mons_res_negative_energy(monster))
        {
            canned_msg(MSG_NOTHING_HAPPENS);
            return (false);
        }

        // The practical maximum of this is about 25 (pow @ 100). - bwr
        int hp_gain = 3 + random2avg(9, 2) + random2(pow) / 7;

        hp_gain = std::min(monster->hit_points, hp_gain);
        hp_gain = std::min(you.hp_max - you.hp, hp_gain);

        if (!hp_gain)
        {
            canned_msg(MSG_NOTHING_HAPPENS);
            return (false);
        }

        const bool mons_was_summoned = mons_is_summoned(monster);

        monster->hurt(&you, hp_gain);

        if (monster->alive())
            print_wounds(monster);

        hp_gain /= 2;

        if (hp_gain && !mons_was_summoned)
        {
            mpr("You feel life coursing into your body.");
            inc_hp(hp_gain, false);
        }
    }

    return (success);
}

bool burn_freeze(int pow, beam_type flavour, monsters *monster)
{
    pow = std::min(25, pow);

    if (monster == NULL)
    {
        mpr("There isn't anything close enough!");
        // If there's no monster there, you still pay the costs in
        // order to prevent locating invisible monsters.
        return (true);
    }

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    const bool success = !stop_attack_prompt(monster, false, you.pos());

    if (success)
    {
        set_attack_conducts(conducts, monster);

        mprf("You %s %s.",
             (flavour == BEAM_FIRE)        ? "burn" :
             (flavour == BEAM_COLD)        ? "freeze" :
             (flavour == BEAM_MISSILE)     ? "crush" :
             (flavour == BEAM_ELECTRICITY) ? "zap"
                                           : "______",
             monster->name(DESC_NOCAP_THE).c_str());

        behaviour_event(monster, ME_ANNOY, MHITYOU);
    }

    enable_attack_conducts(conducts);

    if (success)
    {
        bolt beam;
        beam.flavour = flavour;
        beam.thrower = KILL_YOU;

        const int orig_hurted = roll_dice(1, 3 + pow / 3);
        int hurted = mons_adjust_flavoured(monster, beam, orig_hurted);
        monster->hurt(&you, hurted);

        if (monster->alive())
        {
            monster->expose_to_element(flavour, orig_hurted);
            print_wounds(monster);

            if (flavour == BEAM_COLD)
            {
                const int cold_res = mons_res_cold( monster );
                if (cold_res <= 0)
                {
                    const int stun = (1 - cold_res) * random2(2 + pow/5);
                    monster->speed_increment -= stun;
                }
            }
        }
    }

    return (success);
}

bool summon_animals(int pow)
{
    bool success = false;

    // Maybe we should just generate a Lair monster instead (and
    // guarantee that it is mobile)?
    const monster_type animals[] = {
        MONS_BUMBLEBEE, MONS_WAR_DOG, MONS_SHEEP, MONS_YAK,
        MONS_HOG, MONS_SOLDIER_ANT, MONS_WOLF,
        MONS_GRIZZLY_BEAR, MONS_POLAR_BEAR, MONS_BLACK_BEAR,
        MONS_GIANT_SNAIL, MONS_BORING_BEETLE, MONS_GILA_MONSTER,
        MONS_KOMODO_DRAGON, MONS_SPINY_FROG, MONS_HOUND
    };

    int count = 0;
    const int count_max = 8;

    int pow_left = pow + 1;

    const bool varied = coinflip();

    monster_type mon = MONS_PROGRAM_BUG;

    while (pow_left >= 0 && count < count_max)
    {
        // Pick a random monster and subtract its cost.
        if (varied || count == 0)
            mon = RANDOM_ELEMENT(animals);

        const int pow_spent = mons_power(mon) * 3;

        // Allow a certain degree of overuse, but not too much.
        // Guarantee at least two summons.
        if (pow_spent >= pow_left * 2 && count >= 2)
            break;

        pow_left -= pow_spent;
        count++;

        const bool friendly = (random2(pow) > 4);

        if (create_monster(
                mgen_data(mon,
                          friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                          4, 0, you.pos(), MHITYOU)) != -1)
        {
            success = true;
        }
    }

    return (success);
}

bool cast_summon_butterflies(int pow, god_type god)
{
    bool success = false;

    const int how_many = std::max(15, 4 + random2(3) + random2(pow) / 10);

    for (int i = 0; i < how_many; ++i)
    {
        if (create_monster(
                mgen_data(MONS_BUTTERFLY, BEH_FRIENDLY,
                          3, SPELL_SUMMON_BUTTERFLIES,
                          you.pos(), MHITYOU,
                          0, god)) != -1)
        {
            success = true;
        }
    }

    if (!success)
        canned_msg(MSG_NOTHING_HAPPENS);

    return (success);
}

bool cast_summon_small_mammals(int pow, god_type god)
{
    bool success = false;

    monster_type mon = MONS_PROGRAM_BUG;

    int count = 0;
    const int count_max = 2;

    do
    {
        switch (random2(pow+1))
        {
        case 75: case 74: case 38:
            mon = MONS_ORANGE_RAT;
            break;

        case 65: case 64: case 63: case 27: case 26: case 25:
            mon = MONS_GREEN_RAT;
            break;

        case 57: case 56: case 55: case 54: case 53: case 52:
        case 20: case 18: case 16: case 14: case 12: case 10:
            mon = (coinflip()) ? MONS_QUOKKA : MONS_GREY_RAT;
            break;

        default:
            mon = (coinflip()) ? MONS_GIANT_BAT : MONS_RAT;
            break;
        }

        // If you worship a good god, don't summon an evil small mammal
        // (in this case, the orange rat).
        if (player_will_anger_monster(mon))
            mon = MONS_GREEN_RAT;

        count++;

        if (create_monster(
                mgen_data(mon, BEH_FRIENDLY,
                          3, SPELL_SUMMON_SMALL_MAMMALS,
                          you.pos(), MHITYOU,
                          0, god)) != -1)
        {
            success = true;
        }

    }
    while (random2(pow+1) > 32 && count < count_max);

    return (success);
}

bool cast_sticks_to_snakes(int pow, god_type god)
{
    if (!you.weapon())
    {
        mprf("Your %s feel slithery!", your_hand(true).c_str());
        return (false);
    }

    const item_def& wpn = *you.weapon();

    // Don't enchant sticks marked with {!D}.
    if (!check_warning_inscriptions(wpn, OPER_DESTROY))
    {
        mprf("%s feel%s slithery for a moment!",
             wpn.name(DESC_CAP_YOUR).c_str(),
             wpn.quantity > 1 ? "s" : "");
        return (false);
    }

    monster_type mon = MONS_PROGRAM_BUG;

    const int dur = std::min(3 + random2(pow) / 20, 5);
    int how_many_max = 1 + random2(1 + you.skills[SK_TRANSMUTATIONS]) / 4;
    const bool friendly = (!item_cursed(wpn));
    const beh_type beha = (friendly) ? BEH_FRIENDLY : BEH_HOSTILE;

    int count = 0;

    if (wpn.base_type == OBJ_MISSILES && wpn.sub_type == MI_ARROW)
    {
        if (wpn.quantity < how_many_max)
            how_many_max = wpn.quantity;

        for (int i = 0; i <= how_many_max; i++)
        {
            if (one_chance_in(5 - std::min(4, div_rand_round(pow * 2, 25)))
                || get_ammo_brand(wpn) == SPMSL_POISONED)
            {
                mon = x_chance_in_y(pow / 3, 100) ? MONS_WATER_MOCCASIN
                                                  : MONS_SNAKE;
            }
            else
                mon = MONS_SMALL_SNAKE;

            if (create_monster(
                    mgen_data(mon, beha,
                              dur, SPELL_STICKS_TO_SNAKES,
                              you.pos(), MHITYOU,
                              0, god)) != -1)
            {
                count++;
            }
        }
    }

    if (wpn.base_type == OBJ_WEAPONS
        && (wpn.sub_type == WPN_CLUB
            || wpn.sub_type == WPN_SPEAR
            || wpn.sub_type == WPN_QUARTERSTAFF
            || wpn.sub_type == WPN_SCYTHE
            || wpn.sub_type == WPN_GIANT_CLUB
            || wpn.sub_type == WPN_GIANT_SPIKED_CLUB
            || wpn.sub_type == WPN_BOW
            || wpn.sub_type == WPN_LONGBOW
            || wpn.sub_type == WPN_ANKUS
            || wpn.sub_type == WPN_HALBERD
            || wpn.sub_type == WPN_GLAIVE
            || wpn.sub_type == WPN_BLOWGUN))
    {
        // Upsizing Snakes to Water Moccasins as the base class for using
        // the really big sticks (so bonus applies really only to trolls
        // and ogres).  Still, it's unlikely any character is strong
        // enough to bother lugging a few of these around. - bwr

        if (item_mass(wpn) < 300)
            mon = MONS_SNAKE;
        else
            mon = MONS_WATER_MOCCASIN;

        if (pow > 20 && one_chance_in(3))
            mon = MONS_WATER_MOCCASIN;

        if (pow > 40 && one_chance_in(3))
            mon = MONS_VIPER;

        if (pow > 70 && one_chance_in(3))
            mon = MONS_BLACK_MAMBA;

        if (pow > 90 && one_chance_in(3))
            mon = MONS_GREY_SNAKE;

        if (create_monster(
                mgen_data(mon, beha,
                          dur, SPELL_STICKS_TO_SNAKES,
                          you.pos(), MHITYOU,
                          0, god)) != -1)
        {
            count++;
        }
    }

    if (wpn.quantity < count)
        count = wpn.quantity;

    if (count > 0)
    {
        dec_inv_item_quantity(you.equip[EQ_WEAPON], count);

        mprf("You create %s snake%s!",
             count > 1 ? "some" : "a",
             count > 1 ? "s" : "");
        return (true);
    }

    mprf("Your %s feel slithery!", your_hand(true).c_str());
    return (false);
}

bool cast_summon_scorpions(int pow, god_type god)
{
    bool success = false;

    const int how_many = stepdown_value(1 + random2(pow)/10 + random2(pow)/10,
                                        2, 2, 6, 8);

    for (int i = 0; i < how_many; ++i)
    {
        bool friendly = (random2(pow) > 3);

        if (create_monster(
                mgen_data(MONS_SCORPION,
                          friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                          3, SPELL_SUMMON_SCORPIONS,
                          you.pos(), MHITYOU,
                          0, god)) != -1)
        {
            success = true;
        }
    }

    if (!success)
        canned_msg(MSG_NOTHING_HAPPENS);

    return (success);
}

// Creates a mixed swarm of typical swarming animals.
// Number, duration and friendlinesss depend on spell power.
bool cast_summon_swarm(int pow, god_type god,
                       bool force_hostile,
                       bool permanent)
{
    bool success = false;
    const int dur = permanent ? 0 : std::min(2 + (random2(pow) / 4), 6);
    const int how_many = stepdown_value(2 + random2(pow)/10 + random2(pow)/25,
                                        2, 2, 6, 8);

    for (int i = 0; i < how_many; ++i)
    {
        const monster_type swarmers[] = {
            MONS_KILLER_BEE,   MONS_KILLER_BEE,    MONS_KILLER_BEE,
            MONS_SCORPION,     MONS_WORM,          MONS_GIANT_MOSQUITO,
            MONS_GIANT_BEETLE, MONS_GIANT_BLOWFLY, MONS_WOLF_SPIDER,
            MONS_BUTTERFLY,    MONS_YELLOW_WASP,   MONS_GIANT_ANT,
            MONS_GIANT_ANT,    MONS_GIANT_ANT
        };

        const monster_type mon = RANDOM_ELEMENT(swarmers);
        const bool friendly = force_hostile ? false : (random2(pow) > 7);

        if (create_monster(
                mgen_data(mon,
                          friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                          dur, !permanent ? SPELL_SUMMON_SWARM : 0,
                          you.pos(),
                          MHITYOU,
                          0, god)) != -1)
        {
            success = true;
        }
    }

    if (!success)
        canned_msg(MSG_NOTHING_HAPPENS);

    return (success);
}

bool cast_call_canine_familiar(int pow, god_type god)
{
    bool success = false;

    monster_type mon = MONS_PROGRAM_BUG;

    const int chance = random2(pow);
    if (chance < 10)
        mon = MONS_JACKAL;
    else if (chance < 15)
        mon = MONS_HOUND;
    else
    {
        switch (chance % 7)
        {
        case 0:
            if (one_chance_in(you.species == SP_HILL_ORC ? 3 : 6))
                mon = MONS_WARG;
            else
                mon = MONS_WOLF;
            break;

        case 1:
        case 2:
            mon = MONS_WAR_DOG;
            break;

        case 3:
        case 4:
            mon = MONS_HOUND;
            break;

        default:
            mon = MONS_JACKAL;
            break;
        }
    }

    const int dur = std::min(2 + (random2(pow) / 4), 6);

    const bool friendly = (random2(pow) > 3);

    if (create_monster(
            mgen_data(mon,
                      friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                      dur, SPELL_CALL_CANINE_FAMILIAR,
                      you.pos(),
                      MHITYOU,
                      0, god)) != -1)
    {
        success = true;

        mpr("A canine appears!");

        if (!friendly)
            mpr("It doesn't look very happy.");
    }
    else
        canned_msg(MSG_NOTHING_HAPPENS);

    return (success);
}

// 'unfriendly' is percentage chance summoned elemental goes
//              postal on the caster (after taking into account
//              chance of that happening to unskilled casters
//              anyway).
bool cast_summon_elemental(int pow, god_type god,
                           monster_type restricted_type,
                           int unfriendly)
{
    monster_type mon = MONS_PROGRAM_BUG;

    coord_def targ;
    dist smove;

    const int dur = std::min(2 + (random2(pow) / 5), 6);
    const bool any_elemental = (restricted_type == MONS_PROGRAM_BUG);

    while (true)
    {
        mpr("Summon from material in which direction? ", MSGCH_PROMPT);

        direction(smove, DIR_DIR, TARG_ANY);

        if (!smove.isValid)
        {
            canned_msg(MSG_OK);
            return (false);
        }

        targ = you.pos() + smove.delta;

        if (const monsters *m = monster_at(targ))
        {
            if (you.can_see(m))
                mpr("There's something there already!");
            else
            {
                mpr("Something seems to disrupt your summoning.");
                return (false);
            }
        }
        else if (smove.delta.origin())
            mpr("You can't summon an elemental from yourself!");
        else if ((any_elemental || restricted_type == MONS_EARTH_ELEMENTAL)
                 && (grd(targ) == DNGN_ROCK_WALL
                     || grd(targ) == DNGN_CLEAR_ROCK_WALL))
        {
            if (!in_bounds(targ))
            {
                mpr("That wall won't yield to your beckoning.");
                // XXX: Should this cost a turn?
            }
            else
            {
                mon = MONS_EARTH_ELEMENTAL;
                break;
            }
        }
        else
            break;
    }

    if (mon == MONS_EARTH_ELEMENTAL)
    {
        grd(targ) = DNGN_FLOOR;
    }
    else if (env.cgrid(targ) != EMPTY_CLOUD
             && env.cloud[env.cgrid(targ)].type == CLOUD_FIRE
             && (any_elemental || restricted_type == MONS_FIRE_ELEMENTAL))
    {
        mon = MONS_FIRE_ELEMENTAL;
        delete_cloud(env.cgrid(targ));
    }
    else if (grd(targ) == DNGN_LAVA
             && (any_elemental || restricted_type == MONS_FIRE_ELEMENTAL))
    {
        mon = MONS_FIRE_ELEMENTAL;
    }
    else if (grid_is_watery(grd(targ))
             && (any_elemental || restricted_type == MONS_WATER_ELEMENTAL))
    {
        mon = MONS_WATER_ELEMENTAL;
    }
    else if (grd(targ) >= DNGN_FLOOR
             && env.cgrid(targ) == EMPTY_CLOUD
             && (any_elemental || restricted_type == MONS_AIR_ELEMENTAL))
    {
        mon = MONS_AIR_ELEMENTAL;
    }

    // Found something to summon?
    if (mon == MONS_PROGRAM_BUG)
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return (false);
    }

    // silly - ice for water? 15jan2000 {dlb}
    // little change here to help with the above... and differentiate
    // elements a bit... {bwr}
    // - Water elementals are now harder to be made reliably friendly.
    // - Air elementals are harder because they're more dynamic/dangerous.
    // - Earth elementals are more static and easy to tame (as before).
    // - Fire elementals fall in between the two (10 is still fairly easy).
    const bool friendly = ((mon != MONS_FIRE_ELEMENTAL
                            || x_chance_in_y(you.skills[SK_FIRE_MAGIC], 10))

                        && (mon != MONS_WATER_ELEMENTAL
                            || x_chance_in_y(you.skills[SK_ICE_MAGIC],
                                             (you.species == SP_MERFOLK) ? 5
                                                                         : 15))

                        && (mon != MONS_AIR_ELEMENTAL
                            || x_chance_in_y(you.skills[SK_AIR_MAGIC], 15))

                        && (mon != MONS_EARTH_ELEMENTAL
                            || x_chance_in_y(you.skills[SK_EARTH_MAGIC], 5))

                        && random2(100) >= unfriendly);

    if (create_monster(
            mgen_data(mon,
                      friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                      dur, SPELL_SUMMON_ELEMENTAL,
                      targ,
                      MHITYOU,
                      0, god)) == -1)
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        return (false);
    }

    mpr("An elemental appears!");

    if (!friendly)
        mpr("It doesn't seem to appreciate being summoned.");

    return (true);
}

bool cast_summon_ice_beast(int pow, god_type god)
{
    monster_type mon = MONS_ICE_BEAST;

    const int dur = std::min(2 + (random2(pow) / 4), 6);

    if (create_monster(
            mgen_data(mon, BEH_FRIENDLY,
                      dur, SPELL_SUMMON_ICE_BEAST,
                      you.pos(), MHITYOU,
                      0, god)) != -1)
    {
        mpr("A chill wind blows around you.");
        return (true);
    }

    canned_msg(MSG_NOTHING_HAPPENS);
    return (false);
}

bool cast_summon_ugly_thing(int pow, god_type god)
{
    const int chance = std::max(6 - (pow / 12), 1);
    monster_type mon = (one_chance_in(chance)) ? MONS_VERY_UGLY_THING
                                               : MONS_UGLY_THING;

    const int dur = std::min(2 + (random2(pow) / 4), 6);

    const bool friendly = (random2(pow) > 3);

    if (create_monster(
            mgen_data(mon,
                      friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                      dur, SPELL_SUMMON_UGLY_THING,
                      you.pos(),
                      MHITYOU,
                      0, god)) != -1)
    {
        mpr((mon == MONS_VERY_UGLY_THING) ? "A very ugly thing appears."
                                          : "An ugly thing appears.");

        if (!friendly)
            mpr("It doesn't look very happy.");

        return (true);
    }

    canned_msg(MSG_NOTHING_HAPPENS);
    return (false);
}

bool cast_summon_dragon(int pow, god_type god)
{
    // Removed the chance of multiple dragons... one should be more
    // than enough, and if it isn't, the player can cast again...
    // especially since these aren't on the Abjuration plan... they'll
    // last until they die (maybe that should be changed, but this is
    // a very high level spell so it might be okay).  -- bwr
    const bool friendly = (random2(pow) > 5);

    if (create_monster(
            mgen_data(MONS_DRAGON,
                      friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                      3, SPELL_SUMMON_DRAGON,
                      you.pos(),
                      MHITYOU,
                      0, god)) != -1)
    {
        mpr("A dragon appears.");

        if (!friendly)
            mpr("It doesn't look very happy.");

        return (true);
    }

    canned_msg(MSG_NOTHING_HAPPENS);
    return (false);
}

bool summon_berserker(int pow, god_type god, int spell,
                      bool force_hostile)
{
    monster_type mon = MONS_PROGRAM_BUG;

    int dur = std::min(2 + (random2(pow) / 4), 6);

    if (pow <= 100)
    {
        // bears
        mon = (coinflip()) ? MONS_BLACK_BEAR : MONS_GRIZZLY_BEAR;
    }
    else if (pow <= 140)
    {
        // ogres
        if (one_chance_in(3))
            mon = MONS_TWO_HEADED_OGRE;
        else
            mon = MONS_OGRE;
    }
    else if (pow <= 180)
    {
        // trolls
        switch (random2(8))
        {
        case 0:
            mon = MONS_DEEP_TROLL;
            break;
        case 1:
        case 2:
            mon = MONS_IRON_TROLL;
            break;
        case 3:
        case 4:
            mon = MONS_ROCK_TROLL;
            break;
        default:
            mon = MONS_TROLL;
            break;
        }
    }
    else
    {
        // giants
        mon = (coinflip()) ? MONS_HILL_GIANT : MONS_STONE_GIANT;
    }

    int monster =
        create_monster(
            mgen_data(mon,
                      !force_hostile ? BEH_FRIENDLY : BEH_HOSTILE,
                      dur, spell, you.pos(), MHITYOU, 0, god));

    if (monster == -1)
        return (false);

    monsters *summon = &menv[monster];

    summon->go_berserk(false);
    mon_enchant berserk = summon->get_ench(ENCH_BERSERK);
    mon_enchant abj = summon->get_ench(ENCH_ABJ);

    // Let Trog's gifts berserk longer, and set the abjuration
    // timeout to the berserk timeout.
    berserk.duration = berserk.duration * 3 / 2;
    berserk.maxduration = berserk.duration;
    abj.duration = abj.maxduration = berserk.duration;
    summon->update_ench(berserk);
    summon->update_ench(abj);

    player_angers_monster(&menv[monster]);

    return (true);
}

static bool _summon_holy_being_wrapper(int pow, god_type god, int spell,
                                       monster_type mon, int dur, bool friendly,
                                       bool quiet)
{
    UNUSED(pow);

    const int monster =
        create_monster(
            mgen_data(mon,
                      friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                      dur, spell,
                      you.pos(),
                      MHITYOU,
                      MG_FORCE_BEH, god));

    if (monster == -1)
        return (false);

    monsters *summon = &menv[monster];
    summon->flags |= MF_ATT_CHANGE_ATTEMPT;

    if (!quiet)
    {
        mprf("You are momentarily dazzled by a brilliant %slight.",
             (mon == MONS_DAEVA) ? "golden " :
             (mon == MONS_ANGEL) ? "white "
                                 : "");
    }

    player_angers_monster(&menv[monster]);

    return (true);
}

static bool _summon_holy_being_wrapper(int pow, god_type god, int spell,
                                       holy_being_class_type hbct, int dur,
                                       bool friendly, bool quiet)
{
    monster_type mon = summon_any_holy_being(hbct);

    return _summon_holy_being_wrapper(pow, god, spell, mon, dur, friendly,
                                      quiet);
}

bool summon_holy_warrior(int pow, god_type god, int spell,
                         bool force_hostile, bool permanent,
                         bool quiet)
{
    return _summon_holy_being_wrapper(pow, god, spell, HOLY_BEING_WARRIOR,
                                      !permanent ?
                                          std::min(2 + (random2(pow) / 4), 6) :
                                          0,
                                      !force_hostile, quiet);
}

bool cast_tukimas_dance(int pow, god_type god, bool force_hostile)
{
    bool success = true;

    const int dur = std::min(2 + (random2(pow) / 5), 6);

    const int wpn = you.equip[EQ_WEAPON];

    // See if the wielded item is appropriate.
    if (wpn == -1
        || you.inv[wpn].base_type != OBJ_WEAPONS
        || is_range_weapon(you.inv[wpn])
        || is_special_unrandom_artefact(you.inv[wpn]))
    {
        success = false;
    }

    // See if we can get an mitm for the dancing weapon.
    const int i = get_item_slot();

    if (i == NON_ITEM)
        success = false;
    else if (success)
    {
        // Copy item now so that mitm[i] is occupied and doesn't get picked
        // by get_item_slot() when giving the dancing weapon its item
        // during create_monster().
        mitm[i] = you.inv[wpn];
    }

    int monster;

    if (success)
    {
        // Cursed weapons become hostile.
        const bool friendly = (!force_hostile && !item_cursed(you.inv[wpn]));

        monster =
            create_monster(
                mgen_data(MONS_DANCING_WEAPON,
                          friendly ? BEH_FRIENDLY : BEH_HOSTILE,
                          dur, SPELL_TUKIMAS_DANCE,
                          you.pos(),
                          MHITYOU,
                          0, god));

        if (monster == -1)
        {
            mitm[i].clear();
            success = false;
        }
    }

    if (!success)
    {
        destroy_item(i);

        if (wpn != -1)
        {
            mprf("%s vibrates crazily for a second.",
                 you.inv[wpn].name(DESC_CAP_YOUR).c_str());
        }
        else
            mprf("Your %s twitch.", your_hand(true).c_str());

        return (false);
    }

    // We are successful.  Unwield the weapon, removing any wield
    // effects.
    unwield_item();

    // Copy the unwielded item.
    mitm[i] = you.inv[wpn];
    mitm[i].quantity = 1;
    mitm[i].pos.set(-2, -2);
    mitm[i].link = NON_ITEM + 1 + monster;

    // Mark the weapon as thrown, so that we'll autograb it when the
    // tango's done.
    mitm[i].flags |= ISFLAG_THROWN;

    mprf("%s dances into the air!", you.inv[wpn].name(DESC_CAP_YOUR).c_str());

    you.inv[wpn].quantity = 0;

    destroy_item(menv[monster].inv[MSLOT_WEAPON]);
    menv[monster].inv[MSLOT_WEAPON] = i;
    menv[monster].colour = mitm[i].colour;
    burden_change();

    return (true);
}

bool cast_conjure_ball_lightning(int pow, god_type god)
{
    bool success = false;

    // Restricted so that the situation doesn't get too gross.  Each of
    // these will explode for 3d20 damage. -- bwr
    const int how_many = std::min(8, 3 + random2(2 + pow / 50));

    for (int i = 0; i < how_many; ++i)
    {
        coord_def target;
        bool found = false;
        for (int j = 0; j < 10; ++j)
        {
            if (random_near_space(you.pos(), target, true, true)
                && distance(you.pos(), target) <= 5)
            {
                found = true;
                break;
            }
        }

        // If we fail, we'll try the ol' summon next to player trick.
        if (!found)
            target = you.pos();

        int monster =
            mons_place(
                mgen_data(MONS_BALL_LIGHTNING, BEH_FRIENDLY,
                          0, SPELL_CONJURE_BALL_LIGHTNING,
                          target, MHITNOT, 0, god));

        if (monster != -1)
        {
            success = true;
            menv[monster].add_ench(ENCH_SHORT_LIVED);
        }
    }

    if (success)
        mpr("You create some ball lightning!");
    else
        canned_msg(MSG_NOTHING_HAPPENS);

    return (success);
}

// Turns corpses in LOS into skeletons and grows toadstools on them.
// returns the number of corpses consumed
int fungal_bloom()
{
    int seen_mushrooms  = 0;
    int seen_corpses    = 0;

    int processed_count = 0;
    for (radius_iterator i(you.position, LOS_RADIUS); i; ++i)
    {
        // going to ignore squares that are already occupied by non-fungi
        if (actor_at(*i) && !actor_at(*i)->mons_species() == MONS_TOADSTOOL)
            continue;


        for (stack_iterator j(*i); j; ++j)
        {
            bool corpse_on_pos = false;
            if (j->base_type == OBJ_CORPSES && j->sub_type == CORPSE_BODY)
            {
                corpse_on_pos  = true;
                int trial_prob = mushroom_prob(*j);

                processed_count++;
                int target_count = 1 + binomial_generator(20, trial_prob);

                int seen_per;
                spawn_corpse_mushrooms(*j, target_count, seen_per, true);

                seen_mushrooms += seen_per;

                // Either turn this corpse into a skeleton or destroy it.
                if (mons_skeleton(j->plus))
                    turn_corpse_into_skeleton(*j);
                else
                    destroy_item(j->index());
            }

            if (corpse_on_pos && see_grid(*i))
                seen_corpses++;
        }
    }

    if (seen_mushrooms > 0)
    {
        // We obviously saw some corpses since we only processed squares
        // in LOS.
        ASSERT(seen_corpses > 0);

        mprf("%s grow%s from %snearby corpse%s.",
             (seen_mushrooms > 1 ? "Some toadstools"
                                   : "A toadstool"),
             (seen_mushrooms > 1 ? "s" : ""),
             (seen_corpses > 1)  ? "" : "a ",
             (seen_corpses > 1)  ? "s" : "");
    }

    return (processed_count);
}

int create_plant(coord_def & target)
{
    if (actor_at(target) || !mons_class_can_pass(MONS_PLANT, grd(target)))
        return 0;

    const int plant = create_monster(mgen_data
                                     (MONS_PLANT,
                                      BEH_FRIENDLY,
                                      0,
                                      0,
                                      target,
                                      MHITNOT,
                                      MG_FORCE_PLACE, GOD_FEAWN));


    if (plant != -1 && see_grid(target))
        mpr("A plant grows up from the ground.");

    return (plant != -1);
}

bool sunlight()
{
    int c_size = 5;
    int x_offset[] = {-1, 0, 0, 0, 1};
    int y_offset[] = { 0,-1, 0, 1, 0};

    dist spelld;

    bolt temp_bolt;

    temp_bolt.colour = YELLOW;
    direction(spelld, DIR_TARGET, TARG_ENEMY, LOS_RADIUS, false, false,
              false, true, "Select sunlight destination", NULL,
              true);

    if (!spelld.isValid)
        return (false);

    coord_def base = spelld.target;

    int evap_count  = 0;
    int plant_count = 0;

    // FIXME: Uncomfortable level of code duplication here but the explosion
    // code in bolt subjects the input radius to r*(r+1) for the threshold and
    // since r is an integer we can never get just the 4-connected neighbours.
    // Anyway the bolt code doesn't seem to be well set up to handle the
    // 'occasional plant' gimmick.
    for (int i = 0;i < c_size; ++i)
    {
        coord_def target = base;
        target.x += x_offset[i];
        target.y += y_offset[i];

        if (!in_bounds(target) || grid_is_solid(grd(target)))
            continue;

        temp_bolt.explosion_draw_cell(target);

        actor *victim = actor_at(target);

        // If this is a water square we will evaporate it.
        dungeon_feature_type ftype = grd(target);

        switch (ftype)
        {
        case DNGN_SHALLOW_WATER:
            ftype = DNGN_FLOOR;
            break;

        case DNGN_DEEP_WATER:
            ftype = DNGN_SHALLOW_WATER;
            break;

        default:
            break;
        }

        if (grd(target) != ftype)
        {
            grd(target) = ftype;
            if (see_grid(target))
                evap_count++;
        }

        monsters *mons = monster_at(target);

        // Pop submerged status.
        if (mons && mons_habitat(mons) == HT_WATER)
        {
            mons->del_ench(ENCH_SUBMERGED);
            if (ftype == DNGN_FLOOR)
                mons->add_ench(mon_enchant(ENCH_AQUATIC_LAND, 0, KC_YOU));
        }

        if (victim)
        {
            if (!mons)
                you.backlight();
            else
                backlight_monsters(target, 1, 0);
        }
        else if (one_chance_in(100)
                 && ftype >= DNGN_FLOOR_MIN
                 && ftype <= DNGN_FLOOR_MAX)
        {
            // Create a plant.
            const int plant = create_monster(mgen_data(MONS_PLANT,
                                                       BEH_HOSTILE,
                                                       0,
                                                       0,
                                                       target,
                                                       MHITNOT,
                                                       MG_FORCE_PLACE,
                                                       GOD_FEAWN));

            if (plant != -1 && see_grid(target))
                plant_count++;
        }
    }
    delay(50);

    update_screen();

    if (plant_count)
    {
        mprf("%s grow%s in the sunlight.",
             (plant_count > 1 ? "Some plants": "A plant"),
             (plant_count > 1 ? "": "s"));
    }

    if (evap_count)
        mprf("Some water evaporates in the bright sunlight.");

    return (true);
}

template<typename T>
bool less_second(const T & left, const T & right)
{
    return left.second < right.second;
}

typedef std::pair<coord_def, int> point_distance;

// dfs starting at origin, find the distance from the origin to the targets
// (not leaving LOS, not crossing monsters or solid walls) and store that in
// distances
void path_distance(coord_def & origin,
                   std::vector<coord_def> & targets,
                   std::vector<int> & distances)
{
    std::set<unsigned> exclusion;
    std::queue<point_distance> fringe;
    fringe.push(point_distance(origin,0));

    int idx = origin.x + origin.y * X_WIDTH;
    exclusion.insert(idx);

    while (!fringe.empty())
    {
        point_distance current = fringe.front();
        fringe.pop();

        // did we hit a target?
        for (unsigned i = 0; i < targets.size(); ++i)
        {
            if (current.first == targets[i])
            {
                distances[i] = current.second;
                break;
            }
        }

        for (adjacent_iterator adj_it(current.first); adj_it; ++adj_it)
        {
            idx = adj_it->x + adj_it->y * X_WIDTH;
            if (see_grid(*adj_it)
                && !grid_is_solid(env.grid(*adj_it))
                && exclusion.insert(idx).second)
            {
                monsters * temp = monster_at(*adj_it);
                if (!temp || (temp->attitude == ATT_HOSTILE
                              && temp->mons_species() != MONS_PLANT
                              && temp->mons_species() != MONS_TOADSTOOL
                              && temp->mons_species() != MONS_FUNGUS))
                {
                    fringe.push(point_distance(*adj_it, current.second+1));
                }
            }
        }
    } // end while
}

// so we are basically going to compute point to point distance between
// the points of origin and the end points (origins and targets respecitvely)
// We will return a vector consisting of the minimum distances along one
// dimension of the distance matrix.
void point_point(std::vector<coord_def> & origins,
                 std::vector<coord_def> & targets,
                 bool origin_to_target,
                 std::vector<int> & distances)
{

    distances.clear();
    // Consider a matrix where the points of origin form the rows and
    // the target points form the column, we want to take the minimum along
    // one of those dimensions.
    if (origin_to_target)
        distances.resize(origins.size(), INT_MAX);
    else
        distances.resize(targets.size(), INT_MAX);

    std::vector<int> current_distances(targets.size(), 0);
    for (unsigned i = 0; i < origins.size(); ++i)
    {
        for (unsigned j = 0; j < current_distances.size(); ++j)
            current_distances[j] = INT_MAX;

        path_distance(origins[i], targets, current_distances);

        // So we got the distance from a point of origin to one of the
        // targets. What should we do with it?
        if (origin_to_target)
        {
            // The minimum of current_distances is points(i)
            int min_dist = current_distances[0];
            for (unsigned j = 1; i < current_distances.size(); ++i)
                if (current_distances[j] < min_dist)
                    min_dist = current_distances[j];

            distances[i] = min_dist;
        }
        else
        {
            for (unsigned j = 0; j < targets.size(); ++j)
            {
                if (i == 0)
                    distances[j] = current_distances[j];
                else if (current_distances[j] < distances[j])
                    distances[j] = current_distances[j];
            }
        }
    }
}

// So the idea is we want to decide which adjacent tiles are in the most 'danger'
// We claim danger is proportional to the minimum distances from the point to a
// (hostile) monster. This function carries out at most 8 depth-first searches
// to calculate the distances in question. In practice it should be called for
// at most 7 searches since 8 (all adjacent free, > 8 monsters in view) can be
// special cased easily.
bool prioritize_adjacent(coord_def & target, std::vector<coord_def> & candidates)
{
    radius_iterator los_it(target, LOS_RADIUS, true, true, true);

    std::vector<coord_def> mons_positions;
    // collect hostile monster positions in LOS
    for ( ; los_it; ++los_it)
    {
        monsters * hostile = monster_at(*los_it);

        if (hostile && hostile->attitude == ATT_HOSTILE)
            mons_positions.push_back(hostile->pos());
    }

    mprf("found %d hostiles", mons_positions.size());

    if (mons_positions.empty())
    {
        std::random_shuffle(candidates.begin(), candidates.end());
        return (true);
    }

    bool squares_to_monsters = mons_positions.size() > candidates.size();

    std::vector<int> distances;

    // So the idea is we will search from either possible plant locations to
    // monsters or from monsters to possible plant locations, but honestly the
    // implementation is unnecessarily tense and doing plants to monsters all
    // the time would be fine. Yet I'm reluctant to change it because it does
    // work.
    if (squares_to_monsters)
        point_point(candidates, mons_positions, squares_to_monsters, distances);
    else
        point_point(mons_positions, candidates, squares_to_monsters, distances);

    std::vector<point_distance> possible_moves(candidates.size());

    for (unsigned i = 0; i < possible_moves.size(); ++i)
    {
        possible_moves[i].first  = candidates[i];
        possible_moves[i].second = distances[i];
    }

    std::sort(possible_moves.begin(), possible_moves.end(),
              less_second<point_distance>);

    for (unsigned i = 0; i < candidates.size(); ++i)
        candidates[i] = possible_moves[i].first;

    return (true);
}

// Create a ring or partial ring around the caster.
// User is prompted to select a stack of fruit then plants are placed on open
// squares adjacent to the caster, of course 1 piece of fruit is consumed per
// plant so a complete ring may not be formed.
bool plant_ring_from_fruit()
{
    int possible_count;
    int created_count = 0;
    int rc = prompt_invent_item("Use which fruit?",
                                MT_INVLIST,
                                OSEL_FRUIT,
                                true,
                                true,
                                true,
                                '\0',
                                -1,
                                &possible_count);

    if (prompt_failed(rc))
        return 0;

    std::vector<coord_def> adjacent;

    for (adjacent_iterator adj_it(you.pos()); adj_it; ++adj_it)
    {
        if (mons_class_can_pass(MONS_PLANT, env.grid(*adj_it))
            && !actor_at(*adj_it))
        {
            adjacent.push_back(*adj_it);
        }
    }

    if ((int) adjacent.size() > possible_count)
    {
        prioritize_adjacent(you.pos(), adjacent);

        //::update_screen();
    }

    unsigned target_count =
        (possible_count < (int) adjacent.size()) ? possible_count
                                                 : adjacent.size();

    for (unsigned i = 0; i < target_count; ++i)
        if (create_plant(adjacent[i]))
            created_count++;

    dec_inv_item_quantity(rc, created_count);

    return (created_count);
}


// Creates a circle of water around the target (radius is approximately 2)
// Turns normal floor tiles into shallow water and turns (unoccupied) shallow
// water into deep water.
// Chance of spawning plants or fungus on unoccupied dry floor tiles outside
// of the rainfall area
// Returns the number of plants/fungus created
int rain(coord_def & target)
{
    int spawned_count = 0;
    for (radius_iterator rad(target, LOS_RADIUS, true, true, true); rad; ++rad)
    {
        // Adjusting the shape of the rainfall slightly to make it look nicer.
        // I want a threshold of 2.5 on the euclidean distance so a threshold
        // of 6 prior to the sqrt is close enough.
        int rain_thresh = 6;
        coord_def local = *rad - target;

        dungeon_feature_type ftype = grd(*rad);

        if (local.abs() > rain_thresh)
        {
            // Maybe spawn a plant on (dry, open) squares that are in LOS but
            // outside the rainfall area.
            // In open space there are 213 squares in LOS, and we are
            // going to drop water on (25-4) of those, so if we want x plants
            // to spawn on average in open space the trial probability should
            // be x/192
            if (x_chance_in_y(5, 192)
                && !actor_at(*rad)
                && ftype  >= DNGN_FLOOR_MIN
                && ftype  <= DNGN_FLOOR_MAX)
            {
                const int plant = create_monster(mgen_data
                                     (coinflip() ? MONS_PLANT : MONS_FUNGUS,
                                      BEH_HOSTILE,
                                      0,
                                      0,
                                      *rad,
                                      MHITNOT,
                                      MG_FORCE_PLACE, GOD_FEAWN));

                if (plant != -1)
                    spawned_count++;
            }
            continue;
        }

        // Turn regular floor squares only into shallow water
        if (ftype >= DNGN_FLOOR_MIN && ftype <= DNGN_FLOOR_MAX)
        {
            grd(*rad) = DNGN_SHALLOW_WATER;
            // Remove blood stains as well
            env.map(*rad).property &= ~(FPROP_BLOODY);

            monsters *mon = monster_at(*rad);
            if (mon && mon->has_ench(ENCH_AQUATIC_LAND))
                mon->del_ench(ENCH_AQUATIC_LAND);
        }
        // We can also turn shallow water into deep water, but we're just going
        // to skip cases where there is something on the shallow water.
        // Destroying items will probably annoy people and insta-killing
        // monsters is clearly out of the question.
        else if (!actor_at(*rad)
                 && igrd(*rad) == NON_ITEM
                 && ftype == DNGN_SHALLOW_WATER)
        {
            grd(*rad) = DNGN_DEEP_WATER;
        }
    }

    if (spawned_count>0)
    {
        mprf("%s grow%s in the rain.",
             (spawned_count > 1 ? "Some plants" : "A plant"),
             (spawned_count > 1 ? "" : "s"));
    }

    return (spawned_count);
}

void corpse_spores()
{
    for (radius_iterator rad(you.pos(), LOS_RADIUS, true, true, true); rad;
         ++rad)
    {
        for (stack_iterator stack_it(*rad); stack_it; ++stack_it)
        {
            if (stack_it->base_type == OBJ_CORPSES
                && stack_it->sub_type == CORPSE_BODY)
            {

                create_monster(mgen_data(MONS_GIANT_SPORE,
                                         BEH_FRIENDLY,
                                         0,
                                         0,
                                         *rad,
                                         MHITNOT,
                                         MG_FORCE_PLACE));


                if (mons_skeleton(stack_it->plus))
                    turn_corpse_into_skeleton(*stack_it);
                else
                    destroy_item(stack_it->index());

                break;
            }
        }
    }
}

typedef std::pair<monsters *, int> monster_cost;

struct lesser_second
{
    bool operator()(const monster_cost & left, const monster_cost & right)
    {
        // Explicitly making this comparison unstable. I'm not clear on the
        // complete implications of this but it should be ok for a heap.
        if (left.second == right.second)
            return coinflip();

        return (left.second < right.second);
    }
};

bool evolve_flora()
{
    int needed_fruit = 2;

    std::priority_queue<monster_cost,
                        std::vector<monster_cost>,
                        lesser_second > available_targets;

    int points         = 15;
    int plant_cost     = 10;
    int toadstool_cost =  1;
    int fungus_cost    =  5;

    for (radius_iterator rad(you.pos(), LOS_RADIUS, true, true, true); rad;
         ++rad)
    {
        monsters * target = monster_at(*rad);
        if (!target)
            continue;

        int cost = 0;
        switch (target->mons_species())
        {
        case MONS_PLANT:
            cost = plant_cost;
            break;

        case MONS_FUNGUS:
            cost = fungus_cost;
            break;

        case MONS_TOADSTOOL:
            cost = toadstool_cost;
            break;
        };

        if (cost != 0)
            available_targets.push(std::pair<monsters *, int>(target, cost));
    }

    if (available_targets.empty())
        return (false);

    int rc;
    int available_count;

    rc = prompt_invent_item("Use which fruit (must have at least 2)?",
                            MT_INVLIST, OSEL_SOME_FRUIT, true, true, true,
                            '\0', -1, &available_count);

    if (prompt_failed(rc))
        return (false);

    dec_inv_item_quantity(rc, needed_fruit);

    int plants_evolved     = 0;
    int toadstools_evolved = 0;
    int fungi_evolved      = 0;

    while (!available_targets.empty() && points > 0)
    {
        monster_cost current_target = available_targets.top();

        monsters * current_plant = current_target.first;
        available_targets.pop();

        // Can we afford this thing?
        if (current_target.second > points)
            continue;

        points -= current_target.second;

        int base_species = current_plant->mons_species();
        coord_def target_square = current_plant->pos();

        // Remove the original plant.
        // XXX: Why do we destroy the old and create a new plant
        //      rather than simply upgrade the old plant?
        monster_die(current_plant, KILL_MISC, NON_MONSTER, true);

        monster_type new_species = MONS_PLANT;
        switch (base_species)
        {
        case MONS_PLANT:
            new_species = MONS_OKLOB_PLANT;
            plants_evolved++;
            break;

        case MONS_FUNGUS:
            new_species = MONS_WANDERING_MUSHROOM;
            fungi_evolved++;
            break;

        case MONS_TOADSTOOL:
            new_species = MONS_FUNGUS;
            toadstools_evolved++;
            break;
        };

        rc = create_monster(mgen_data(new_species,
                                      BEH_FRIENDLY, 0, 0, target_square,
                                      MHITNOT, MG_FORCE_PLACE, GOD_FEAWN));

        // We can potentially upgrade toadstools a second time.
        if (base_species == MONS_TOADSTOOL && rc != -1)
            available_targets.push(monster_cost(&env.mons[rc], fungus_cost));
    }

    // messaging...
    if (plants_evolved > 0)
    {
        mprf("%s can now spit acid.",
             (plants_evolved == 1 ? "A plant" : "Some plants"));
    }

    if (toadstools_evolved>0)
    {
        const bool plural          = toadstools_evolved > 1;
        mprf("%s toadstool%s gain%s stability.",
             (plural ? "Some" : "A"),
             (plural ? "s" : ""),
             (plural ? ""  : "s"));
    }

    if (fungi_evolved > 0)
    {
        const bool multiple = fungi_evolved > 1;
        mprf("The fungal %s can now pick up %s mycelia and move.",
             (multiple ? "colonies" : "colony"),
             (multiple ? "their" : "its"));
    }

    return (true);
}
