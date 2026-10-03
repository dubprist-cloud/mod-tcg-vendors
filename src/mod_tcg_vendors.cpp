#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "GossipDef.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "Optional.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ============================================================
//  Operation modes — mirror TCGVendors.Mode config values
// ============================================================
enum TCGVendorMode : int
{
    MODE_DISABLED  = 0,
    MODE_FREE      = 1,
    MODE_BLIZZLIKE = 2,
    MODE_ITEM_CODE = 3,
};

// ============================================================
//  NPC Entry IDs
// ============================================================
enum TCGNPCEntries : uint32
{
    NPC_LANDRO_LONGSHOT      = 17249,
    NPC_RANSIN_DONNER        = 2943,
    NPC_ZAS_TYSH             = 7951,
    NPC_THARL_STONEBLEEDER   = 16076,
    NPC_GAREL_REDROCK        = 16070,
    NPC_EDWARD_CAIRN         = 29095,
    NPC_IAN_DRAKE            = 29093,
};

// ============================================================
//  NPC greeting text IDs  (see sql/world/base/tcg_vendors_setup.sql)
// ============================================================
enum TCGNpcTextIds : uint32
{
    NPC_TEXT_LANDRO   = 90001,
    NPC_TEXT_BLIZZCON = 90002,
    NPC_TEXT_PROMO    = 90003,   // Garel Redrock / Tharl Stonebleeder
    NPC_TEXT_WWI      = 90004,   // Edward Cairn / Ian Drake (Worldwide Invitational)
};

// ============================================================
//  Gossip sender values
// ============================================================
enum GossipSenders : uint32
{
    SENDER_CODE_ENTRY = 0,
    SENDER_MAIN       = 1,
    SENDER_HOA        = 2,
    SENDER_TDP        = 3,
    SENDER_FOO        = 4,
    SENDER_MOTL       = 5,
    SENDER_SOTB       = 6,
    SENDER_HFI        = 7,
    SENDER_DOW        = 8,
    SENDER_BOG        = 9,
    SENDER_FOH        = 10,
    SENDER_SW         = 11,
    SENDER_WG         = 12,
    SENDER_IC         = 13,
    SENDER_PR         = 14,

    // Sender for the GM "Clear redemption flags" text-input button.
    SENDER_GM_CLEAR    = 15,

    // Sender for the GM "Force re-deliver" override confirmation dialog.
    SENDER_GM_FORCE    = 16,

    // Sender for the GM "Send a code to a player" browse path and final
    // text-input submission.  Sits between force-delivery (16) and promo (18-21).
    SENDER_GM_SEND_CODE = 17,

    // Promo vendor (Garel Redrock / Tharl Stonebleeder) category sub-menu senders.
    SENDER_PROMO_MURLOC  = 18,
    SENDER_PROMO_CLASSIC = 19,
    SENDER_PROMO_STORE   = 20,
    SENDER_PROMO_EVENTS  = 21,

    // Action used on the root menu "Browse by expansion set" button.
    ACTION_OPEN_BROWSE    = 99,
    ACTION_OPEN_SEND_CODE = 98,   // Root "[GM] Send a code to a player..." button
};

// ============================================================
//  Item entry IDs for Landro's boxes — referenced by the
//  LandroBoxesMultiRedeem config check.
// ============================================================
static constexpr uint32 ITEM_LANDROS_GIFT_BOX = 54218;
static constexpr uint32 ITEM_LANDROS_PET_BOX  = 50301;

// Simple Stationery — the carrier item for boss-drop and GM-sent codes.
// Its text is read client-side once ITEM_FIELD_FLAG_READABLE is set.
static constexpr uint32 ITEM_SIMPLE_STATIONERY = 9311;

// Marker written into creature_loot_template.Comment for rows this module owns.
// OnStartup deletes rows matching BOSS_DROP_LOOT_COMMENT_PREFIX and nothing
// else, so stationery dropped by any other system survives a restart.
//
// BOSS_DROP_LOOT_COMMENT_PREFIX is a SQL LIKE pattern, so it must end in '%' and
// must stay pure ASCII.  BOSS_DROP_LOOT_COMMENT is the literal text stored in
// the column and must start with the pattern's stem.
static constexpr char const* BOSS_DROP_LOOT_COMMENT_PREFIX = "TCG code scroll%";
static constexpr char const* BOSS_DROP_LOOT_COMMENT         = "TCG code scroll - mod-tcg-vendors";

// Spell taught by the Warbot Ignition Key.  Red/Blue War Fuel are only
// displayed (and redeemable) after the player has learned this companion.
static constexpr uint32 WARBOT_PET_SPELL = 65682;

// ============================================================
//  TCGItem  —  one entry in a gossip browse sub-menu
//
//  isConsumable   When true in Mode 1: skip character_tcg_redeemed
//                 so the player can take the item repeatedly.
//                 In Mode 2 the code is still consumed; isConsumable
//                 only removes the per-character uniqueness gate,
//                 allowing a second legitimate code for the same
//                 consumable to be redeemed on the same character.
//
//  factionMount   Horde gets entries[0], Alliance gets entries[1].
//                 entries[0] is always the redemption key.
// ============================================================================
struct TCGItem
{
    std::string         displayName;
    std::vector<uint32> entries;
    bool                factionMount = false;
    bool                isConsumable = false;
    std::string         rewardGroupKey; // Mode 3: expected reward group key
    uint32              requiredSpell = 0; // 0 = no condition; else player->HasSpell() must be true
};

// ============================================================
//  RewardGroup  —  what a single code awards (Mode 2)
//  String key must match account_tcg_codes.reward_group and
//  the REWARD_GROUPS dict in tools/generate_codes.py.
// ============================================================
struct RewardGroup
{
    std::string         displayName;
    std::vector<uint32> itemEntries;
    bool                factionMount = false;
    bool                isConsumable = false;
};

// ============================================================
//  Master reward group catalog  (Mode 2 code path)
// ============================================================
static const std::map<std::string, RewardGroup> REWARD_GROUPS =
{
    // --- Heroes of Azeroth ---
    { "TCG_TABARD_OF_FLAME",           { "Tabard of Flame",                             { 23705 }        } },
    { "TCG_HIPPOGRYPH_HATCHLING",      { "Hippogryph Hatchling",                        { 23713 }        } },
    { "TCG_RIDING_TURTLE",             { "Riding Turtle",                               { 23720 }        } },

    // --- Through the Dark Portal ---
    { "TCG_PICNIC_BASKET",             { "Picnic Basket",                               { 32566 }        } },
    { "TCG_BANANA_CHARM",              { "Banana Charm",                                { 32588 }        } },
    { "TCG_IMP_IN_A_BALL",             { "Imp in a Ball",                               { 32542 }        } },

    // --- Fires of Outland ---
    { "TCG_GOBLIN_GUMBO_KETTLE",       { "Goblin Gumbo Kettle",                         { 33219 }        } },
    { "TCG_FISHING_CHAIR",             { "Fishing Chair",                               { 33223 }        } },
    { "TCG_SPECTRAL_TIGER",            { "Reins of the Spectral Tiger (both variants)", { 33224, 33225 } } },

    // --- March of the Legion ---
    { "TCG_PAPER_FLYING_MACHINE",      { "Paper Flying Machine Kit",                    { 34499 }        } },
    { "TCG_ROCKET_CHICKEN",            { "Rocket Chicken",                              { 34492 }        } },
    { "TCG_DRAGON_KITE",               { "Dragon Kite",                                 { 34493 }        } },

    // --- Servants of the Betrayer ---
    { "TCG_X51_NETHER_ROCKET",         { "X-51 Nether-Rocket (both variants)",          { 35225, 35226 } } },
    { "TCG_PET_BISCUIT",               { "Papa Hummel's Old-Fashioned Pet Biscuit",     { 35223 }, false, true } },
    { "TCG_GOBLIN_WEATHER_MACHINE",    { "Goblin Weather Machine - Prototype 01-B",     { 35227 }        } },

    // --- Hunt for Illidan ---
    { "TCG_PATH_OF_ILLIDAN",           { "Path of Illidan",                             { 38233 }, false, true } },
    { "TCG_DISCO",                     { "D.I.S.C.O.",                                  { 38301 }        } },
    { "TCG_SOUL_TRADER_BEACON",        { "Soul-Trader Beacon",                          { 38050 }        } },

    // --- Drums of War ---
    { "TCG_PARTY_GRENADE",             { "Party G.R.E.N.A.D.E.",                        { 38577 }, false, true } },
    { "TCG_FLAG_OF_OWNERSHIP",         { "The Flag of Ownership",                       { 38578 }        } },
    { "TCG_BIG_BATTLE_BEAR",           { "Big Battle Bear",                             { 38576 }        } },

    // --- Blood of Gladiators ---
    { "TCG_SANDBOX_TIGER",             { "Sandbox Tiger",                               { 45047 }, false, true } },
    { "TCG_EPIC_PURPLE_SHIRT",         { "Epic Purple Shirt",                           { 45037 }        } },
    { "TCG_FOAM_SWORD_RACK",           { "Foam Sword Rack",                             { 45063 }        } },

    // --- Fields of Honor ---
    { "TCG_PATH_OF_CENARIUS",          { "Path of Cenarius",                            { 46779 }, false, true } },
    { "TCG_OGRE_PINATA",               { "Ogre Pinata",                                 { 46780 }        } },
    { "TCG_MAGIC_ROOSTER_EGG",         { "Magic Rooster Egg",                           { 46778 }        } },

    // --- Scourgewar ---
    { "TCG_SCOURGEWAR_MINIMOUNT",      { "Scourgewar Mini-Mount",                       { 49288, 49289 }, true, true } },
    { "TCG_TUSKARR_KITE",              { "Tuskarr Kite",                                { 49287 }        } },
    { "TCG_SPECTRAL_TIGER_CUB",        { "Spectral Tiger Cub",                          { 49343 }        } },

    // --- Wrathgate ---
    { "TCG_LANDROS_GIFT_BOX",          { "Landro's Gift Box",                           { 54218 }        } },
    { "TCG_INSTANT_STATUE_PEDESTAL",   { "Instant Statue Pedestal",                     { 54212 }        } },
    { "TCG_BLAZING_HIPPOGRYPH",        { "Blazing Hippogryph",                          { 54069 }        } },

    // --- Icecrown ---
    { "TCG_PAINT_BOMB",                { "Paint Bomb",                                  { 54455 }, false, true } },
    { "TCG_ETHEREAL_PORTAL",           { "Ethereal Portal",                             { 54452 }        } },
    { "TCG_WOOLY_WHITE_RHINO",         { "Wooly White Rhino",                           { 54068 }        } },

    // --- Points Redemption ---
    { "TCG_TABARD_OF_FROST",           { "Tabard of Frost",                             { 23709 }        } },
    { "TCG_PERPETUAL_PURPLE_FIREWORK", { "Perpetual Purple Firework",                   { 23714 }        } },
    { "TCG_CARVED_OGRE_IDOL",          { "Carved Ogre Idol",                            { 23716 }        } },
    { "TCG_TABARD_OF_THE_ARCANE",      { "Tabard of the Arcane",                        { 38310 }        } },
    { "TCG_TABARD_OF_BRILLIANCE",      { "Tabard of Brilliance",                        { 38312 }        } },
    { "TCG_TABARD_OF_THE_DEFENDER",    { "Tabard of the Defender",                      { 38314 }        } },
    { "TCG_TABARD_OF_FURY",            { "Tabard of Fury",                              { 38313 }        } },
    { "TCG_TABARD_OF_NATURE",          { "Tabard of Nature",                            { 38309 }        } },
    { "TCG_TABARD_OF_THE_VOID",        { "Tabard of the Void",                          { 38311 }        } },
    { "TCG_LANDROS_PET_BOX",           { "Landro's Pet Box",                            { 50301 }        } },

    // --- Blizzcon promotional ---
    { "BLIZZCON_MURKY",                { "Murky (Blue Murloc Egg)",                     { 20371 }        } },
    { "BLIZZCON_MURLOC_COSTUME",       { "Murloc Costume",                              { 33079 }        } },
    { "BLIZZCON_BIG_BLIZZARD_BEAR",    { "Big Blizzard Bear",                           { 43599 }        } },

    // --- Murloc companion eggs ---
    { "PROMO_GURKY",             { "Gurky (Pink Murloc Egg)",   { 22114 }        } },
    { "PROMO_ORANGE_MURLOC_EGG", { "Orange Murloc Egg",         { 20651 }        } },
    { "PROMO_WHITE_MURLOC_EGG",  { "White Murloc Egg",          { 22780 }        } },
    { "PROMO_HEAVY_MURLOC_EGG",  { "Heavy Murloc Egg",          { 46802 }        } },
    { "PROMO_MURKIMUS_SPEAR",    { "Murkimus' Little Spear",    { 45180 }        } },

    // --- Classic & Special Promotions ---
    { "PROMO_ZERGLING_LEASH",    { "Zergling Leash",            { 13582 }        } },
    { "PROMO_PANDA_COLLAR",      { "Panda Collar",              { 13583 }        } },
    { "PROMO_DIABLO_STONE",      { "Diablo Stone",              { 13584 }        } },
    { "PROMO_NETHERWHELP",       { "Netherwhelp's Collar",      { 25535 }        } },
    { "PROMO_FROSTYS_COLLAR",    { "Frosty's Collar",           { 39286 }        } },
    { "WWI_TYRAELS_HILT",           { "Tyrael's Hilt",             { 39656 }        } },
    { "PROMO_WARBOT_KEY",        { "Warbot Ignition Key",       { 46767 }        } },

    // --- Blizzard Store ---
    { "PROMO_ENCHANTED_ONYX",    { "Enchanted Onyx",            { 48527 }        } },
    { "PROMO_CORE_HOUND_PUP",    { "Core Hound Pup",            { 49646 }        } },
    { "PROMO_GRYPHON_HATCHLING", { "Gryphon Hatchling",         { 49662 }        } },
    { "PROMO_WIND_RIDER_CUB",    { "Wind Rider Cub",            { 49663 }        } },
    { "PROMO_PANDAREN_MONK",     { "Pandaren Monk",             { 49665 }        } },

    // --- Special Events & Tournaments ---
    { "PROMO_LIL_PHYLACTERY",    { "Lil' Phylactery",           { 49693 }        } },
    { "PROMO_LIL_XT",            { "Lil' XT",                   { 54847 }        } },
    { "PROMO_MINI_THOR",         { "Mini Thor",                 { 56806 }        } },
    { "PROMO_ONYXIAN_WHELPLING", { "Onyxian Whelpling",         { 49362 }        } },
};

// ============================================================
//  Landro gossip catalog  (Mode 1 all-player browse,
//                          Mode 2 GM-only browse,
//                          Mode 3 item-specific code entry)
// ============================================================
static const std::map<uint32, std::vector<TCGItem>> LANDRO_CATALOG =
{
    { SENDER_HOA,  {
        { "Tabard of Flame",                             { 23705 },        false, false, "TCG_TABARD_OF_FLAME"           },
        { "Hippogryph Hatchling",                        { 23713 },        false, false, "TCG_HIPPOGRYPH_HATCHLING"      },
        { "Riding Turtle",                               { 23720 },        false, false, "TCG_RIDING_TURTLE"             },
    }},
    { SENDER_TDP,  {
        { "Picnic Basket",                               { 32566 },        false, false, "TCG_PICNIC_BASKET"             },
        { "Banana Charm",                                { 32588 },        false, false, "TCG_BANANA_CHARM"              },
        { "Imp in a Ball",                               { 32542 },        false, false, "TCG_IMP_IN_A_BALL"             },
    }},
    { SENDER_FOO,  {
        { "Goblin Gumbo Kettle",                         { 33219 },        false, false, "TCG_GOBLIN_GUMBO_KETTLE"       },
        { "Fishing Chair",                               { 33223 },        false, false, "TCG_FISHING_CHAIR"             },
        { "Reins of the Spectral Tiger (both variants)", { 33224, 33225 }, false, false, "TCG_SPECTRAL_TIGER"            },
    }},
    { SENDER_MOTL, {
        { "Paper Flying Machine Kit",                    { 34499 },        false, false, "TCG_PAPER_FLYING_MACHINE"      },
        { "Rocket Chicken",                              { 34492 },        false, false, "TCG_ROCKET_CHICKEN"            },
        { "Dragon Kite",                                 { 34493 },        false, false, "TCG_DRAGON_KITE"               },
    }},
    { SENDER_SOTB, {
        { "X-51 Nether-Rocket (both variants)",          { 35225, 35226 }, false, false, "TCG_X51_NETHER_ROCKET"         },
        { "Papa Hummel's Old-Fashioned Pet Biscuit",    { 35223 },        false, true,  "TCG_PET_BISCUIT"               },
        { "Goblin Weather Machine - Prototype 01-B",     { 35227 },        false, false, "TCG_GOBLIN_WEATHER_MACHINE"    },
    }},
    { SENDER_HFI,  {
        { "Path of Illidan",                             { 38233 },        false, true,  "TCG_PATH_OF_ILLIDAN"           },
        { "D.I.S.C.O.",                                  { 38301 },        false, false, "TCG_DISCO"                     },
        { "Soul-Trader Beacon",                          { 38050 },        false, false, "TCG_SOUL_TRADER_BEACON"        },  // permanent companion
    }},
    { SENDER_DOW,  {
        { "Party G.R.E.N.A.D.E.",                        { 38577 },        false, true,  "TCG_PARTY_GRENADE"             },
        { "The Flag of Ownership",                       { 38578 },        false, false, "TCG_FLAG_OF_OWNERSHIP"         },
        { "Big Battle Bear",                             { 38576 },        false, false, "TCG_BIG_BATTLE_BEAR"           },
    }},
    { SENDER_BOG,  {
        { "Sandbox Tiger",                               { 45047 },        false, true,  "TCG_SANDBOX_TIGER"             },
        { "Epic Purple Shirt",                           { 45037 },        false, false, "TCG_EPIC_PURPLE_SHIRT"         },
        { "Foam Sword Rack",                             { 45063 },        false, false, "TCG_FOAM_SWORD_RACK"           },
    }},
    { SENDER_FOH,  {
        { "Path of Cenarius",                            { 46779 },        false, true,  "TCG_PATH_OF_CENARIUS"          },
        { "Ogre Pinata",                                 { 46780 },        false, false, "TCG_OGRE_PINATA"               },
        { "Magic Rooster Egg",                           { 46778 },        false, false, "TCG_MAGIC_ROOSTER_EGG"         },
    }},
    { SENDER_SW,   {
        { "Scourgewar Mini-Mount",                       { 49288, 49289 }, true,  true,  "TCG_SCOURGEWAR_MINIMOUNT"      },
        { "Tuskarr Kite",                                { 49287 },        false, false, "TCG_TUSKARR_KITE"              },
        { "Spectral Tiger Cub",                          { 49343 },        false, false, "TCG_SPECTRAL_TIGER_CUB"        },
    }},
    { SENDER_WG,   {
        { "Landro's Gift Box",                          { 54218 },        false, false, "TCG_LANDROS_GIFT_BOX"          },
        { "Instant Statue Pedestal",                     { 54212 },        false, false, "TCG_INSTANT_STATUE_PEDESTAL"   },
        { "Blazing Hippogryph",                          { 54069 },        false, false, "TCG_BLAZING_HIPPOGRYPH"        },
    }},
    { SENDER_IC,   {
        { "Paint Bomb",                                  { 54455 },        false, true,  "TCG_PAINT_BOMB"                },
        { "Ethereal Portal",                             { 54452 },        false, false, "TCG_ETHEREAL_PORTAL"           },
        { "Wooly White Rhino",                           { 54068 },        false, false, "TCG_WOOLY_WHITE_RHINO"         },
    }},
    { SENDER_PR,   {
        { "Tabard of Frost",                             { 23709 },        false, false, "TCG_TABARD_OF_FROST"           },
        { "Perpetual Purple Firework",                   { 23714 },        false, false, "TCG_PERPETUAL_PURPLE_FIREWORK" },
        { "Carved Ogre Idol",                            { 23716 },        false, false, "TCG_CARVED_OGRE_IDOL"          },
        { "Tabard of the Arcane",                        { 38310 },        false, false, "TCG_TABARD_OF_THE_ARCANE"      },
        { "Tabard of Brilliance",                        { 38312 },        false, false, "TCG_TABARD_OF_BRILLIANCE"      },
        { "Tabard of the Defender",                      { 38314 },        false, false, "TCG_TABARD_OF_THE_DEFENDER"    },
        { "Tabard of Fury",                              { 38313 },        false, false, "TCG_TABARD_OF_FURY"            },
        { "Tabard of Nature",                            { 38309 },        false, false, "TCG_TABARD_OF_NATURE"          },
        { "Tabard of the Void",                          { 38311 },        false, false, "TCG_TABARD_OF_THE_VOID"        },
        { "Landro's Pet Box",                           { 50301 },        false, false, "TCG_LANDROS_PET_BOX"           },
    }},
};

static const std::map<uint32, std::string> EXPANSION_NAMES =
{
    { SENDER_HOA,  "Heroes of Azeroth"        },
    { SENDER_TDP,  "Through the Dark Portal"  },
    { SENDER_FOO,  "Fires of Outland"         },
    { SENDER_MOTL, "March of the Legion"      },
    { SENDER_SOTB, "Servants of the Betrayer" },
    { SENDER_HFI,  "Hunt for Illidan"         },
    { SENDER_DOW,  "Drums of War"             },
    { SENDER_BOG,  "Blood of Gladiators"      },
    { SENDER_FOH,  "Fields of Honor"          },
    { SENDER_SW,   "Scourgewar"               },
    { SENDER_WG,   "Wrathgate"                },
    { SENDER_IC,   "Icecrown"                 },
    { SENDER_PR,   "Points Redemption"        },
};

// ============================================================
//  Worldwide Invitational catalog  (Edward Cairn / Ian Drake)
//
//  These two vendors handle Tyrael's Hilt exclusively — the reward
//  granted at the 2008 Worldwide Invitational event in Paris.
// ============================================================
static const std::vector<TCGItem> TYRAELS_CATALOG =
{
    { "Tyrael's Hilt", { 39656 }, false, false, "WWI_TYRAELS_HILT" },
};

static const std::map<uint32, std::string> PROMO_CATEGORY_NAMES =
{
    { SENDER_PROMO_MURLOC,  "Murloc Companions"             },
    { SENDER_PROMO_CLASSIC, "Classic & Special Promotions"  },
    { SENDER_PROMO_STORE,   "Blizzard Store"                },
    { SENDER_PROMO_EVENTS,  "Special Events & Tournaments"  },
};

// ============================================================
//  Blizzcon vendor catalog  (Ransin Donner / Zas'Tysh)
// ============================================================
static const std::vector<TCGItem> BLIZZCON_CATALOG =
{
    { "Murky (Blue Murloc Egg)", { 20371 }, false, false, "BLIZZCON_MURKY"             },
    { "Murloc Costume",          { 33079 }, false, false, "BLIZZCON_MURLOC_COSTUME"    },
    { "Big Blizzard Bear",       { 43599 }, false, false, "BLIZZCON_BIG_BLIZZARD_BEAR" },
};

// ============================================================
//  Promo vendor catalog  (Garel Redrock / Tharl Stonebleeder)
// ============================================================
static const std::map<uint32, std::vector<TCGItem>> PROMO_CATALOG =
{
    { SENDER_PROMO_MURLOC,  {
        { "Gurky (Pink Murloc Egg)",   { 22114 }, false, false, "PROMO_GURKY"              },
        { "Orange Murloc Egg",         { 20651 }, false, false, "PROMO_ORANGE_MURLOC_EGG"  },
        { "White Murloc Egg",          { 22780 }, false, false, "PROMO_WHITE_MURLOC_EGG"   },
        { "Heavy Murloc Egg",          { 46802 }, false, false, "PROMO_HEAVY_MURLOC_EGG"   },
        { "Murkimus' Little Spear",    { 45180 }, false, false, "PROMO_MURKIMUS_SPEAR"     },
    }},
    { SENDER_PROMO_CLASSIC, {
        { "Zergling Leash",            { 13582 }, false, false, "PROMO_ZERGLING_LEASH"     },
        { "Panda Collar",              { 13583 }, false, false, "PROMO_PANDA_COLLAR"       },
        { "Diablo Stone",              { 13584 }, false, false, "PROMO_DIABLO_STONE"       },
        { "Netherwhelp's Collar",      { 25535 }, false, false, "PROMO_NETHERWHELP"        },
        { "Frosty's Collar",           { 39286 }, false, false, "PROMO_FROSTYS_COLLAR"     },
        { "Warbot Ignition Key",        { 46767 }, false, false, "PROMO_WARBOT_KEY"         },
        { "Red War Fuel",              { 46766 }, false, true,  "PROMO_RED_WAR_FUEL",  WARBOT_PET_SPELL },
        { "Blue War Fuel",             { 46765 }, false, true,  "PROMO_BLUE_WAR_FUEL", WARBOT_PET_SPELL },
    }},
    { SENDER_PROMO_STORE,   {
        { "Enchanted Onyx",            { 48527 }, false, false, "PROMO_ENCHANTED_ONYX"     },
        { "Core Hound Pup",            { 49646 }, false, false, "PROMO_CORE_HOUND_PUP"     },
        { "Gryphon Hatchling",         { 49662 }, false, false, "PROMO_GRYPHON_HATCHLING"  },
        { "Wind Rider Cub",            { 49663 }, false, false, "PROMO_WIND_RIDER_CUB"     },
        { "Pandaren Monk",             { 49665 }, false, false, "PROMO_PANDAREN_MONK"      },
    }},
    { SENDER_PROMO_EVENTS,  {
        { "Lil' Phylactery",           { 49693 }, false, false, "PROMO_LIL_PHYLACTERY"     },
        { "Lil' XT",                   { 54847 }, false, false, "PROMO_LIL_XT"             },
        { "Mini Thor",                 { 56806 }, false, false, "PROMO_MINI_THOR"           },
        { "Onyxian Whelpling",         { 49362 }, false, false, "PROMO_ONYXIAN_WHELPLING"  },
    }},
};

// ============================================================
//  C O N F I G   H E L P E R S
// ============================================================

static int GetVendorMode()
{
    int mode = sConfigMgr->GetOption<int>("TCGVendors.Mode", MODE_BLIZZLIKE);
    if (mode < MODE_DISABLED || mode > MODE_ITEM_CODE)
    {
        LOG_WARN("module",
            "mod-tcg-vendors: TCGVendors.Mode has unrecognised value {} — "
            "falling back to Mode 2 (Blizz-like).", mode);
        return MODE_BLIZZLIKE;
    } else {
        return mode;
    }
}
// Returns true when Landro's Gift Box and Pet Box should be treated
// as consumable (multi-redeemable).  Reads TCGVendors.LandroBoxesMultiRedeem.
static bool GetLandroBoxIsConsumable()
{
    return sConfigMgr->GetOption<bool>("TCGVendors.LandroBoxesMultiRedeem", false);
}

// Convenience: given a TCGItem or RewardGroup entry, resolves whether
// the box override applies to it.
static bool IsItemConsumable(uint32 redemptionKey, bool baseConsumable)
{
    if (redemptionKey == ITEM_LANDROS_GIFT_BOX || redemptionKey == ITEM_LANDROS_PET_BOX)
        return GetLandroBoxIsConsumable();
    return baseConsumable;
}

// ============================================================
//  H E L P E R   F U N C T I O N S
// ============================================================

// Helper: Map itemId to vendor name
// Helper: Map an item entry to the NPC vendor name(s) that handle it.
//
// Rather than maintaining a hardcoded list, we scan the three catalogs
// directly — this stays automatically correct as catalogs change.
//
// Blizzcon and Promo items are sold at paired Alliance/Horde vendors;
// we list both so the stationery is useful regardless of the reader's
// faction.
static std::string GetVendorForItem(uint32 itemId)
{
    // TCG expansion items — Landro Longshot, Booty Bay
    for (auto const& [sender, items] : LANDRO_CATALOG)
        for (auto const& tcgItem : items)
            for (uint32 e : tcgItem.entries)
                if (e == itemId)
                    return "Landro Longshot in Booty Bay";

    // Blizzcon promotional items — Ransin Donner (Alliance) / Zas'Tysh (Horde)
    for (auto const& tcgItem : BLIZZCON_CATALOG)
        for (uint32 e : tcgItem.entries)
            if (e == itemId)
                return "Ransin Donner in Ironforge (Alliance) or Zas'Tysh in Orgrimmar (Horde)";

    // Additional promotional items — Garel Redrock (Alliance) / Tharl Stonebleeder (Horde)
    for (auto const& [sender, items] : PROMO_CATALOG)
        for (auto const& tcgItem : items)
            for (uint32 e : tcgItem.entries)
                if (e == itemId)
                    return "Garel Redrock in Ironforge (Alliance) or Tharl Stonebleeder in Orgrimmar (Horde)";

    // WorldWide Invitational — Ian Drake (Alliance) / Edward Cairn (Horde)
    for (auto const& tcgItem : TYRAELS_CATALOG)
        for (uint32 e : tcgItem.entries)
            if (e == itemId)
                return "Ian Drake in Stormwind (Alliance) or Edward Cairn in Undercity (Horde)";

    // Fallback — should not be reached for any configured item
    LOG_WARN("module",
        "mod-tcg-vendors: GetVendorForItem: item {} not found in any catalog. "
        "Check TCGVendors.BossDrop.ItemIds.", itemId);
    return "the TCG vendor NPC";
}

// Helper: Get item name from item ID (fallback to numeric ID string if not found)
static std::string GetItemName(uint32 itemId)
{
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
        return proto->Name1;
    return std::to_string(itemId);
}

// Helper: Build the flavor text message for a stationery drop
static std::string BuildStationeryText(const std::string& bossName,
                                       const std::string& itemName,
                                       const std::string& code,
                                       uint32             itemId)
{
    std::string vendor = GetVendorForItem(itemId);
    return "Congratulations! You have defeated " + bossName + ".\n\n"
           "Enclosed is a code redeemable for: " + itemName + ".\n\n"
           "Code:\n" + code + "\n\n"
           "To redeem this code, visit " + vendor + " and speak with the TCG vendor NPC.\n\n"
           "This code is single-use. Thank you for playing!";
}

// Helper: Make a stationery item right-click-readable in the client.
//
// How the text reaches the client in this core:
//
//   1. Item::SetText() stores the string in the in-memory Item (m_text).
//      Item::SaveToDB() persists m_text into item_instance.text
//      (Item.cpp: stmt->SetData(++index, m_text)), so the column is written
//      by the normal save path and needs no hand-rolled UPDATE.
//
//   2. ITEM_FIELD_FLAG_READABLE (0x00000200) must be set on the item's flags
//      field.  This is what makes the client offer "Read" in the item tooltip
//      and send CMSG_ITEM_TEXT_QUERY.
//
//   3. WorldSession::HandleItemTextQuery answers that packet from the live
//      object — "data << item->GetText()" — not from a fresh
//      item_instance.text lookup.  So the flag alone plus an in-memory SetText
//      is sufficient for the player to read the scroll immediately; the column
//      only matters for persistence across a restart.
//
// SetText alone is insufficient without the readable flag, and the flag alone
// is insufficient without SetText.
static void StampItemText(Item* item, Player* owner, const std::string& text)
{
    // 1. Set in-memory text — populates m_text, persisted by SaveToDB.
    item->SetText(text);

    // 2. Set ITEM_FIELD_FLAG_READABLE (0x00000200) on the item's flags field.
    item->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_READABLE);

    // 3. Mark dirty so the flag update is sent to the client on the next
    //    update cycle and written to item_instance on the next autosave.
    if (owner)
        item->SetState(ITEM_CHANGED, owner);
}

// Helper: Create a stationery item (9311) with text set so the client shows
// the right-click "Read" option.
static Item* CreateStationeryWithText(Player* owner, const std::string& text)
{
    Item* item = Item::CreateItem(ITEM_SIMPLE_STATIONERY, 1, owner);
    if (!item)
        return nullptr;
    StampItemText(item, nullptr, text);  // no owner yet — SaveToDB will persist
    return item;
}

// Helper: Generate a random redemption code, formatted XXXX-XXXX-XXXX-XXXX.
//
// Uses the OpenSSL-backed CSPRNG (Acore::Crypto::GetRandomBytes) rather than
// urand().  urand() is an SFMT Mersenne Twister with per-thread state: codes
// produced from it are predictable to anyone who can recover that state from
// observed output.  Redemption codes are bearer tokens, so they must not be
// predictable.  The charset must stay identical to IsValidCodeFormat() and to
// CHARSET in tools/generate_codes.py.
static std::string GenerateRandomCode()
{
    // 31 unambiguous characters: A-Z minus I/L/O, plus 2-9 minus 0/1.
    static const char alphanum[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    constexpr size_t ALPHANUM_SIZE = sizeof(alphanum) - 1;   // 31

    std::string raw;
    raw.reserve(16);

    // Draw uniformly over 0..255 by rejection sampling: 256 is not a multiple
    // of 31, so a plain modulo would bias the first ALPHANUM_SIZE-1 symbols.
    constexpr uint32_t RANGE = 256;
    constexpr uint32_t LIMIT = RANGE - (RANGE % ALPHANUM_SIZE);   // 248

    while (raw.size() < 16)
    {
        std::array<uint8, 16> buf{};
        Acore::Crypto::GetRandomBytes(buf);

        for (uint8 byte : buf)
        {
            if (byte >= LIMIT)
                continue;

            raw += alphanum[byte % ALPHANUM_SIZE];

            if (raw.size() == 16)
                break;
        }
    }

    return raw.substr(0, 4) + "-" + raw.substr(4, 4) + "-" +
           raw.substr(8, 4) + "-" + raw.substr(12, 4);
}

// Helper: Find reward_group key for an itemId
static std::string GetRewardGroupForItem(uint32 itemId)
{
    for (const auto& pair : REWARD_GROUPS)
    {
        for (uint32 entry : pair.second.itemEntries)
        {
            if (entry == itemId)
                return pair.first;
        }
    }
    return "";
}

// Helper: Insert code into the module's code tracking table.
//
// DirectExecute (blocking) rather than Execute (asynchronous): the code is
// inserted here and then handed to the player on a stationery item.  If the
// INSERT were still queued when the player tried to redeem it, the redemption
// lookup would miss and report the code as unrecognised.
//
// INSERT IGNORE matches tools/generate_codes.py: a duplicate primary key is
// skipped rather than raising a MySQL error.  DirectExecute still returns void,
// so a collision cannot be reported to the caller — but with a CSPRNG over
// 31^16 codes the odds are negligible.
static void InsertCodeToDatabase(const std::string& code, const std::string& rewardGroup)
{
    std::string escCode = code;
    std::string escGroup = rewardGroup;
    CharacterDatabase.EscapeString(escCode);
    CharacterDatabase.EscapeString(escGroup);
    CharacterDatabase.DirectExecute(
        "INSERT IGNORE INTO account_tcg_codes (code, reward_group, redeemed, account_id, character_guid, redeemed_date) "
        "VALUES ('{}', '{}', 0, NULL, NULL, NULL)",
        escCode, escGroup);
}

static bool HasRedeemed(uint32 guid, uint32 itemEntry)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM character_tcg_redeemed WHERE guid = {} AND item_entry = {}",
        guid, itemEntry);
    return result != nullptr;
}

// Helper: Record that a character has received a unique (non-consumable) item.
//
// DirectExecute (blocking) so the row is committed before this function
// returns.  HasRedeemed() reads through the synchronous pool while an
// Execute() write would sit on the asynchronous pool, so an asynchronous
// insert could be invisible to the very next click and hand out a second copy
// of a one-per-character reward.
static void MarkRedeemed(uint32 guid, uint32 itemEntry)
{
    CharacterDatabase.DirectExecute(
        "INSERT IGNORE INTO character_tcg_redeemed (guid, item_entry) VALUES ({}, {})",
        guid, itemEntry);
}


    uint32 GetPromoStackSize(uint32 itemEntry)
    {
        switch (itemEntry)
        {
            case 46766: // Red War Fuel
            case 46765: // Blue War Fuel
                return 5;

            default:
                return 1;
        }
    }

// Resolve which items to physically hand to the player, handling
// the faction-mount split.
static std::vector<uint32> ResolveItems(Player* player,
                                        const std::vector<uint32>& entries,
                                        bool factionMount)
{
    if (factionMount && entries.size() >= 2)
    {
        bool isHorde = (player->GetTeamId() == TEAM_HORDE);
        return { entries[isHorde ? 0 : 1] };
    }
    return entries;
}

// Attempt to place one item into the player's bags.
// Returns true if it fit, false if bags are full.
static bool TryAddItemToBags(Player* player, uint32 entry)
{
    ItemPosCountVec dest;
    return player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, GetPromoStackSize(entry)) == EQUIP_ERR_OK;
}

// Mail a list of items to the player as a fallback when bags are full.
// Each item is sent in a separate mail so the player can retrieve them
// individually from the mailbox without needing free bag space for all
// of them at once.
//
// Returns true only if every item was created and handed to the mail draft.
// A false return means the player did NOT receive the full set, so the caller
// must not record the redemption — otherwise the code/flag is burned with
// nothing given and the item becomes unobtainable.
static bool MailItemsToPlayer(Player* player, Creature* creature,
                              const std::vector<uint32>& entries,
                              const std::string& displayName)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bool allCreated = true;

    for (uint32 entry : entries)
    {
        Item* item = Item::CreateItem(entry, GetPromoStackSize(entry), player);
        if (!item)
        {
            LOG_ERROR("module",
                "mod-tcg-vendors: Failed to create item {} for mail fallback "
                "(player guid {}).", entry, player->GetGUID().GetCounter());
            allCreated = false;
            continue;
        }
        item->SaveToDB(trans);

        MailDraft(
            "Your Reward: " + displayName,
            "Your bags were full when you redeemed your item at " +
            std::string(creature->GetName()) + ", so it was mailed to you instead.\n\n"
            "Here you go!  Have a nice day!")
            .AddItem(item)
            .SendMailTo(trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(creature));
    }

    CharacterDatabase.CommitTransaction(trans);
    return allCreated;
}

// Return values from TryDeliverItem — lets callers send the right whisper
// without needing to re-query the database or add out-parameters.
enum DeliveryResult
{
    DELIVERY_FAILED,      // Already redeemed — nothing was given, whisper already sent
    DELIVERY_BAGS,        // Items placed directly into player's bags
    DELIVERY_MAIL,        // Bags were full — items mailed, whisper already sent
    DELIVERY_ERROR,       // Item could not be created — nothing given, no flag recorded
};

// Central delivery function used by all modes and both NPC classes.
static DeliveryResult TryDeliverItem(Player*                    player,
                                     Creature*                  creature,
                                     const std::vector<uint32>& allEntries,
                                     uint32                     redemptionKey,
                                     bool                       factionMount,
                                     bool                       consumable,
                                     const std::string&         displayName)
{
    // Resolve first: for a faction mount the key that must be recorded is the
    // variant this player actually receives, not allEntries[0].  Using
    // allEntries[0] for both factions would let an Alliance character claim the
    // mount twice — once through the GM path (which records the resolved
    // variant) and once through the player path.
    //
    // Note for existing databases: rows written before this fix used
    // allEntries[0] for both factions, so an Alliance character that already
    // redeemed 49288 may be able to claim 49289 once after upgrading.  That is
    // a one-off cosmetic duplicate, deliberately not migrated.
    std::vector<uint32> toGive       = ResolveItems(player, allEntries, factionMount);
    uint32              effectiveKey = toGive.empty() ? redemptionKey : toGive.front();

    if (!consumable && HasRedeemed(player->GetGUID().GetCounter(), effectiveKey))
    {
        creature->Whisper(
            "Your character has already received \"" + displayName + "\".",
            LANG_UNIVERSAL, player);
        return DELIVERY_FAILED;
    }

    // Check whether every item in the set fits in bags right now.
    bool bagsFull = false;
    for (uint32 entry : toGive)
    {
        if (!TryAddItemToBags(player, entry))
        {
            bagsFull = true;
            break;
        }
    }

    if (bagsFull)
    {
        // Only record the redemption if the mail really went out.  Recording it
        // on failure would burn the code/flag with nothing delivered.
        bool mailed = MailItemsToPlayer(player, creature, toGive, displayName);

        if (!consumable && mailed)
            MarkRedeemed(player->GetGUID().GetCounter(), effectiveKey);

        if (mailed)
            creature->Whisper(
                "Your bags are full! \"" + displayName + "\" has been mailed to you. "
                "Visit any mailbox to collect it.",
                LANG_UNIVERSAL, player);
        else
            creature->Whisper(
                "\"" + displayName + "\" could not be created and was NOT mailed. "
                "Your code has not been used — please contact a Game Master.",
                LANG_UNIVERSAL, player);

        return mailed ? DELIVERY_MAIL : DELIVERY_ERROR;
    }

    // Bags have room — deliver directly.
    for (uint32 entry : toGive)
        player->AddItem(entry, GetPromoStackSize(entry));

    if (!consumable)
        MarkRedeemed(player->GetGUID().GetCounter(), effectiveKey);

    return DELIVERY_BAGS;
}

// ============================================================
//  C O D E   R E D E M P T I O N   (Mode 2)
// ============================================================

// Serialises the read-validate-burn sequence of a code redemption.
//
// WHY THIS IS NEEDED:
// The sequence "SELECT redeemed -> branch -> UPDATE redeemed=1" is not atomic,
// and AzerothCore's database layer cannot make it atomic for us:
//   - DatabaseWorkerPool::Execute() is asynchronous and returns void, so the
//     number of affected rows is not observable.
//   - Query() and Execute() run on *different* connections (IDX_SYNCH vs
//     IDX_ASYNC pools), so there is no read-after-write ordering between them.
// Without serialisation, two simultaneous submissions (a double-click, a
// client macro, or two players entering the same code) can both read
// redeemed = 0 and both receive the reward.
//
// The lock below closes the in-process race; the matching "AND redeemed = 0"
// predicate in the burn UPDATE is the second line of defence at the database
// level, so a code can never be marked used twice even if some future code
// path skips this lock.
//
// The lock is deliberately held only across the check and the burn, not
// across item creation or mail delivery.
static std::mutex s_codeRedeemMutex;

static std::string NormalizeCode(const std::string& raw)
{
    std::string code = raw;
    code.erase(0, code.find_first_not_of(" \t\r\n"));
    auto last = code.find_last_not_of(" \t\r\n");
    if (last != std::string::npos)
        code.erase(last + 1);
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return code;
}

// Validates XXXX-XXXX-XXXX-XXXX using the same unambiguous charset
// as tools/generate_codes.py (A-Z excl. I,L,O + 2-9 excl. 0,1).
static bool IsValidCodeFormat(const std::string& code)
{
    if (code.size() != 19)
        return false;
    if (code[4] != '-' || code[9] != '-' || code[14] != '-')
        return false;

    static const std::string VALID_CHARS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (size_t i = 0; i < code.size(); ++i)
    {
        if (i == 4 || i == 9 || i == 14)
            continue;
        if (VALID_CHARS.find(code[i]) == std::string::npos)
            return false;
    }
    return true;
}

static void HandleCodeRedemption(Player*            player,
                                 Creature*          creature,
                                 const std::string& rawCode)
{
    std::string code = NormalizeCode(rawCode);

    // ---- 1. Format check
    if (!IsValidCodeFormat(code))
    {
        creature->Whisper(
            "That doesn't look like a valid code. "
            "Codes follow the format XXXX-XXXX-XXXX-XXXX. "
            "Please check your code and try again.",
            LANG_UNIVERSAL, player);
        return;
    }

    // ---- 2. Database lookup
    std::string escapedCode = code;
    CharacterDatabase.EscapeString(escapedCode);

    // Everything that has to be atomic lives in this scope: the lookup, the
    // validation, and the burn.  TryDeliverItem deliberately runs after the lock
    // is released, which keeps the serialised DB round-trip as short as possible
    // and avoids holding a code-redeem lock across item creation and mail
    // commits.
    //
    // REWARD_GROUPS is static const and never mutated after startup, so
    // holding a pointer into it outside the lock is safe.
    RewardGroup const* group    = nullptr;
    uint32             redeemKey = 0;
    bool               consumable = false;

    {
        std::lock_guard<std::mutex> redeemGuard(s_codeRedeemMutex);

        QueryResult result = CharacterDatabase.Query(
            "SELECT reward_group, redeemed "
            "FROM account_tcg_codes "
            "WHERE code = '{}'",
            escapedCode);

        if (!result)
        {
            creature->Whisper(
                "That code was not recognised. "
                "Please double-check the code and try again.",
                LANG_UNIVERSAL, player);
            return;
        }

        Field*      fields      = result->Fetch();
        std::string rewardGroup = fields[0].Get<std::string>();
        bool        redeemed    = fields[1].Get<bool>();

        // ---- 3. Already used?
        if (redeemed)
        {
            creature->Whisper(
                "That code has already been redeemed. Each code may only be used once.",
                LANG_UNIVERSAL, player);
            return;
        }

        // ---- 4. Reward group lookup
        auto groupIt = REWARD_GROUPS.find(rewardGroup);
        if (groupIt == REWARD_GROUPS.end())
        {
            creature->Whisper(
                "Your code is valid but references an unknown reward. "
                "Please contact a Game Master for assistance.",
                LANG_UNIVERSAL, player);
            LOG_ERROR("module",
                "mod-tcg-vendors: Code '{}' references unknown reward_group '{}'. "
                "Check the account_tcg_codes table.",
                code, rewardGroup);
            return;
        }

        // Every group in REWARD_GROUPS has at least one item entry today, but
        // that is a data invariant rather than a code guarantee.  Reading front()
        // on an empty vector is undefined behaviour and would take the server
        // down inside a player-triggered path, so it is checked.
        if (groupIt->second.itemEntries.empty())
        {
            creature->Whisper(
                "That reward is misconfigured and cannot be delivered. "
                "Please contact a Game Master.",
                LANG_UNIVERSAL, player);
            LOG_ERROR("module",
                "mod-tcg-vendors: REWARD_GROUPS['{}'] has no item entries. "
                "Add at least one before issuing codes for it.", rewardGroup);
            return;
        }

        group     = &groupIt->second;
        redeemKey = group->itemEntries.front();

        // ---- 5. Resolve consumable flag (box override applies here)
        consumable = IsItemConsumable(redeemKey, group->isConsumable);

        // ---- 6. Burn the code.  Delivery happens after this scope.
        //
        // The "AND redeemed = 0" predicate is what actually guarantees
        // single-use: InnoDB serialises the row lock, and the second arriving
        // UPDATE re-evaluates the predicate after acquiring it and matches zero
        // rows.  Combined with s_codeRedeemMutex (which stops the loser from
        // reaching this point in the same process) a code cannot be redeemed
        // twice.
        //
        // Burning before delivery is deliberate.  If the server dies between the
        // burn and AddItem, a GM can inspect the table and re-deliver through the
        // GM browse path; the reverse order would let a crash hand out a second
        // copy.
        //
        // DirectExecute, not Execute: Execute() is asynchronous and runs on the
        // IDX_ASYNC pool, so its write is not yet applied when this returns.  A
        // second redemption blocked on s_codeRedeemMutex would then run its
        // verification Query() on the IDX_SYNCH pool, not see redeemed = 1, and
        // hand out a second copy.  DirectExecute blocks on the synchronous pool,
        // so the burn is committed before the lock is released.
        CharacterDatabase.DirectExecute(
            "UPDATE account_tcg_codes "
            "SET redeemed = 1, account_id = {}, character_guid = {}, redeemed_date = NOW() "
            "WHERE code = '{}' AND redeemed = 0",
            player->GetSession()->GetAccountId(),
            player->GetGUID().GetCounter(),
            escapedCode);
    }

    DeliveryResult deliveryResult = TryDeliverItem(player, creature,
                                                   group->itemEntries, redeemKey,
                                                   group->factionMount, consumable,
                                                   group->displayName);

    if (deliveryResult == DELIVERY_BAGS)
    {
        creature->Whisper(
            "Code accepted! \"" + group->displayName + "\" has been added to your inventory. Enjoy!",
            LANG_UNIVERSAL, player);
    }
}

// ============================================================
//  Item-specific code redemption (Mode 3)
// ============================================================
static void HandleItemSpecificCodeRedemption(Player*            player,
                                              Creature*          creature,
                                              const std::string& rawCode,
                                              const std::string& expectedGroupKey,
                                              const std::string& itemDisplayName)
{
    std::string code = NormalizeCode(rawCode);

    // --- Format check
    if (!IsValidCodeFormat(code))
    {
        creature->Whisper(
            "That doesn't look like a valid code. "
            "Codes follow the format XXXX-XXXX-XXXX-XXXX. "
            "Please check your code and try again.",
            LANG_UNIVERSAL, player);
        return;
    }

    // --- Database lookup
    // Serialised against concurrent redemptions — see s_codeRedeemMutex.
    std::string escapedCode = code;
    CharacterDatabase.EscapeString(escapedCode);

    // Same locking discipline as HandleCodeRedemption: the lookup, validation
    // and burn are atomic; delivery happens after the lock is released.
    // REWARD_GROUPS is static const, so group stays valid outside the scope.
    RewardGroup const* group     = nullptr;
    uint32             redeemKey = 0;
    bool               consumable = false;

    {
        std::lock_guard<std::mutex> redeemGuard(s_codeRedeemMutex);

        QueryResult codeResult = CharacterDatabase.Query(
            "SELECT reward_group, redeemed "
            "FROM account_tcg_codes "
            "WHERE code = '{}'",
            escapedCode);

        if (!codeResult)
        {
            creature->Whisper(
                "That code was not recognised. "
                "Please double-check the code and try again.",
                LANG_UNIVERSAL, player);
            return;
        }

        Field*      fields      = codeResult->Fetch();
        std::string rewardGroup = fields[0].Get<std::string>();
        bool        redeemed    = fields[1].Get<bool>();

        // --- Already used?
        if (redeemed)
        {
            creature->Whisper(
                "That code has already been redeemed. Each code may only be used once.",
                LANG_UNIVERSAL, player);
            return;
        }

        // --- Code must match the item the player selected
        if (rewardGroup != expectedGroupKey)
        {
            creature->Whisper(
                "That code is not valid for \"" + itemDisplayName + "\". "
                "Please check that you are entering the correct code for this item.",
                LANG_UNIVERSAL, player);
            return;
        }

        // --- Reward group lookup (should always succeed here)
        auto groupIt = REWARD_GROUPS.find(rewardGroup);
        if (groupIt == REWARD_GROUPS.end())
        {
            creature->Whisper(
                "Your code references an unknown reward. "
                "Please contact a Game Master for assistance.",
                LANG_UNIVERSAL, player);
            LOG_ERROR("module",
                "mod-tcg-vendors: Code '{}' references unknown reward_group '{}'. "
                "Check the account_tcg_codes table.",
                code, rewardGroup);
            return;
        }

        // See HandleCodeRedemption: an empty entry list is a data mistake, not
        // a runtime condition, and must not become undefined behaviour here.
        if (groupIt->second.itemEntries.empty())
        {
            creature->Whisper(
                "That reward is misconfigured and cannot be delivered. "
                "Please contact a Game Master.",
                LANG_UNIVERSAL, player);
            LOG_ERROR("module",
                "mod-tcg-vendors: REWARD_GROUPS['{}'] has no item entries. "
                "Add at least one before issuing codes for it.", rewardGroup);
            return;
        }

        group      = &groupIt->second;
        redeemKey  = group->itemEntries.front();
        consumable = IsItemConsumable(redeemKey, group->isConsumable);

        // --- Burn the code.  See HandleCodeRedemption for the full rationale on
        // "AND redeemed = 0" and on DirectExecute rather than Execute.
        CharacterDatabase.DirectExecute(
            "UPDATE account_tcg_codes "
            "SET redeemed = 1, account_id = {}, character_guid = {}, redeemed_date = NOW() "
            "WHERE code = '{}' AND redeemed = 0",
            player->GetSession()->GetAccountId(),
            player->GetGUID().GetCounter(),
            escapedCode);
    }

    DeliveryResult deliveryResult = TryDeliverItem(player, creature,
                                                   group->itemEntries, redeemKey,
                                                   group->factionMount, consumable,
                                                   group->displayName);

    if (deliveryResult == DELIVERY_BAGS)
    {
        creature->Whisper(
            "Code accepted! \"" + group->displayName + "\" has been added to your inventory. Enjoy!",
            LANG_UNIVERSAL, player);
    }
}

// ============================================================
//  B R O W S E   M E N U   H E L P E R S
// ============================================================

// Build a gossip item label, appending "[Already Redeemed]" or
// "[Unlimited]" hints as appropriate.
//
// `redeemed` is passed in rather than looked up here: every caller already has
// to compute it to decide which dialog to open, and HasRedeemed() is a blocking
// round-trip on the synchronous connection pool.  Querying twice per menu entry
// doubled the cost of opening a browse menu for no benefit.
static std::string BuildItemLabel(const std::string& name,
                                  bool               consumable,
                                  bool               redeemed)
{
    if (consumable)
        return name + " [Unlimited]";

    if (redeemed)
        return name + " [Already Redeemed]";

    return name;
}

// Populate and send the expansion set list for Landro's browse menu.
static void ShowExpansionList(Player* player, Creature* creature, uint32 npcTextId)
{
    ClearGossipMenuFor(player);
    for (auto const& [senderVal, name] : EXPANSION_NAMES)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name, SENDER_MAIN, senderVal);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_MAIN, 0);
    SendGossipMenuFor(player, npcTextId, creature->GetGUID());
}

// Populate and send the item list for one expansion set.
// When the browsing player has GM mode active, every item is presented as a
// player-name text input (for GM delivery) rather than a confirmation dialog.
static void ShowExpansionItems(Player* player, Creature* creature,
                               uint32 sender, uint32 npcTextId)
{
    auto catalogIt = LANDRO_CATALOG.find(sender);
    if (catalogIt == LANDRO_CATALOG.end())
    {
        ShowExpansionList(player, creature, npcTextId);
        return;
    }

    bool isGM = player->IsGameMaster();
    int  mode = GetVendorMode();
    ClearGossipMenuFor(player);
    const std::vector<TCGItem>& items = catalogIt->second;
    uint32 playerGuid = player->GetGUID().GetCounter();

    for (uint32 i = 0; i < static_cast<uint32>(items.size()); ++i)
    {
        uint32 redeemKey  = items[i].entries[0];
        bool   consumable = IsItemConsumable(redeemKey, items[i].isConsumable);

        if (isGM)
        {
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] " + items[i].displayName,
                sender, i + 1,
                "Enter character name to deliver \"" + items[i].displayName + "\" to:",
                0, true);
        }
        else
        {
            bool        redeemed = !consumable && HasRedeemed(playerGuid, redeemKey);
            std::string label    = BuildItemLabel(items[i].displayName,
                                                  consumable, redeemed);
            if (redeemed)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, sender, i + 1);
            }
            else if (mode == MODE_BLIZZLIKE || mode == MODE_ITEM_CODE)
            {
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                    sender, i + 1,
                    "Enter your redemption code for \"" + items[i].displayName + "\":",
                    0, true);
            }
            else
            {
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                    sender, i + 1,
                    "Receive \"" + items[i].displayName + "\"?",
                    0, false);
            }
        }
    }

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back to set list", SENDER_MAIN, 0);
    SendGossipMenuFor(player, npcTextId, creature->GetGUID());
}

// Handle a browse-menu item selection (used by both NPC classes).
// Returns true if handled.
static bool HandleBrowseSelect(Player* player, Creature* creature,
                               uint32 action,
                               const std::vector<TCGItem>& items)
{
    uint32 idx = action - 1;
    if (idx >= static_cast<uint32>(items.size()))
    {
        CloseGossipMenuFor(player);
        return true;
    }

    const TCGItem& item      = items[idx];
    uint32         redeemKey = item.entries[0];
    bool           consumable = IsItemConsumable(redeemKey, item.isConsumable);

    DeliveryResult result = TryDeliverItem(player, creature,
                                           item.entries, redeemKey,
                                           item.factionMount, consumable,
                                           item.displayName);

    if (result == DELIVERY_BAGS)
        creature->Whisper("Item granted. Enjoy!", LANG_UNIVERSAL, player);
    return true;
}

// ============================================================
//  GM delivery helpers
// ============================================================

// Returns true on successful delivery (or whisper sent).
// Returns false when the item has already been redeemed and forceOverride is false —
// the caller should then present the SENDER_GM_FORCE override confirmation dialog.
static bool HandleGMDelivery(Player*                    gm,
                             Creature*                  creature,
                             const std::string&         targetName,
                             const std::vector<uint32>& allEntries,
                             bool                       factionMount,
                             bool                       consumable,
                             const std::string&         displayName,
                             bool                       forceOverride)
{
    if (targetName.empty())
    {
        creature->Whisper("Please enter a character name.", LANG_UNIVERSAL, gm);
        return true;
    }

    // toGive is built from allEntries below and indexed at [0].  Every catalog
    // entry has at least one item today, but an empty list would make that index
    // undefined behaviour inside a GM-triggered path, so it is rejected up front.
    if (allEntries.empty())
    {
        creature->Whisper(
            "[GM] This item has no item entries configured and cannot be delivered.",
            LANG_UNIVERSAL, gm);
        LOG_ERROR("module",
            "mod-tcg-vendors: HandleGMDelivery called with an empty item entry list "
            "for \"{}\".", displayName);
        return true;
    }

    Player* targetPlayer = ObjectAccessor::FindPlayerByName(targetName);

    ObjectGuid::LowType targetGuid;
    uint8               targetRace;

    if (targetPlayer)
    {
        targetGuid = targetPlayer->GetGUID().GetCounter();
        targetRace = targetPlayer->getRace();
    }
    else
    {
        std::string escapedName = targetName;
        CharacterDatabase.EscapeString(escapedName);

        QueryResult charResult = CharacterDatabase.Query(
            "SELECT guid, race FROM characters WHERE name = '{}'", escapedName);

        if (!charResult)
        {
            creature->Whisper(
                "Character \"" + targetName + "\" was not found. "
                "Check the spelling and try again.",
                LANG_UNIVERSAL, gm);
            return true;
        }

        Field* charFields = charResult->Fetch();
        targetGuid = charFields[0].Get<uint32>();
        targetRace = charFields[1].Get<uint8>();
    }

    // --- Resolve faction-aware items using the target's race, not the GM's ---
    std::vector<uint32> toGive;
    if (factionMount && allEntries.size() >= 2)
    {
        TeamId team = Player::TeamIdForRace(targetRace);
        toGive = { allEntries[team == TEAM_HORDE ? 0 : 1] };
    }
    else
    {
        toGive = allEntries;
    }

    // --- Already-redeemed guard ---
    // If forceOverride is false, return false so the caller can show the
    // SENDER_GM_FORCE confirmation dialog.  If forceOverride is true we
    // skip this check entirely and re-deliver regardless.
    if (!forceOverride && !consumable && HasRedeemed(targetGuid, toGive[0]))
        return false;

    // --- Mail all items directly — no bag-space check ---
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bool allCreated = true;

    for (uint32 entry : toGive)
    {
        Item* item = Item::CreateItem(entry, GetPromoStackSize(entry), targetPlayer);
        if (!item)
        {
            LOG_ERROR("module",
                "mod-tcg-vendors: GM delivery failed to create item {} "
                "for character {} (guid {}).",
                entry, targetName, targetGuid);
            allCreated = false;
            continue;
        }
        item->SaveToDB(trans);
        MailDraft(
            "Item Delivery: " + displayName,
            "A Game Master has delivered \"" + displayName + "\" to your character.\n"
            "Visit any mailbox to collect it.")
            .AddItem(item)
            .SendMailTo(trans,
                targetPlayer ? MailReceiver(targetPlayer, targetGuid)
                             : MailReceiver(targetGuid),
                MailSender(creature));
    }
    CharacterDatabase.CommitTransaction(trans);

    // --- Record the delivery for unique items (keyed on target, not GM) ---
    //
    // Only record when the mail really went out.  Recording a failed delivery
    // would clear the GM's ability to retry: the flag is already set, so the
    // next attempt would be refused without forceOverride.
    //
    // toGive[0] is used rather than the caller's redemptionKey so that a
    // faction mount records the variant the target actually received.  For
    // TCG_SCOURGEWAR_MINIMOUNT that is entries[0] (49288, Horde) or
    // entries[1] (49289, Alliance); keying both factions on entries[0] would
    // let an Alliance mount be redeemed a second time through the player path.
    if (!consumable && allCreated)
        MarkRedeemed(targetGuid, toGive[0]);

    if (allCreated)
    {
        creature->Whisper(
            "[GM] \"" + displayName + "\" has been mailed to \"" + targetName + "\".",
            LANG_UNIVERSAL, gm);
    }
    else
    {
        creature->Whisper(
            "[GM] FAILED to create \"" + displayName + "\" for \"" + targetName +
            "\". Nothing was mailed and NO redemption was recorded — "
            "check the log for the offending item entry.",
            LANG_UNIVERSAL, gm);
    }
    return true;
}

static void HandleGMClearFlags(Player*            gm,
                               Creature*          creature,
                               const std::string& targetName)
{
    if (targetName.empty())
    {
        creature->Whisper("Please enter a character name.", LANG_UNIVERSAL, gm);
        return;
    }

    std::string escapedName = targetName;
    CharacterDatabase.EscapeString(escapedName);

    QueryResult charResult = CharacterDatabase.Query(
        "SELECT guid FROM characters WHERE name = '{}'", escapedName);

    if (!charResult)
    {
        creature->Whisper(
            "Character \"" + targetName + "\" was not found. "
            "Check the spelling and try again.",
            LANG_UNIVERSAL, gm);
        return;
    }

    ObjectGuid::LowType targetGuid = charResult->Fetch()[0].Get<uint32>();

    // Count existing records so the whisper is informative.
    // Get<uint64> because COUNT(*) is BIGINT, not INT.
    QueryResult countResult = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM character_tcg_redeemed WHERE guid = {}", targetGuid);

    uint64 count = countResult ? countResult->Fetch()[0].Get<uint64>() : 0;

    // DirectExecute so the flags are actually gone by the time the GM is told
    // they are cleared — otherwise an immediate re-redeem could still be
    // blocked by a row that is still queued on the asynchronous pool.
    CharacterDatabase.DirectExecute(
        "DELETE FROM character_tcg_redeemed WHERE guid = {}", targetGuid);

    if (count == 0)
    {
        creature->Whisper(
            "[GM] \"" + targetName + "\" had no TCG redemption records to clear.",
            LANG_UNIVERSAL, gm);
    }
    else
    {
        creature->Whisper(
            "[GM] Cleared " + std::to_string(count) +
            " redemption record(s) for \"" + targetName + "\". "
            "They may now re-receive those items.",
            LANG_UNIVERSAL, gm);
    }
}

// ============================================================
//  GM Send-Code helpers
//
//  "Send a code to a player" generates a fresh redemption code for a
//  chosen item, inserts it into account_tcg_codes as unredeemed, and
//  mails the target a readable stationery item with WoW-flavoured text
//  containing the code and vendor directions.
//
//  The item is NOT delivered directly — the player redeems it at the
//  appropriate vendor NPC using the mailed code.
// ============================================================

static std::string BuildGMCodeText(const std::string& targetName,
                                    const std::string& itemName,
                                    const std::string& code,
                                    uint32             itemId)
{
    std::string vendor = GetVendorForItem(itemId);
    return "Greetings, " + targetName + "!\n\n"
           "A Game Master has arranged a special gift for you!\n\n"
           "You have been awarded a redemption code for:\n"
           + itemName + "\n\n"
           "Your code:\n"
           + code + "\n\n"
           "To claim your reward, visit " + vendor + " and enter this code "
           "when prompted.\n\n"
           "This code is single-use and will be bound to your account upon "
           "redemption.  Keep it safe!\n\n"
           "Good luck on your adventures in Azeroth!";
}

static void HandleGMSendCode(Player*            gm,
                              Creature*          creature,
                              const std::string& targetName,
                              const std::string& displayName,
                              const std::string& rewardGroupKey,
                              uint32             itemId)
{
    if (targetName.empty())
    {
        creature->Whisper("Please enter a character name.", LANG_UNIVERSAL, gm);
        return;
    }

    if (REWARD_GROUPS.find(rewardGroupKey) == REWARD_GROUPS.end())
    {
        creature->Whisper(
            "[GM] Unknown reward group for this item — cannot generate a code.",
            LANG_UNIVERSAL, gm);
        LOG_ERROR("module",
            "mod-tcg-vendors: HandleGMSendCode: unknown reward group '{}'.",
            rewardGroupKey);
        return;
    }

    Player* targetPlayer = ObjectAccessor::FindPlayerByName(targetName);
    ObjectGuid::LowType targetGuid;

    if (targetPlayer)
    {
        targetGuid = targetPlayer->GetGUID().GetCounter();
    }
    else
    {
        std::string escapedName = targetName;
        CharacterDatabase.EscapeString(escapedName);
        QueryResult charResult = CharacterDatabase.Query(
            "SELECT guid FROM characters WHERE name = '{}'", escapedName);
        if (!charResult)
        {
            creature->Whisper(
                "Character \"" + targetName + "\" was not found. "
                "Check the spelling and try again.",
                LANG_UNIVERSAL, gm);
            return;
        }
        targetGuid = charResult->Fetch()[0].Get<uint32>();
    }

    std::string code   = GenerateRandomCode();
    InsertCodeToDatabase(code, rewardGroupKey);

    std::string text   = BuildGMCodeText(targetName, displayName, code, itemId);
    Item*       scroll = CreateStationeryWithText(targetPlayer, text);
    if (!scroll)
    {
        creature->Whisper("[GM] Failed to create stationery item.", LANG_UNIVERSAL, gm);
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    scroll->SaveToDB(trans);
    MailDraft("A special reward awaits you!", "")
        .AddItem(scroll)
        .SendMailTo(trans,
            targetPlayer ? MailReceiver(targetPlayer, targetGuid)
                         : MailReceiver(targetGuid),
            MailSender(creature));
    CharacterDatabase.CommitTransaction(trans);

    creature->Whisper(
        "[GM] A code for \"" + displayName + "\" has been mailed to \"" +
        targetName + "\".",
        LANG_UNIVERSAL, gm);
}

static void ShowExpansionListForCode(Player* player, Creature* creature, uint32 npcTextId)
{
    ClearGossipMenuFor(player);
    for (auto const& [senderVal, name] : EXPANSION_NAMES)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name, SENDER_GM_SEND_CODE, senderVal);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_MAIN, 0);
    SendGossipMenuFor(player, npcTextId, creature->GetGUID());
}

static void ShowExpansionItemsForCode(Player* player, Creature* creature,
                                      uint32 sender, uint32 npcTextId)
{
    auto catalogIt = LANDRO_CATALOG.find(sender);
    if (catalogIt == LANDRO_CATALOG.end())
    {
        ShowExpansionListForCode(player, creature, npcTextId);
        return;
    }
    ClearGossipMenuFor(player);
    const std::vector<TCGItem>& items = catalogIt->second;
    for (uint32 i = 0; i < static_cast<uint32>(items.size()); ++i)
    {
        uint32 encoded = (sender << 8) | (i + 1);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
            "[GM] Send code: " + items[i].displayName,
            SENDER_GM_SEND_CODE, encoded,
            "Enter character name to send a code for \"" +
            items[i].displayName + "\" to:",
            0, true);
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back to set list",
        SENDER_GM_SEND_CODE, 0);
    SendGossipMenuFor(player, npcTextId, creature->GetGUID());
}

static void ShowPromoCategoryListForCode(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    for (auto const& [senderVal, name] : PROMO_CATEGORY_NAMES)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name, SENDER_GM_SEND_CODE, senderVal);
    // The Back entry uses SENDER_GM_SEND_CODE, not SENDER_MAIN: the promo vendor
    // routes SENDER_MAIN / action 0 to the *delivery* tree, so a SENDER_MAIN
    // Back button here dropped a GM into the wrong submenu with no way out.
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_GM_SEND_CODE, 0);
    SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
}

static void ShowPromoItemsForCode(Player* player, Creature* creature, uint32 sender)
{
    auto catalogIt = PROMO_CATALOG.find(sender);
    if (catalogIt == PROMO_CATALOG.end())
    {
        ShowPromoCategoryListForCode(player, creature);
        return;
    }
    ClearGossipMenuFor(player);
    const std::vector<TCGItem>& items = catalogIt->second;
    for (uint32 i = 0; i < static_cast<uint32>(items.size()); ++i)
    {
        uint32 encoded = (sender << 8) | (i + 1);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
            "[GM] Send code: " + items[i].displayName,
            SENDER_GM_SEND_CODE, encoded,
            "Enter character name to send a code for \"" +
            items[i].displayName + "\" to:",
            0, true);
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back",
        SENDER_GM_SEND_CODE, 0);
    SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
}

// ============================================================
//  n p c _ l a n d r o _ l o n g s h o t
// ============================================================
class npc_landro_longshot : public CreatureScript
{
public:
    npc_landro_longshot() : CreatureScript("npc_landro_longshot") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        if (player->IsGameMaster())
        {
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Browse and deliver TCG items...",
                SENDER_MAIN, ACTION_OPEN_BROWSE);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Clear character redemption flags",
                SENDER_GM_CLEAR, 0,
                "Enter character name to clear all TCG redemption flags:",
                0, true);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Send a code to a player...",
                SENDER_MAIN, ACTION_OPEN_SEND_CODE);
            SendGossipMenuFor(player, NPC_TEXT_LANDRO, creature->GetGUID());
            return true;
        }

        int mode = GetVendorMode();

        // Mode 0: disabled — return false and let the DB gossip handle it
        if (mode == MODE_DISABLED)
            return false;

        if (mode == MODE_FREE || mode == MODE_ITEM_CODE)
        {
            // Mode 1: free browse — items awarded on confirmation click.
            // Mode 3: browse first, then enter a code per item — text
            //         input box appears when the player clicks an item.
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Browse TCG items by expansion set...",
                SENDER_MAIN, ACTION_OPEN_BROWSE);
        }
        else  // Mode 2: Blizz-Like
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "I have a TCG redemption code.",
                SENDER_CODE_ENTRY, 0,
                "Please enter your redemption code:",
                0, true);
        }

        SendGossipMenuFor(player, NPC_TEXT_LANDRO, creature->GetGUID());
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature,
                            uint32 sender, uint32 action,
                            const char* code) override
    {
        std::string codeStr(code ? code : "");

        // GM: clear redemption flags for named character
        if (sender == SENDER_GM_CLEAR)
        {
            HandleGMClearFlags(player, creature, codeStr);
            OnGossipHello(player, creature);
            return true;
        }

        // GM: force re-delivery override confirmation.
        if (sender == SENDER_GM_FORCE)
        {
            uint32 origSender = (action >> 8) & 0xFF;
            uint32 origAction = action & 0xFF;
            auto catalogIt = LANDRO_CATALOG.find(origSender);
            if (catalogIt != LANDRO_CATALOG.end())
            {
                uint32 idx = origAction - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    uint32 redeemKey  = items[idx].entries[0];
                    bool   consumable = IsItemConsumable(redeemKey, items[idx].isConsumable);
                    HandleGMDelivery(player, creature, codeStr,
                                     items[idx].entries,
                                     items[idx].factionMount,
                                     consumable,
                                     items[idx].displayName,
                                     true /* forceOverride */);
                    ShowExpansionItems(player, creature, origSender, NPC_TEXT_LANDRO);
                    return true;
                }
            }
            CloseGossipMenuFor(player);
            return false;
        }

        // GM send-code path: generate a code and mail stationery to target.
        if (sender == SENDER_GM_SEND_CODE)
        {
            uint32 origSender = (action >> 8) & 0xFF;
            uint32 origAction = action & 0xFF;
            auto catalogIt = LANDRO_CATALOG.find(origSender);
            if (catalogIt != LANDRO_CATALOG.end())
            {
                uint32 idx = origAction - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    HandleGMSendCode(player, creature, codeStr,
                        items[idx].displayName,
                        items[idx].rewardGroupKey,
                        items[idx].entries[0]);
                    ShowExpansionItemsForCode(player, creature, origSender, NPC_TEXT_LANDRO);
                    return true;
                }
            }
            CloseGossipMenuFor(player);
            return false;
        }

        // GM delivery: player entered a character name for an item in the
        // expansion browser.
        if (player->IsGameMaster())
        {
            auto catalogIt = LANDRO_CATALOG.find(sender);
            if (catalogIt != LANDRO_CATALOG.end())
            {
                uint32 idx = action - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    uint32 redeemKey  = items[idx].entries[0];
                    bool   consumable = IsItemConsumable(redeemKey, items[idx].isConsumable);
                    bool delivered = HandleGMDelivery(player, creature, codeStr,
                                                     items[idx].entries,
                                                     items[idx].factionMount,
                                                     consumable,
                                                     items[idx].displayName,
                                                     false /* forceOverride */);
                    if (!delivered)
                    {
                        // Target has already received this item — ask for override.
                        ClearGossipMenuFor(player);
                        uint32 forceAction = (sender << 8) | action;
                        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                            "[GM] Force delivery to \"" + codeStr + "\" anyway",
                            SENDER_GM_FORCE, forceAction,
                            "\"" + codeStr + "\" already has \"" + items[idx].displayName +
                            "\". Enter their name again to confirm forced re-delivery:",
                            0, true);
                        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                            "< Cancel", SENDER_MAIN, 0);
                        SendGossipMenuFor(player, NPC_TEXT_LANDRO, creature->GetGUID());
                    }
                    else
                    {
                        ShowExpansionItems(player, creature, sender, NPC_TEXT_LANDRO);
                    }
                    return true;
                }
            }
            return false;
        }

        // Mode 2 (Blizzlike) item-level code entry — any valid unused code is accepted;
        // the reward is determined by the code's reward_group, not by which item was clicked.
        if (GetVendorMode() == MODE_BLIZZLIKE)
        {
            HandleCodeRedemption(player, creature, codeStr);
            CloseGossipMenuFor(player);
            return true;
        }

        // Mode 3: item-specific code entry — code must match the selected item.
        if (GetVendorMode() == MODE_ITEM_CODE)
        {
            auto catalogIt = LANDRO_CATALOG.find(sender);
            if (catalogIt != LANDRO_CATALOG.end())
            {
                uint32 idx = action - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    HandleItemSpecificCodeRedemption(
                        player, creature, codeStr,
                        items[idx].rewardGroupKey,
                        items[idx].displayName);
                    ShowExpansionItems(player, creature, sender, NPC_TEXT_LANDRO);
                    return true;
                }
            }
        }

        return false;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 sender, uint32 action) override
    {
        if (GetVendorMode() == MODE_DISABLED)
            return false;

        // ---------------------------------------------------------------
        // WoW 3.3.5a gossip protocol note:
        //
        // For hasTextBox = true gossip items (Modes 2/3 code-entry items),
        // OnGossipSelect is NEVER called.  Only OnGossipSelectCode fires
        // when the player submits the text entry.  The confirmation popup
        // and text box are handled entirely client-side.
        //
        // OnGossipSelect IS called for hasTextBox = false items:
        //   - Mode 1 items (free delivery confirmation)
        //   - "[Already Redeemed]" items (added without text box in all modes)
        //
        // The server MUST respond to these clicks or the gossip window freezes.
        // ---------------------------------------------------------------

        ClearGossipMenuFor(player);

        if (sender == SENDER_MAIN)
        {
            if (action == ACTION_OPEN_BROWSE)
            {
                ShowExpansionList(player, creature, NPC_TEXT_LANDRO);
                return true;
            }

            if (action == ACTION_OPEN_SEND_CODE)
            {
                ShowExpansionListForCode(player, creature, NPC_TEXT_LANDRO);
                return true;
            }

            if (action == 0)
            {
                OnGossipHello(player, creature);
                return true;
            }

            ShowExpansionItems(player, creature, action, NPC_TEXT_LANDRO);
            return true;
        }

        // Send-code browse tree navigation.
        if (sender == SENDER_GM_SEND_CODE)
        {
            if (action == 0)
                ShowExpansionListForCode(player, creature, NPC_TEXT_LANDRO);
            else
                ShowExpansionItemsForCode(player, creature, action, NPC_TEXT_LANDRO);
            return true;
        }

        if (action == 0)
        {
            ShowExpansionList(player, creature, NPC_TEXT_LANDRO);
            return true;
        }

        auto catalogIt = LANDRO_CATALOG.find(sender);
        if (catalogIt == LANDRO_CATALOG.end())
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (GetVendorMode() == MODE_FREE)
            HandleBrowseSelect(player, creature, action, catalogIt->second);

        ShowExpansionItems(player, creature, sender, NPC_TEXT_LANDRO);
        return true;
    }
};

// ============================================================
//  n p c _ b l i z z c o n _ v e n d o r
//  Handles both Ransin Donner (2943) and Zas'Tysh (7951).
// ============================================================
class npc_blizzcon_vendor : public CreatureScript
{
public:
    npc_blizzcon_vendor() : CreatureScript("npc_blizzcon_vendor") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        if (player->IsGameMaster())
        {
            for (uint32 i = 0; i < static_cast<uint32>(BLIZZCON_CATALOG.size()); ++i)
            {
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    "[GM] " + BLIZZCON_CATALOG[i].displayName,
                    GOSSIP_SENDER_MAIN, i + 1,
                    "Enter character name to deliver \"" +
                    BLIZZCON_CATALOG[i].displayName + "\" to:",
                    0, true);
            }
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Clear character redemption flags",
                SENDER_GM_CLEAR, 0,
                "Enter character name to clear all TCG redemption flags:",
                0, true);
            for (uint32 i = 0; i < static_cast<uint32>(BLIZZCON_CATALOG.size()); ++i)
            {
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                    "[GM] Send code: " + BLIZZCON_CATALOG[i].displayName,
                    SENDER_GM_SEND_CODE, i + 1,
                    "Enter character name to send a code for \"" +
                    BLIZZCON_CATALOG[i].displayName + "\" to:",
                    0, true);
            }
            SendGossipMenuFor(player, NPC_TEXT_BLIZZCON, creature->GetGUID());
            return true;
        }

        int mode = GetVendorMode();

        if (mode == MODE_DISABLED)
            return false;

        uint32 playerGuid = player->GetGUID().GetCounter();

        if (mode == MODE_FREE || mode == MODE_BLIZZLIKE || mode == MODE_ITEM_CODE)
        {
            for (uint32 i = 0; i < static_cast<uint32>(BLIZZCON_CATALOG.size()); ++i)
            {
                uint32 redeemKey  = BLIZZCON_CATALOG[i].entries[0];
                bool   consumable = IsItemConsumable(redeemKey, BLIZZCON_CATALOG[i].isConsumable);
                bool   redeemed   = !consumable && HasRedeemed(playerGuid, redeemKey);
                std::string label = BuildItemLabel(BLIZZCON_CATALOG[i].displayName,
                                                   consumable, redeemed);

                if (redeemed)
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, label,
                        GOSSIP_SENDER_MAIN, i + 1);
                else if (mode == MODE_BLIZZLIKE || mode == MODE_ITEM_CODE)
                    AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                        GOSSIP_SENDER_MAIN, i + 1,
                        "Enter your redemption code for \"" +
                        BLIZZCON_CATALOG[i].displayName + "\":",
                        0, true);
                else
                    AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                        GOSSIP_SENDER_MAIN, i + 1,
                        "Receive \"" + BLIZZCON_CATALOG[i].displayName + "\"?",
                        0, false);
            }
        }

        SendGossipMenuFor(player, NPC_TEXT_BLIZZCON, creature->GetGUID());
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature,
                            uint32 sender, uint32 action,
                            const char* code) override
    {
        std::string codeStr(code ? code : "");

        if (sender == SENDER_GM_CLEAR)
        {
            HandleGMClearFlags(player, creature, codeStr);
            OnGossipHello(player, creature);
            return true;
        }

        // GM: force re-delivery override confirmation.
        // For Blizzcon, action is just the 1-based item index (no expansion to encode).
        if (sender == SENDER_GM_FORCE)
        {
            uint32 idx = action - 1;
            if (idx < static_cast<uint32>(BLIZZCON_CATALOG.size()))
            {
                uint32 redeemKey  = BLIZZCON_CATALOG[idx].entries[0];
                bool   consumable = IsItemConsumable(redeemKey, BLIZZCON_CATALOG[idx].isConsumable);
                HandleGMDelivery(player, creature, codeStr,
                                 BLIZZCON_CATALOG[idx].entries,
                                 BLIZZCON_CATALOG[idx].factionMount,
                                 consumable,
                                 BLIZZCON_CATALOG[idx].displayName,
                                 true /* forceOverride */);
                OnGossipHello(player, creature);
                return true;
            }
            CloseGossipMenuFor(player);
            return false;
        }

        // GM send-code path for Blizzcon items.
        // action is 1-based item index directly (no expansion encoding needed).
        if (sender == SENDER_GM_SEND_CODE)
        {
            uint32 idx = action - 1;
            if (idx < static_cast<uint32>(BLIZZCON_CATALOG.size()))
            {
                HandleGMSendCode(player, creature, codeStr,
                    BLIZZCON_CATALOG[idx].displayName,
                    BLIZZCON_CATALOG[idx].rewardGroupKey,
                    BLIZZCON_CATALOG[idx].entries[0]);
                OnGossipHello(player, creature);
                return true;
            }
            CloseGossipMenuFor(player);
            return false;
        }

        if (player->IsGameMaster())
        {
            uint32 idx = action - 1;
            if (idx < static_cast<uint32>(BLIZZCON_CATALOG.size()))
            {
                uint32 redeemKey  = BLIZZCON_CATALOG[idx].entries[0];
                bool   consumable = IsItemConsumable(redeemKey, BLIZZCON_CATALOG[idx].isConsumable);
                bool delivered = HandleGMDelivery(player, creature, codeStr,
                                                  BLIZZCON_CATALOG[idx].entries,
                                                  BLIZZCON_CATALOG[idx].factionMount,
                                                  consumable,
                                                  BLIZZCON_CATALOG[idx].displayName,
                                                  false /* forceOverride */);
                if (!delivered)
                {
                    ClearGossipMenuFor(player);
                    AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                        "[GM] Force delivery to \"" + codeStr + "\" anyway",
                        SENDER_GM_FORCE, action,
                        "\"" + codeStr + "\" already has \"" + BLIZZCON_CATALOG[idx].displayName +
                        "\". Enter their name again to confirm forced re-delivery:",
                        0, true);
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                        "< Cancel", SENDER_MAIN, 0);
                    SendGossipMenuFor(player, NPC_TEXT_BLIZZCON, creature->GetGUID());
                }
                else
                {
                    OnGossipHello(player, creature);
                }
                return true;
            }
            return false;
        }

        if (GetVendorMode() == MODE_BLIZZLIKE)
        {
            HandleCodeRedemption(player, creature, codeStr);
            CloseGossipMenuFor(player);
            return true;
        }

        if (GetVendorMode() == MODE_ITEM_CODE)
        {
            uint32 idx = action - 1;
            if (idx < static_cast<uint32>(BLIZZCON_CATALOG.size()))
            {
                HandleItemSpecificCodeRedemption(
                    player, creature, codeStr,
                    BLIZZCON_CATALOG[idx].rewardGroupKey,
                    BLIZZCON_CATALOG[idx].displayName);
                OnGossipHello(player, creature);
                return true;
            }
        }

        return false;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 /*sender*/, uint32 action) override
    {
        // Disabled vendor: hand nothing out.  See npc_promo_vendor for why this
        // guard is repeated in every click handler.
        if (GetVendorMode() == MODE_DISABLED)
            return false;

        // Mode 2 and 3 deliver through the code dialogs in OnGossipSelectCode;
        // only Mode 1 offers the free confirmation click that reaches here.
        if (GetVendorMode() != MODE_FREE)
        {
            OnGossipHello(player, creature);
            return true;
        }

        ClearGossipMenuFor(player);

        HandleBrowseSelect(player, creature, action, BLIZZCON_CATALOG);

        OnGossipHello(player, creature);
        return true;
    }
};

// ============================================================
//  Promo vendor browse helpers
// ============================================================

static void ShowPromoCategoryList(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    for (auto const& [senderVal, name] : PROMO_CATEGORY_NAMES)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name, SENDER_MAIN, senderVal);
    // Without this the only way out of the category list was closing the gossip
    // window.  OnGossipSelect maps SENDER_MAIN / action 0 back to OnGossipHello.
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_MAIN, 0);
    SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
}

// Build the item list for one promo category.
// isGM true  => every item shown as player-name text input (GM delivery).
//         requiredSpell items are always visible to GMs.
// isGM false => items filtered by requiredSpell; mode-appropriate dialog.
static void ShowPromoItems(Player* player, Creature* creature, uint32 sender)
{
    auto catalogIt = PROMO_CATALOG.find(sender);
    if (catalogIt == PROMO_CATALOG.end())
    {
        ShowPromoCategoryList(player, creature);
        return;
    }

    bool isGM = player->IsGameMaster();
    int  mode = GetVendorMode();
    ClearGossipMenuFor(player);
    const std::vector<TCGItem>& items = catalogIt->second;
    uint32 playerGuid = player->GetGUID().GetCounter();

    for (uint32 i = 0; i < static_cast<uint32>(items.size()); ++i)
    {
        uint32 redeemKey  = items[i].entries[0];
        // Route through IsItemConsumable so the Landro box override is honoured
        // here too — reading the raw flag made this menu disagree with the
        // other three vendor classes as soon as a box was added to a promo set.
        bool   consumable = IsItemConsumable(redeemKey, items[i].isConsumable);

        // Conditional items: skip for non-GMs who haven't learned the required spell.
        if (!isGM && items[i].requiredSpell != 0 && !player->HasSpell(items[i].requiredSpell))
            continue;

        if (isGM)
        {
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] " + items[i].displayName,
                sender, i + 1,
                "Enter character name to deliver \"" + items[i].displayName + "\" to:",
                0, true);
        }
        else
        {
            bool        redeemed = !consumable && HasRedeemed(playerGuid, redeemKey);
            std::string label    = BuildItemLabel(items[i].displayName,
                                                  consumable, redeemed);
            if (redeemed)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, sender, i + 1);
            }
            else if (items[i].requiredSpell != 0)
            {
                // Spell-gated items (War Fuel) are always free regardless of mode —
                // no code is ever generated for them; possession of the required
                // companion is sufficient authorisation.
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                    sender, i + 1,
                    "Receive \"" + items[i].displayName + "\"?",
                    0, false);
            }
            else if (mode == MODE_BLIZZLIKE || mode == MODE_ITEM_CODE)
            {
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                    sender, i + 1,
                    "Enter your redemption code for \"" + items[i].displayName + "\":",
                    0, true);
            }
            else
            {
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                    sender, i + 1,
                    "Receive \"" + items[i].displayName + "\"?",
                    0, false);
            }
        }
    }

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_MAIN, 0);
    SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
}

// ============================================================
//  n p c _ p r o m o _ v e n d o r
//  Handles Garel Redrock (Alliance, Ironforge) and
//  Tharl Stonebleeder (Horde, Orgrimmar).
// ============================================================
class npc_promo_vendor : public CreatureScript
{
public:
    npc_promo_vendor() : CreatureScript("npc_promo_vendor") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        if (player->IsGameMaster())
        {
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Browse and deliver promotional items...",
                SENDER_MAIN, ACTION_OPEN_BROWSE);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Send a code to a player...",
                SENDER_MAIN, ACTION_OPEN_SEND_CODE);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Clear character redemption flags",
                SENDER_GM_CLEAR, 0,
                "Enter character name to clear all TCG redemption flags:",
                0, true);
            SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
            return true;
        }

        int mode = GetVendorMode();

        if (mode == MODE_DISABLED)
            return false;

        if (mode == MODE_FREE || mode == MODE_ITEM_CODE)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Browse promotional items by category...",
                SENDER_MAIN, ACTION_OPEN_BROWSE);
        }
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "I have a Promotional redemption code.",
                SENDER_CODE_ENTRY, 0,
                "Please enter your redemption code:",
                0, true);
        }

        SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature,
                            uint32 sender, uint32 action,
                            const char* code) override
    {
        std::string codeStr(code ? code : "");

        if (sender == SENDER_GM_CLEAR)
        {
            HandleGMClearFlags(player, creature, codeStr);
            OnGossipHello(player, creature);
            return true;
        }

        if (sender == SENDER_GM_FORCE)
        {
            uint32 origSender = (action >> 8) & 0xFF;
            uint32 origAction = action & 0xFF;
            auto catalogIt = PROMO_CATALOG.find(origSender);
            if (catalogIt != PROMO_CATALOG.end())
            {
                uint32 idx = origAction - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    bool consumable = IsItemConsumable(items[idx].entries[0],
                                                       items[idx].isConsumable);
                    HandleGMDelivery(player, creature, codeStr,
                                     items[idx].entries,
                                     items[idx].factionMount,
                                     consumable,
                                     items[idx].displayName,
                                     true /* forceOverride */);
                    ShowPromoItems(player, creature, origSender);
                    return true;
                }
            }
            CloseGossipMenuFor(player);
            return false;
        }

        // GM send-code path for promo items.
        // action encodes (categorySender << 8) | itemIndex.
        if (sender == SENDER_GM_SEND_CODE)
        {
            uint32 origSender = (action >> 8) & 0xFF;
            uint32 origAction = action & 0xFF;
            auto catalogIt = PROMO_CATALOG.find(origSender);
            if (catalogIt != PROMO_CATALOG.end())
            {
                uint32 idx = origAction - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    HandleGMSendCode(player, creature, codeStr,
                        items[idx].displayName,
                        items[idx].rewardGroupKey,
                        items[idx].entries[0]);
                    ShowPromoItemsForCode(player, creature, origSender);
                    return true;
                }
            }
            CloseGossipMenuFor(player);
            return false;
        }

        if (player->IsGameMaster())
        {
            auto catalogIt = PROMO_CATALOG.find(sender);
            if (catalogIt != PROMO_CATALOG.end())
            {
                uint32 idx = action - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    bool consumable = IsItemConsumable(items[idx].entries[0],
                                                       items[idx].isConsumable);
                    bool delivered = HandleGMDelivery(player, creature, codeStr,
                                                     items[idx].entries,
                                                     items[idx].factionMount,
                                                     consumable,
                                                     items[idx].displayName,
                                                     false /* forceOverride */);
                    if (!delivered)
                    {
                        ClearGossipMenuFor(player);
                        uint32 forceAction = (sender << 8) | action;
                        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                            "[GM] Force delivery to \"" + codeStr + "\" anyway",
                            SENDER_GM_FORCE, forceAction,
                            "\"" + codeStr + "\" already has \"" + items[idx].displayName +
                            "\". Enter their name again to confirm forced re-delivery:",
                            0, true);
                        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                            "< Cancel", SENDER_MAIN, 0);
                        SendGossipMenuFor(player, NPC_TEXT_PROMO, creature->GetGUID());
                    }
                    else
                    {
                        ShowPromoItems(player, creature, sender);
                    }
                    return true;
                }
            }
            return false;
        }

        if (GetVendorMode() == MODE_BLIZZLIKE)
        {
            HandleCodeRedemption(player, creature, codeStr);
            CloseGossipMenuFor(player);
            return true;
        }

        if (GetVendorMode() == MODE_ITEM_CODE)
        {
            auto catalogIt = PROMO_CATALOG.find(sender);
            if (catalogIt != PROMO_CATALOG.end())
            {
                uint32 idx = action - 1;
                const std::vector<TCGItem>& items = catalogIt->second;
                if (idx < static_cast<uint32>(items.size()))
                {
                    HandleItemSpecificCodeRedemption(
                        player, creature, codeStr,
                        items[idx].rewardGroupKey,
                        items[idx].displayName);
                    ShowPromoItems(player, creature, sender);
                    return true;
                }
            }
        }

        return false;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 sender, uint32 action) override
    {
        // A disabled vendor must not hand out anything, even if a gossip menu
        // exists for these NPCs in the database.  OnGossipHello already returns
        // false in this mode; this keeps the click handlers consistent with it
        // and with npc_landro_longshot, rather than relying on the absence of
        // gossip_menu rows.
        if (GetVendorMode() == MODE_DISABLED)
            return false;

        ClearGossipMenuFor(player);

        if (sender == SENDER_MAIN)
        {
            if (action == ACTION_OPEN_SEND_CODE)
            {
                ShowPromoCategoryListForCode(player, creature);
                return true;
            }

            if (action == ACTION_OPEN_BROWSE)
            {
                ShowPromoCategoryList(player, creature);
                return true;
            }

            // action == 0 is the "< Back" of the promo category list.
            if (action == 0)
            {
                OnGossipHello(player, creature);
                return true;
            }

            ShowPromoItems(player, creature, action);
            return true;
        }

        // Send-code browse tree navigation.
        if (sender == SENDER_GM_SEND_CODE)
        {
            if (action == 0)
                ShowPromoCategoryListForCode(player, creature);
            else if (PROMO_CATALOG.find(action) != PROMO_CATALOG.end())
                ShowPromoItemsForCode(player, creature, action);
            else
                ShowPromoCategoryListForCode(player, creature);
            return true;
        }

        // "< Back" from an item list returns to the category list.
        if (action == 0)
        {
            ShowPromoCategoryList(player, creature);
            return true;
        }

        auto catalogIt = PROMO_CATALOG.find(sender);
        if (catalogIt == PROMO_CATALOG.end())
        {
            CloseGossipMenuFor(player);
            return true;
        }

        uint32 idx = action - 1;
        const std::vector<TCGItem>& items = catalogIt->second;
        if (idx >= static_cast<uint32>(items.size()))
        {
            CloseGossipMenuFor(player);
            return true;
        }

        const TCGItem& item = items[idx];

        if (item.requiredSpell != 0 && !player->HasSpell(item.requiredSpell))
        {
            creature->Whisper(
                "You must have the Warbot companion learned before you can "
                "collect fuel for it.",
                LANG_UNIVERSAL, player);
            ShowPromoItems(player, creature, sender);
            return true;
        }

        DeliveryResult result = TryDeliverItem(player, creature,
                                               item.entries, item.entries[0],
                                               item.factionMount,
                                               IsItemConsumable(item.entries[0], item.isConsumable),
                                               item.displayName);
        if (result == DELIVERY_BAGS)
            creature->Whisper("Item granted. Enjoy!", LANG_UNIVERSAL, player);

        ShowPromoItems(player, creature, sender);
        return true;
    }
};

// ============================================================
//  BOSS DROP CONFIG HELPERS
//
//  The BossDrop id lists are parsed once, in OnStartup, and cached.  They used
//  to be re-read from sConfigMgr and re-tokenised on every boss kill and every
//  stationery looted.
// ============================================================

// Parses a comma-separated uint32 list.
//
// A previous version used std::stoul, which throws std::invalid_argument on any
// non-numeric token and std::out_of_range on a huge one.  These lists are read
// inside OnPlayerCreatureKill and OnPlayerLootItem, so a typo such as a trailing
// space — the exact shape the shipped .dist comment invites an operator to
// paste — would abort the worldserver mid-raid.  strtoul plus an explicit
// endptr/errno check turns that into a logged warning and a skipped token.
static std::vector<uint32> ParseUint32List(const std::string& str,
                                          std::string        const& optionName)
{
    std::vector<uint32> ids;

    size_t start = 0;
    while (start <= str.size())
    {
        size_t end = str.find(',', start);
        std::string token = (end == std::string::npos)
            ? str.substr(start)
            : str.substr(start, end - start);

        // Trim surrounding whitespace so "1, 2, 3" and "1,2,3" behave alike.
        size_t first = token.find_first_not_of(" \t\r\n");
        size_t last  = token.find_last_not_of(" \t\r\n");

        // An all-whitespace token comes from a trailing or doubled comma and is
        // not worth reporting.
        if (first != std::string::npos)
        {
            token = token.substr(first, last - first + 1);

            errno = 0;
            char* endPtr = nullptr;

            // Reject a sign explicitly.  strtoul("-1") returns ULONG_MAX, which
            // on Windows (32-bit unsigned long) is exactly uint32's maximum and
            // would slip past the range check below — giving platform-dependent
            // behaviour for the same config string.
            bool negative = token[0] == '-';

            unsigned long value = negative ? 0 : std::strtoul(token.c_str(), &endPtr, 10);

            if (negative || errno == ERANGE ||
                value > std::numeric_limits<uint32>::max() ||
                endPtr == token.c_str() || *endPtr != '\0')
            {
                LOG_ERROR("module",
                    "mod-tcg-vendors: {} contains an invalid entry \"{}\" — ignored. "
                    "Expected a comma-separated list of unsigned integers.",
                    optionName, token);
            }
            else
            {
                ids.push_back(static_cast<uint32>(value));
            }
        }

        if (end == std::string::npos)
            break;
        start = end + 1;
    }

    return ids;
}

static bool GetBossDropEnabled()
{
    return sConfigMgr->GetOption<bool>("TCGVendors.BossDrop.Enabled", false);
}

// Cached BossDrop configuration, filled by OnStartup.
// Before startup runs these are empty, which is harmless: every consumer
// is itself gated on GetBossDropEnabled().
static std::vector<uint32> s_bossDropCreatureIds;
static std::vector<uint32> s_bossDropItemIds;

static std::vector<uint32> const& GetBossDropCreatureIds()
{
    return s_bossDropCreatureIds;
}

static std::vector<uint32> const& GetBossDropItemIds()
{
    return s_bossDropItemIds;
}

// Refresh the cached BossDrop ids from the config file.
static void ReloadBossDropConfig()
{
    s_bossDropCreatureIds = ParseUint32List(
        sConfigMgr->GetOption<std::string>("TCGVendors.BossDrop.CreatureIds", ""),
        "TCGVendors.BossDrop.CreatureIds");

    s_bossDropItemIds = ParseUint32List(
        sConfigMgr->GetOption<std::string>("TCGVendors.BossDrop.ItemIds", ""),
        "TCGVendors.BossDrop.ItemIds");
}

// TCGVendors.BossDrop.MailParticipants
//   0 — disabled (loot window only)
//   1 — mail only (no loot template rows; stationery mailed to one randomly
//       chosen participant)
//   2 — mail AND loot (stationery on corpse + mail to one randomly chosen
//       participant)
//
// The option name is historical: values 1 and 2 used to mail every member.
static int GetBossDropMailMode()
{
    int mode = sConfigMgr->GetOption<int>("TCGVendors.BossDrop.MailParticipants", 0);
    if (mode < 0 || mode > 2)
    {
        LOG_WARN("module",
            "mod-tcg-vendors: TCGVendors.BossDrop.MailParticipants has unrecognised value {} "
            "— falling back to 0 (disabled).", mode);
        return 0;
    }
    return mode;
}

// ============================================================
//  Boss Drop State
//
//  Keyed by creature entry ID.  Written in OnPlayerCreatureKill,
//  read in OnPlayerLootItem when each player picks up the stationery.
//
//  WHY THIS IS NECESSARY:
//  creature_loot_template stores item template entry IDs, not instances.
//  When a player loots, the engine calls Item::CreateItem() to produce a
//  brand-new blank instance — any pre-saved texted instance is orphaned.
//
//  The solution is to carry the pending text across the kill→loot boundary
//  in a C++ map, then inject it onto the fresh instance in OnPlayerLootItem
//  immediately after the loot system creates it.
// ============================================================
struct PendingBossDrop
{
    std::string rewardGroup;
    std::string bossName;
    std::string itemName;
    uint32      itemId      = 0;
    uint32      droppedAtMs = 0;   // getMSTime() when the kill registered
};

// THREAD SAFETY:
// AzerothCore runs one update thread per map *instance* (MapUpdater starts a
// pool of worker threads, each popping map update requests from a shared queue).
// A map is updated by exactly one thread at a time, but two raid instances — or
// a raid and a battleground — run on different threads simultaneously.  Both
// hooks below execute inside their map's update thread, so this process-wide
// map is touched concurrently and every access needs the lock.
//
// Entries are also expired: a boss that is killed but never looted would
// otherwise leave its metadata behind for the lifetime of the process.
static std::mutex                       s_pendingBossDropsMutex;
static std::map<uint32, PendingBossDrop> s_pendingBossDrops;

// How long a kill stays eligible for looting.  Corpses despawn after roughly
// two minutes, and a reward is only meaningful while the corpse is there.
static constexpr uint32 PENDING_BOSS_DROP_TTL_MS = 5 * 60 * 1000;

// Drop every stale entry.  Called with the lock held.
static void PrunePendingBossDrops(uint32 nowMs)
{
    for (auto it = s_pendingBossDrops.begin(); it != s_pendingBossDrops.end();)
    {
        if (nowMs >= it->second.droppedAtMs &&
            nowMs - it->second.droppedAtMs > PENDING_BOSS_DROP_TTL_MS)
            it = s_pendingBossDrops.erase(it);
        else
            ++it;
    }
}

// Record the loot metadata for a just-killed boss.
static void StorePendingBossDrop(uint32 creatureEntry, PendingBossDrop const& drop)
{
    uint32 nowMs = getMSTime();

    std::lock_guard<std::mutex> guard(s_pendingBossDropsMutex);
    PrunePendingBossDrops(nowMs);
    s_pendingBossDrops[creatureEntry] = drop;
}

// Fetch the loot metadata for a boss, without removing it: several players loot
// the same corpse and each needs the metadata.  Removal is left to the TTL
// sweep, since the corpse may still have looters who have not opened it yet.
static bool LoadPendingBossDrop(uint32 creatureEntry, PendingBossDrop* out)
{
    uint32 nowMs = getMSTime();

    std::lock_guard<std::mutex> guard(s_pendingBossDropsMutex);
    PrunePendingBossDrops(nowMs);

    auto it = s_pendingBossDrops.find(creatureEntry);
    if (it == s_pendingBossDrops.end())
        return false;

    *out = it->second;
    return true;
}

// ============================================================
//  tcg_boss_drop_script  (PlayerScript)
//  Uses OnPlayerCreatureKill to detect configured boss kills.
// ============================================================
class tcg_boss_drop_script : public PlayerScript
{
public:
    tcg_boss_drop_script() : PlayerScript("tcg_boss_drop_script") {}

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!GetBossDropEnabled() || !killer || !killed)
            return;

        uint32 creatureEntry = killed->GetEntry();
        std::vector<uint32> const& bossIds = GetBossDropCreatureIds();
        if (std::find(bossIds.begin(), bossIds.end(), creatureEntry) == bossIds.end())
            return;

        std::vector<uint32> const& itemIds = GetBossDropItemIds();
        if (itemIds.empty())
            return;

        uint32 itemId = itemIds[urand(0, static_cast<uint32>(itemIds.size()) - 1)];
        std::string rewardGroup = GetRewardGroupForItem(itemId);
        if (rewardGroup.empty())
        {
            LOG_ERROR("module",
                "mod-tcg-vendors: BossDrop item {} has no matching reward_group. "
                "Check TCGVendors.BossDrop.ItemIds and REWARD_GROUPS.", itemId);
            return;
        }

        std::string bossName = killed->GetName();
        std::string itemName = GetItemName(itemId);

        // Park metadata only — code generation happens in OnPlayerLootItem
        // so each looting player gets their own unique code from the corpse.
        // The mail-participants path generates its own codes independently.
        StorePendingBossDrop(creatureEntry,
            { rewardGroup, bossName, itemName, itemId, getMSTime() });

        // ---- Optional: mail a unique code to ONE randomly chosen participant ----
        // Fires for MailParticipants = 1 (mail only) or 2 (mail + loot).
        if (GetBossDropMailMode() >= 1)
        {
            // Candidates: everyone in the killer's group/raid who is on this
            // map instance.  A group never spans instances in WoW, so the map
            // check only guards against stale references during a teleport.
            std::vector<Player*> participants;
            if (Group* group = killer->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member->GetMap() == killed->GetMap())
                        participants.push_back(member);
                }
            }

            // The killer is always eligible: this covers solo kills and the
            // degenerate case where every group member resolved elsewhere.
            if (participants.empty())
                participants.push_back(killer);

            // Exactly one recipient per kill.  Mailing every member made the
            // reward worthless in a full raid — the whole group received the
            // same item — so it is now a single uniform draw.  Being the killer
            // does not disqualify anyone.
            Player* recipient =
                participants[urand(0, static_cast<uint32>(participants.size()) - 1)];

            std::string rCode = GenerateRandomCode();
            InsertCodeToDatabase(rCode, rewardGroup);

            std::string rText = BuildStationeryText(bossName, itemName, rCode, itemId);
            Item* scroll = CreateStationeryWithText(recipient, rText);
            if (!scroll)
            {
                LOG_ERROR("module",
                    "mod-tcg-vendors: BossDrop failed to create stationery for the "
                    "selected recipient (guid {}). Code '{}' was inserted but is "
                    "now unreachable.", recipient->GetGUID().GetCounter(), rCode);
            }
            else
            {
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                scroll->SaveToDB(trans);
                MailDraft("A reward for your valor!", "")
                    .AddItem(scroll)
                    .SendMailTo(trans,
                        MailReceiver(recipient, recipient->GetGUID().GetCounter()),
                        MailSender(killed));
                CharacterDatabase.CommitTransaction(trans);

                LOG_INFO("module",
                    "mod-tcg-vendors: BossDrop selected recipient {} (guid {}) out of "
                    "{} candidate(s); code for \"{}\" mailed.",
                    recipient->GetName(), recipient->GetGUID().GetCounter(),
                    participants.size(), itemName);
            }
        }
    }
};

// ============================================================
//  tcg_boss_drop_player_script  (PlayerScript)
//  Uses OnPlayerLootItem to inject the pending code text onto the
//  fresh stationery instance the loot system just created.
// ============================================================
class tcg_boss_drop_player_script : public PlayerScript
{
public:
    tcg_boss_drop_player_script() : PlayerScript("tcg_boss_drop_player_script") {}

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootGuid) override
    {
        if (!GetBossDropEnabled() || !player || !item)
            return;
 
        if (!lootGuid.IsCreature())
            return;
 
        Creature* source = ObjectAccessor::GetCreature(*player, lootGuid);
        if (!source)
            return;
 
        uint32 creatureEntry = source->GetEntry();
        std::vector<uint32> const& bossIds = GetBossDropCreatureIds();
        if (std::find(bossIds.begin(), bossIds.end(), creatureEntry) == bossIds.end())
            return;

        if (item->GetEntry() != ITEM_SIMPLE_STATIONERY)
            return;

        PendingBossDrop drop;
        if (!LoadPendingBossDrop(creatureEntry, &drop))
            return;

        // Generate a unique code for this specific looting player.
        std::string code = GenerateRandomCode();
        InsertCodeToDatabase(code, drop.rewardGroup);
        std::string text = BuildStationeryText(drop.bossName, drop.itemName, code, drop.itemId);

        // Stamp the looted item in-place.
        //
        // StampItemText does three things:
        //   1. item->SetText(text)       — sets m_text, which Item::SaveToDB
        //      persists into item_instance.text
        //   2. SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_READABLE) — tells the
        //      client this item has readable text; client sends CMSG_ITEM_TEXT_QUERY
        //   3. SetState(ITEM_CHANGED)    — queues the flag update to be sent to
        //      the client on the next update cycle
        //
        // HandleItemTextQuery answers the packet straight from the live Item, so
        // the text is readable the moment the player right-clicks — no
        // immediate re-save is required.
        StampItemText(item, player, text);
    }
};

// ============================================================
//  tcg_boss_drop_world_script  (WorldScript)
//
//  On server startup, ensures every boss entry configured in
//  TCGVendors.BossDrop.CreatureIds has a creature_loot_template
//  row guaranteeing a 100% drop of item 9311 (Simple Stationery).
//
//  If any new rows are written, the creature loot tables are
//  reloaded in-process — no manual .reload or restart required.
//  If the rows already exist from a previous run or from manually
//  applying the SQL, nothing is touched and no reload happens.
// ============================================================
class tcg_boss_drop_world_script : public WorldScript
{
public:
    tcg_boss_drop_world_script() : WorldScript("tcg_boss_drop_world_script") {}

    // Verify that every item entry the module can ever hand out really exists.
    //
    // Item::CreateItem() calls ABORT() — it takes the worldserver down — when
    // item_template has no matching row.  A single mistyped entry in a catalog
    // would therefore abort the server the first time a player redeemed it,
    // with no useful context.  Checking the catalogs once at startup turns that
    // into a clear log line naming the offending entries.
    //
    // Must run after the object manager has loaded item templates.
    static void ValidateCatalogItemEntries()
    {
        std::vector<uint32> broken;

        auto check = [&broken](uint32 entry, char const* where)
        {
            if (sObjectMgr->GetItemTemplate(entry))
                return;

            LOG_ERROR("module",
                "mod-tcg-vendors: item {} referenced by {} has no item_template row. "
                "Redeeming it would abort the server.", entry, where);
            broken.push_back(entry);
        };

        // NOTE: the "where" labels below are deliberately not shaped like a
        // reward-group key (no TCG_/PROMO_/BLIZZCON_/WWI_ prefix in upper case).
        // tools/generate_codes.py scans this file for group-key-shaped string
        // literals to verify that every catalog rewardGroupKey resolves, and a
        // label like "PROMO_CATALOG" would be reported as a dangling key.
        for (auto const& [groupKey, group] : REWARD_GROUPS)
            for (uint32 entry : group.itemEntries)
                check(entry, ("REWARD_GROUPS[" + groupKey + "]").c_str());

        auto checkCatalog = [&check](std::vector<TCGItem> const& items, char const* vendor)
        {
            for (TCGItem const& item : items)
                for (uint32 entry : item.entries)
                    check(entry, vendor);
        };

        for (auto const& [sender, items] : LANDRO_CATALOG)
            checkCatalog(items, "Landro catalog");
        for (auto const& [sender, items] : PROMO_CATALOG)
            checkCatalog(items, "promo catalog");
        checkCatalog(BLIZZCON_CATALOG, "BlizzCon catalog");
        checkCatalog(TYRAELS_CATALOG, "Worldwide Invitational catalog");

        if (!broken.empty())
            LOG_ERROR("module",
                "mod-tcg-vendors: {} catalog item entry/entries do not exist in "
                "item_template. Affected rewards are unusable until fixed: {}",
                broken.size(),
                [&broken]
                {
                    std::string list;
                    for (size_t i = 0; i < broken.size(); ++i)
                    {
                        if (i)
                            list += ", ";
                        list += std::to_string(broken[i]);
                    }
                    return list;
                }());
        else
            LOG_INFO("module",
                "mod-tcg-vendors: All catalog item entries validated against "
                "item_template.");
    }

    void OnStartup() override
    {
        ReloadBossDropConfig();
        ValidateCatalogItemEntries();

        // Purge only the rows this module owns.
        //
        // A blanket "WHERE Item = 9311" would also delete Simple Stationery rows
        // belonging to some other system (an event module, a hand-edited loot
        // table) on every restart.  The rows we insert are identified by their
        // Comment marker, so scoping the delete to that marker removes exactly
        // our rows and nothing else.
        //
        // This runs unconditionally — including when the feature is disabled —
        // so that disabling the module, or emptying CreatureIds, still clears
        // rows left over from a previous configuration.
        WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
        trans->Append(
            "DELETE FROM creature_loot_template WHERE Item = {} AND Comment LIKE '{}'",
            ITEM_SIMPLE_STATIONERY, BOSS_DROP_LOOT_COMMENT_PREFIX);

        int mailMode = GetBossDropMailMode();
        std::vector<uint32> const& bossIds = GetBossDropCreatureIds();

        // MailParticipants = 1 (mail only): stationery is mailed directly to
        // participants and never appears on the corpse, so no loot rows are
        // wanted.  The same applies when the feature is off or CreatureIds is
        // empty — in all three cases the purge above stands on its own.
        bool insertLootRows = GetBossDropEnabled() && mailMode != 1 && !bossIds.empty();

        uint32 inserted = 0;

        if (insertLootRows)
        {
            // MailParticipants = 0 or 2: stationery appears on the corpse.
            // Insert one row per configured boss at 100% drop chance.
            for (uint32 bossEntry : bossIds)
            {
                trans->Append(
                    "INSERT INTO creature_loot_template "
                    "(Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount, Comment) "
                    "VALUES ({}, {}, 0, 100, 0, 1, 0, 1, 1, '{}')",
                    bossEntry, ITEM_SIMPLE_STATIONERY, BOSS_DROP_LOOT_COMMENT);
                LOG_INFO("module",
                    "mod-tcg-vendors: Registered stationery drop for boss entry {}.", bossEntry);
                ++inserted;
            }
        }

        // WHY DirectCommitTransaction AND NOT Execute:
        // DatabaseWorkerPool::Execute() is asynchronous and runs on the IDX_ASYNC
        // connection pool, while LoadLootTemplates_Creature() reads the table back
        // through Query(), which uses the IDX_SYNCH pool.  The two pools are
        // independent, so with Execute() the reload could observe the table
        // before the DELETE/INSERT had been applied — making the boss drop rows
        // take effect only intermittently.
        //
        // DirectCommitTransaction blocks, runs on the synchronous pool and wraps
        // the whole purge-and-insert in one transaction, so the reload below is
        // guaranteed to read the intended state and a failure cannot leave a
        // half-applied loot table.
        WorldDatabase.DirectCommitTransaction(trans);

        LoadLootTemplates_Creature();
        LootTemplates_Creature.CheckLootRefs();

        if (!GetBossDropEnabled())
            LOG_INFO("module",
                "mod-tcg-vendors: BossDrop disabled. "
                "Stationery loot rows owned by this module purged.");
        else if (mailMode == 1)
            LOG_INFO("module",
                "mod-tcg-vendors: BossDrop MailParticipants=1 (mail only). "
                "No loot template rows inserted.");
        else if (bossIds.empty())
            LOG_INFO("module",
                "mod-tcg-vendors: BossDrop enabled but CreatureIds is empty. "
                "Stationery loot rows owned by this module purged.");
        else
            LOG_INFO("module",
                "mod-tcg-vendors: Creature loot templates synced — {} boss drop row(s) active.",
                inserted);
    }
};


// ============================================================
// ============================================================
//
//  n p c _ t y r a e l s _ v e n d o r
//  Handles Edward Cairn (Horde, Undercity, entry 29095) and
//  Ian Drake (Alliance, Stormwind, entry 29093).
//
//  These vendors exist exclusively to distribute Tyrael's Hilt,
//  the reward granted at the 2008 Worldwide Invitational in Paris.
//
// ============================================================
// ============================================================
class npc_tyraels_vendor : public CreatureScript
{
public:
    npc_tyraels_vendor() : CreatureScript("npc_tyraels_vendor") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        if (player->IsGameMaster())
        {
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] " + TYRAELS_CATALOG[0].displayName,
                GOSSIP_SENDER_MAIN, 1,
                "Enter character name to deliver \"" +
                TYRAELS_CATALOG[0].displayName + "\" to:",
                0, true);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Clear character redemption flags",
                SENDER_GM_CLEAR, 0,
                "Enter character name to clear all TCG redemption flags:",
                0, true);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "[GM] Send code: " + TYRAELS_CATALOG[0].displayName,
                SENDER_GM_SEND_CODE, 1,
                "Enter character name to send a code for \"" +
                TYRAELS_CATALOG[0].displayName + "\" to:",
                0, true);
            SendGossipMenuFor(player, NPC_TEXT_WWI, creature->GetGUID());
            return true;
        }

        int mode = GetVendorMode();

        if (mode == MODE_DISABLED)
            return false;

        uint32 playerGuid = player->GetGUID().GetCounter();
        uint32 redeemKey  = TYRAELS_CATALOG[0].entries[0];
        bool   redeemed   = HasRedeemed(playerGuid, redeemKey);
        std::string label = BuildItemLabel(TYRAELS_CATALOG[0].displayName,
                                           false, redeemed);

        if (redeemed)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label,
                GOSSIP_SENDER_MAIN, 1);
        else if (mode == MODE_BLIZZLIKE || mode == MODE_ITEM_CODE)
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                GOSSIP_SENDER_MAIN, 1,
                "Enter your redemption code for \"" +
                TYRAELS_CATALOG[0].displayName + "\":",
                0, true);
        else
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label,
                GOSSIP_SENDER_MAIN, 1,
                "Receive \"" + TYRAELS_CATALOG[0].displayName + "\"?",
                0, false);

        // The WoW 3.3.5a client skips rendering the gossip window when there
        // is exactly one item with hasTextBox = true and jumps straight to the
        // text dialog.  A second item forces the menu to render so the NPC
        // greeting text is visible before the player commits to redeeming.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Never mind.",
            GOSSIP_SENDER_MAIN, 0);

        SendGossipMenuFor(player, NPC_TEXT_WWI, creature->GetGUID());
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature,
                            uint32 sender, uint32 /*action*/,
                            const char* code) override
    {
        std::string codeStr(code ? code : "");

        if (sender == SENDER_GM_CLEAR)
        {
            HandleGMClearFlags(player, creature, codeStr);
            OnGossipHello(player, creature);
            return true;
        }

        // GM force-delivery override.
        if (sender == SENDER_GM_FORCE)
        {
            HandleGMDelivery(player, creature, codeStr,
                             TYRAELS_CATALOG[0].entries,
                             TYRAELS_CATALOG[0].factionMount,
                             IsItemConsumable(TYRAELS_CATALOG[0].entries[0],
                                              TYRAELS_CATALOG[0].isConsumable),
                             TYRAELS_CATALOG[0].displayName,
                             true /* forceOverride */);
            OnGossipHello(player, creature);
            return true;
        }

        // GM send-code path.
        if (sender == SENDER_GM_SEND_CODE)
        {
            HandleGMSendCode(player, creature, codeStr,
                TYRAELS_CATALOG[0].displayName,
                TYRAELS_CATALOG[0].rewardGroupKey,
                TYRAELS_CATALOG[0].entries[0]);
            OnGossipHello(player, creature);
            return true;
        }

        // GM direct delivery.
        if (player->IsGameMaster())
        {
            bool delivered = HandleGMDelivery(player, creature, codeStr,
                                              TYRAELS_CATALOG[0].entries,
                                              TYRAELS_CATALOG[0].factionMount,
                                              IsItemConsumable(TYRAELS_CATALOG[0].entries[0],
                                                               TYRAELS_CATALOG[0].isConsumable),
                                              TYRAELS_CATALOG[0].displayName,
                                              false /* forceOverride */);
            if (!delivered)
            {
                ClearGossipMenuFor(player);
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    "[GM] Force delivery to \"" + codeStr + "\" anyway",
                    SENDER_GM_FORCE, 1,
                    "\"" + codeStr + "\" already has \"" +
                    TYRAELS_CATALOG[0].displayName +
                    "\". Enter their name again to confirm forced re-delivery:",
                    0, true);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "< Cancel", SENDER_MAIN, 0);
                SendGossipMenuFor(player, NPC_TEXT_WWI, creature->GetGUID());
            }
            else
            {
                OnGossipHello(player, creature);
            }
            return true;
        }

        // Mode 2 (Blizzlike): any valid unused code accepted.
        if (GetVendorMode() == MODE_BLIZZLIKE)
        {
            HandleCodeRedemption(player, creature, codeStr);
            CloseGossipMenuFor(player);
            return true;
        }

        // Mode 3: code must match Tyrael's Hilt specifically.
        if (GetVendorMode() == MODE_ITEM_CODE)
        {
            HandleItemSpecificCodeRedemption(
                player, creature, codeStr,
                TYRAELS_CATALOG[0].rewardGroupKey,
                TYRAELS_CATALOG[0].displayName);
            OnGossipHello(player, creature);
            return true;
        }

        return false;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 /*sender*/, uint32 action) override
    {
        // Disabled vendor: hand nothing out.  See npc_promo_vendor for why this
        // guard is repeated in every click handler.
        if (GetVendorMode() == MODE_DISABLED)
            return false;

        ClearGossipMenuFor(player);

        if (action == 0)
        {
            // "Never mind." — just close.
            CloseGossipMenuFor(player);
            return true;
        }

        // Only Mode 1 exposes a free confirmation click; Modes 2 and 3 deliver
        // through the code dialog in OnGossipSelectCode.
        if (GetVendorMode() == MODE_FREE)
            HandleBrowseSelect(player, creature, 1, TYRAELS_CATALOG);

        OnGossipHello(player, creature);
        return true;
    }
};

// ============================================================
//  G M   C O M M A N D :  .tcgcodes
//
//  Reports how many redemption codes remain for each reward.
//  Rows are "<name>: <free>/<total>", worst supply first.
// ============================================================

namespace
{
    // Per-reward code supply as stored in account_tcg_codes.
    struct CodeSupply
    {
        uint64 total = 0;
        uint64 free  = 0;
    };

    // One printable line of the report.
    struct CodeSupplyRow
    {
        std::string label;
        uint64      free  = 0;
        uint64      total = 0;
        bool        known = true;   // false => reward_group not in REWARD_GROUPS
    };

    // Read the whole supply table with a single grouped query.
    //
    // Returns false when the table cannot be read at all, so the command can
    // report that instead of printing an empty report that would be
    // indistinguishable from "no codes have been issued".
    //
    // CAST(... AS UNSIGNED) matters: Field::Get<uint64>() is only valid for a
    // BIGINT column, and MySQL types SUM() as DECIMAL regardless of its input.
    // Without the cast the count would be reinterpreted from the wrong buffer
    // type and print garbage.
    static bool LoadCodeSupply(std::map<std::string, CodeSupply>& out)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT reward_group, COUNT(*), "
            "CAST(SUM(CASE WHEN redeemed = 0 THEN 1 ELSE 0 END) AS UNSIGNED) "
            "FROM account_tcg_codes GROUP BY reward_group");

        if (!result)
            return false;

        do
        {
            Field* fields = result->Fetch();

            std::string key = fields[0].Get<std::string>();
            if (key.empty())
                continue;

            CodeSupply supply;
            supply.total = fields[1].Get<uint64>();
            supply.free  = fields[2].Get<uint64>();

            // Defensive: free can never legitimately exceed total, but the
            // report must not print nonsense if the column were ever changed.
            if (supply.free > supply.total)
                supply.free = supply.total;

            out[key] = supply;
        } while (result->NextRow());

        return true;
    }

    // Printable name for a reward group.  An unrecognised key keeps its raw
    // value and is tagged, so a stale or mistyped key is obvious rather than
    // looking like a normal entry.
    static std::string DescribeRewardGroup(std::string const& key)
    {
        auto it = REWARD_GROUPS.find(key);
        if (it == REWARD_GROUPS.end())
            return key + " [unknown key]";

        return it->second.displayName;
    }
}

// ChatCommandTable and Console live in Acore::ChatCommands.  Targeted using
// declarations are used rather than a whole-namespace directive, so the rest of
// this translation unit keeps referring to the unqualified AzerothCore names.
using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;

class tcg_codes_command : public CommandScript
{
public:
    tcg_codes_command() : CommandScript("tcg_codes_command") { }

    ChatCommandTable GetCommands() const override
    {
        // NOTE: the handler is passed by plain name, never as "&Handler".
        // ChatCommandBuilder's constructor takes "TypedHandler& handler", so the
        // argument must be an lvalue of function type.  "&Handler" produces a
        // prvalue function pointer, which cannot bind to that reference and makes
        // the whole initializer_list unconvertible.
        static ChatCommandTable commandTable =
        {
            { "tcgcodes", HandleCodesCommand, SEC_GAMEMASTER, Console::Yes },
        };

        return commandTable;
    }

    // .tcgcodes            every reward, most depleted first
    // .tcgcodes <n>        only rewards with at most n codes left
    static bool HandleCodesCommand(ChatHandler* handler, Optional<uint32> maxFree)
    {
        if (!handler)
            return false;

        std::map<std::string, CodeSupply> supply;
        if (!LoadCodeSupply(supply))
        {
            handler->PSendSysMessage(
                "TCG codes: cannot read account_tcg_codes. Is the characters "
                "database present and loaded?");
            return true;
        }

        uint64 threshold = maxFree ? static_cast<uint64>(*maxFree)
                                   : std::numeric_limits<uint64>::max();

        // Build one row per known reward group, then append any key found in the
        // database that REWARD_GROUPS does not know about.
        std::vector<CodeSupplyRow> rows;
        rows.reserve(REWARD_GROUPS.size() + supply.size());

        uint64 totalCodes = 0;
        uint64 totalFree  = 0;
        uint64 shown      = 0;
        uint64 exhausted  = 0;

        for (auto const& [key, group] : REWARD_GROUPS)
        {
            auto it = supply.find(key);

            uint64 total = it == supply.end() ? 0 : it->second.total;
            uint64 free  = it == supply.end() ? 0 : it->second.free;

            totalCodes += total;
            totalFree  += free;

            if (free == 0)
                ++exhausted;

            if (free > threshold)
                continue;

            ++shown;
            rows.push_back({ group.displayName, free, total, true });
        }

        for (auto const& [key, counts] : supply)
        {
            // War Fuel and any other catalogue-only key is intentionally never
            // issued, so it is only worth reporting if rows actually exist.
            if (REWARD_GROUPS.find(key) != REWARD_GROUPS.end())
                continue;

            if (counts.free > threshold)
                continue;

            ++shown;
            rows.push_back({ DescribeRewardGroup(key), counts.free, counts.total, false });
        }

        // Worst supply first so an exhausted reward is the first thing a GM sees.
        // Ties keep a stable, readable order by label.
        std::sort(rows.begin(), rows.end(),
                  [](CodeSupplyRow const& a, CodeSupplyRow const& b)
                  {
                      if (a.free != b.free)
                          return a.free < b.free;
                      if (a.total != b.total)
                          return a.total > b.total;
                      return a.label < b.label;
                  });

        handler->PSendSysMessage(
            "TCG codes: {} free of {} across {} reward(s); {} exhausted.",
            totalFree, totalCodes, REWARD_GROUPS.size(), exhausted);

        if (shown == 0)
        {
            handler->PSendSysMessage(
                "TCG codes: nothing at or below the filter.");
            return true;
        }

        if (shown < rows.size())
            handler->PSendSysMessage(
                "TCG codes: showing {} of {} reward(s) at or below {}.",
                shown, rows.size(), threshold);

        // Chunked so a 70-reward report does not arrive as one unreadable block.
        constexpr std::size_t LINES_PER_MESSAGE = 12;

        for (std::size_t i = 0; i < rows.size(); i += LINES_PER_MESSAGE)
        {
            std::size_t const end = std::min(i + LINES_PER_MESSAGE, rows.size());

            std::string line;
            for (std::size_t k = i; k < end; ++k)
            {
                if (k > i)
                    line += " | ";

                line += rows[k].label;
                line += ' ';
                line += std::to_string(rows[k].free);
                line += '/';
                line += std::to_string(rows[k].total);

                if (!rows[k].known)
                    line += " ?";
            }

            handler->PSendSysMessage("{}", line);
        }

        return true;
    }
};

// ============================================================
//  Script registration
// ============================================================
void Addmod_tcg_vendorsScripts()
{
    new npc_landro_longshot();
    new npc_blizzcon_vendor();
    new npc_promo_vendor();
    new npc_tyraels_vendor();
    new tcg_boss_drop_script();
    new tcg_boss_drop_player_script();
    new tcg_boss_drop_world_script();
    new tcg_codes_command();
}

