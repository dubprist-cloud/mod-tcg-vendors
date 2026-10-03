<div align="center">
  <img src="https://raw.githubusercontent.com/lightninjay/mod-tcg-vendors/refs/heads/main/banner.png" alt="TCG Vendor Banner" width="800px">
  <H1><b>TCG Vendors</b></H1><H3>Author: lightninjay<br>with the help of Claude.ai</H3><br>

An [AzerothCore](https://www.azerothcore.org/) module that implements fully functional item
redemption<br> for seven World of Warcraft promotional item NPCs present in the 3.3.5a client,
plus an optional boss drop system that generates unique TCG promotional codes on configurable
boss kills and delivers them to players as readable in-game stationery.

<H3>AFTER ENABLING BOSS DROPS, REBOOT ONCE. THE STATIONERY DROP IS THEN ACTIVE.</H3>
<H3>NO SECOND REBOOT AND NO MANUAL .reload ARE NEEDED — THE LOOT TABLES ARE</H3>
<H3>SYNCHRONISED IN-PROCESS DURING STARTUP.</H3>

| NPC | Location | Items |
|-----|----------|-------|
| **Landro Longshot** | Booty Bay | TCG card set rewards |
| **Ransin Donner** | Ironforge — The Forlorn Cavern | Blizzcon promotional items |
| **Zas'Tysh** | Orgrimmar — Valley of Heroes | Blizzcon promotional items |
| **Garel Redrock** | Ironforge — The Forlorn Cavern | Murloc companions & additional promotional items |
| **Tharl Stonebleeder** | Orgrimmar — Valley of Heroes | Murloc companions & additional promotional items |
| **Edward Cairn** | Undercity | Tyrael's Hilt (Worldwide Invitational) |
| **Ian Drake** | Stormwind | Tyrael's Hilt (Worldwide Invitational) |

Landro Longshot, Ransin Donner, and Zas'Tysh exist in the default AzerothCore database as
stubs with no item delivery logic. Garel Redrock and Tharl Stonebleeder are their counterpart
NPCs, while Ian Drake and Edward Cairn, in Stormwind and Undercity respectively, are solely responsible for the vending
 of Tyrael's hilt, a Blizzard WorldWide Invitational item. This module brings all seven NPCs to life with configurable
redemption modes, correct item entry IDs, a full GM delivery and management toolset, as well as an
automated boss drop system.

> **Note:** Developed against the
> [mod-playerbots fork](https://github.com/liyunfan1223/azerothcore-wotlk) of AzerothCore.
> Should be compatible with standard AzerothCore mainline. See
> [Compatibility](#compatibility) for details.

---

## Features

- **Four configurable operation modes** covering every server style, from fully free to
  retail-authentic code redemption.
- **70 code-redeemable rewards** across all 13 TCG expansion sets, three Blizzcon promotional
  items, the full Garel/Tharl promotional catalogue, and Tyrael's Hilt via the dedicated
  Worldwide Invitational vendors. Three of these rewards (Spectral Tiger, X-51
  Nether-Rocket, Scourgewar Mini-Mount) deliver two item entries each, for 73 item entry IDs.
  Red and Blue War Fuel are a further two catalogue entries that are never code-redeemable
  (see companion-gated items below), and the stationery carrier item 9311 makes 76 distinct
  entries the module touches in total.
- **Multi-item set support** — Spectral Tiger and X-51 Nether-Rocket each award both mount
  variants in a single redemption.
- **Faction-aware delivery** — the Scourgewar Mini-Mount awards the Horde or Alliance variant
  automatically based on the receiving character's race.
- **Unique vs. consumable item classification** — permanent unlocks are one-time per
  character; charged or stackable toys are unlimited.
- **Companion-gated items** — Red War Fuel and Blue War Fuel are freely redeemable in any
  mode, any number of times, but only to characters who have already learned the Warbot
  companion (spell 65682).
- **Inventory fallback** — if a player's bags are full at redemption, all items are mailed
  to the character rather than being blocked or lost.
- **GM free-browse menu** in all modes (including Disabled), allowing Game Masters with GM
  mode active to browse the full catalogue and deliver any item to any character by name.
- **GM force-delivery override** — if a target character has already received a unique item,
  the GM is presented with an override confirmation dialog rather than a silent block.
- **GM redemption flag management** — a dedicated menu option clears all
  `character_tcg_redeemed` records for a named character, resetting their unique-item
  eligibility.
- **Configurable Landro's Box behaviour** — Landro's Gift Box and Landro's Pet Box can be
  set to one-time or unlimited per character independently of all other items.
- **Single-use code redemption** — a code is claimed with a conditional database update plus an
  in-process lock, so concurrent submissions cannot both succeed. See
  [Code Single-Use Enforcement](#code-single-use-enforcement).
- **Boss drop system** — generates a unique TCG promotional code on each configured boss
  kill and delivers it as a readable stationery item. Three configurable delivery modes:
  loot window only, mail to one randomly chosen participant, or both.
- **`.tcgcodes`** — Game Master command reporting remaining code supply per reward, with the
  most depleted first.
- **Interactive code generator dashboard** (`tools/generate_codes.py`) with a terminal UI
  and full non-interactive CLI support for scripted workflows. It refuses to run if its
  reward groups have drifted from the C++ source.

---

## Operation Modes

Set via `TCGVendors.Mode` in `mod-tcg-vendors.conf`.

| Mode | Name | Description |
|------|------|-------------|
| `0` | **Disabled** | Module registers but defers entirely to default database gossip. No items are offered by the scripts. Useful for temporarily suspending the system without reverting any SQL. |
| `1` | **Free** | No codes required. All players browse the full catalogue and claim items directly from the gossip menu. Unique items are one-time per character; consumables are unlimited. |
| `2` | **Blizz-like** *(default)* | Players enter a pre-generated `XXXX-XXXX-XXXX-XXXX` code at the NPC root dialog. Each code is single-use, tracked at account level. The reward is determined entirely by the code — the player does not browse to an item first. Blizzcon vendors show items directly with per-item code dialogs in this mode. |
| `3` | **Item-Specific Code** | Players browse to a specific item and enter a code for that item. A valid code for a *different* item produces a mismatch error. Closest to the original retail experience. |

> **Note on companion-gated items (War Fuel):** Red War Fuel and Blue War Fuel bypass the
> mode setting entirely. They are always presented as a free confirmation click — no code is
> ever generated or required. The only gate is whether the character has already learned the
> Warbot companion pet.

---

## Item Catalogue

<details>
<summary><strong>TCG Expansions — Landro Longshot (Booty Bay)</strong></summary>

| Expansion | Item | Entry | Type |
|-----------|------|-------|------|
| Heroes of Azeroth | Tabard of Flame | 23705 | Unique |
| Heroes of Azeroth | Hippogryph Hatchling | 23713 | Unique |
| Heroes of Azeroth | Riding Turtle | 23720 | Unique |
| Through the Dark Portal | Picnic Basket | 32566 | Unique |
| Through the Dark Portal | Banana Charm | 32588 | Unique |
| Through the Dark Portal | Imp in a Ball | 32542 | Unique |
| Fires of Outland | Goblin Gumbo Kettle | 33219 | Unique |
| Fires of Outland | Fishing Chair | 33223 | Unique |
| Fires of Outland | Reins of the Spectral Tiger *(both variants)* | 33224 + 33225 | Unique |
| March of the Legion | Paper Flying Machine Kit | 34499 | Unique |
| March of the Legion | Rocket Chicken | 34492 | Unique |
| March of the Legion | Dragon Kite | 34493 | Unique |
| Servants of the Betrayer | X-51 Nether-Rocket *(both variants)* | 35225 + 35226 | Unique |
| Servants of the Betrayer | Papa Hummel's Old-Fashioned Pet Biscuit | 35223 | Consumable |
| Servants of the Betrayer | Goblin Weather Machine - Prototype 01-B | 35227 | Unique |
| Hunt for Illidan | Path of Illidan | 38233 | Consumable |
| Hunt for Illidan | D.I.S.C.O. | 38301 | Unique |
| Hunt for Illidan | Soul-Trader Beacon | 38050 | Unique |
| Drums of War | Party G.R.E.N.A.D.E. | 38577 | Consumable |
| Drums of War | The Flag of Ownership | 38578 | Unique |
| Drums of War | Big Battle Bear | 38576 | Unique |
| Blood of Gladiators | Sandbox Tiger | 45047 | Consumable |
| Blood of Gladiators | Epic Purple Shirt | 45037 | Unique |
| Blood of Gladiators | Foam Sword Rack | 45063 | Unique |
| Fields of Honor | Path of Cenarius | 46779 | Consumable |
| Fields of Honor | Ogre Pinata | 46780 | Unique |
| Fields of Honor | Magic Rooster Egg | 46778 | Unique |
| Scourgewar | Scourgewar Mini-Mount *(faction-aware)* | 49288 / 49289 | Consumable |
| Scourgewar | Tuskarr Kite | 49287 | Unique |
| Scourgewar | Spectral Tiger Cub | 49343 | Unique |
| Wrathgate | Landro's Gift Box *(configurable)* | 54218 | Unique† |
| Wrathgate | Instant Statue Pedestal | 54212 | Unique |
| Wrathgate | Blazing Hippogryph | 54069 | Unique |
| Icecrown | Paint Bomb | 54455 | Consumable |
| Icecrown | Ethereal Portal | 54452 | Unique |
| Icecrown | Wooly White Rhino | 54068 | Unique |
| Points Redemption | Tabard of Frost | 23709 | Unique |
| Points Redemption | Perpetual Purple Firework | 23714 | Unique |
| Points Redemption | Carved Ogre Idol | 23716 | Unique |
| Points Redemption | Tabard of the Arcane | 38310 | Unique |
| Points Redemption | Tabard of Brilliance | 38312 | Unique |
| Points Redemption | Tabard of the Defender | 38314 | Unique |
| Points Redemption | Tabard of Fury | 38313 | Unique |
| Points Redemption | Tabard of Nature | 38309 | Unique |
| Points Redemption | Tabard of the Void | 38311 | Unique |
| Points Redemption | Landro's Pet Box *(configurable)* | 50301 | Unique† |

† Behaviour controlled by `TCGVendors.LandroBoxesMultiRedeem`. Default is one-time per character.

</details>

<details>
<summary><strong>Blizzcon Promotional Items — Ransin Donner (Ironforge) / Zas'Tysh (Orgrimmar)</strong></summary>

| Item | Entry | Type |
|------|-------|------|
| Murky (Blue Murloc Egg) | 20371 | Unique |
| Murloc Costume | 33079 | Unique |
| Big Blizzard Bear | 43599 | Unique |

</details>

<details>
<summary><strong>Murloc Companions — Garel Redrock (Ironforge) / Tharl Stonebleeder (Orgrimmar)</strong></summary>

| Item | Entry | Type |
|------|-------|------|
| Gurky (Pink Murloc Egg) | 22114 | Unique |
| Orange Murloc Egg | 20651 | Unique |
| White Murloc Egg | 22780 | Unique |
| Heavy Murloc Egg | 46802 | Unique |
| Murkimus' Little Spear | 45180 | Unique |

</details>

<details>
<summary><strong>Classic & Special Promotions — Garel Redrock / Tharl Stonebleeder</strong></summary>

| Item | Entry | Type | Notes |
|------|-------|------|-------|
| Zergling Leash | 13582 | Unique | WoW Collector's Edition |
| Panda Collar | 13583 | Unique | WoW Collector's Edition |
| Diablo Stone | 13584 | Unique | WoW Collector's Edition |
| Netherwhelp's Collar | 25535 | Unique | TBC Collector's Edition |
| Frosty's Collar | 39286 | Unique | WotLK Collector's Edition |
| Warbot Ignition Key | 46767 | Unique | Mountain Dew Promotion |
| Red War Fuel | 46766 | Consumable | Requires Warbot companion (spell 65682) |
| Blue War Fuel | 46765 | Consumable | Requires Warbot companion (spell 65682) |

> **War Fuel:** Freely redeemable in any server mode with no code required. The character
> must have already learned the Warbot companion pet. Both fuel types are unlimited.

</details>

<details>
<summary><strong>Blizzard Store — Garel Redrock / Tharl Stonebleeder</strong></summary>

| Item | Entry | Type |
|------|-------|------|
| Enchanted Onyx | 48527 | Unique |
| Core Hound Pup | 49646 | Unique |
| Gryphon Hatchling | 49662 | Unique |
| Wind Rider Cub | 49663 | Unique |
| Pandaren Monk | 49665 | Unique |

</details>

<details>
<summary><strong>Special Events & Tournaments — Garel Redrock / Tharl Stonebleeder</strong></summary>

| Item | Entry | Type |
|------|-------|------|
| Lil' Phylactery | 49693 | Unique |
| Lil' XT | 54847 | Unique |
| Mini Thor | 56806 | Unique |
| Onyxian Whelpling | 49362 | Unique |

</details>

<details>
<summary><strong>Worldwide Invitational — Edward Cairn (Undercity) / Ian Drake (Stormwind)</strong></summary>

| Item | Entry | Type | Notes |
|------|-------|------|-------|
| Tyrael's Hilt | 39656 | Unique | 2008 Worldwide Invitational, Paris |

> **Edward Cairn and Ian Drake** are the only vendors who handle Tyrael's Hilt.

</details>

---

## Requirements

- [AzerothCore](https://www.azerothcore.org/) 3.3.5a (or the
  [mod-playerbots fork](https://github.com/liyunfan1223/azerothcore-wotlk))
- Python 3.8+ *(for `tools/generate_codes.py` only — server compilation does not require Python)*

---

## Installation

### 1. Place the module

Clone or copy the `mod-tcg-vendors` folder into your AzerothCore `modules` directory:

```
azerothcore-wotlk/
└── modules/
    └── mod-tcg-vendors/     ← folder must be named exactly this
        ├── conf/
        ├── data/sql/
        ├── src/
        └── tools/
```

> **Important:** If you downloaded a ZIP from GitHub, remove any `-main`, `-master`, or
> similar suffix from the folder name. The folder must be named exactly `mod-tcg-vendors` or
> the build system will generate a mismatched script registration function name and fail to link.

### 2. Apply the SQL

**This step is usually unnecessary.** AzerothCore's runtime database updater scans
`modules/*/data/sql/` on every server start and applies anything it has not seen before, so on
a normal build the four base files and the migration below are handled for you. Apply them by
hand only if you have disabled the auto-updater (`Updates.EnableDatabases = 0`) or want the
schema in place before the first boot.

**Characters database** — creates the two tables this module uses:

```bash
mysql -u root -p acore_characters < data/sql/characters/base/create_tcg_redeemed_table.sql
mysql -u root -p acore_characters < data/sql/characters/base/create_tcg_codes_table.sql
```

**Characters database migration** — renames the Tyrael's Hilt reward group key. Only needed on
databases that already contain rows written by an older version of the generator; a no-op
otherwise.

```bash
mysql -u root -p acore_characters < data/sql/characters/updates/zzz_tcg_tyraels_hilt_reward_group.sql
```

**World database** — sets `ScriptName` on the seven NPC entries, adds NPC greeting texts,
ensures the gossip flag is set on each NPC, and places three creature spawns:

```bash
mysql -u root -p acore_world < data/sql/world/base/zzz_tcg_vendors_setup.sql
mysql -u root -p acore_world < data/sql/world/base/Update_Warbot_Fuel.sql
```

The world SQL files handle the following automatically — no manual in-game steps required:
- Spawns **Garel Redrock** in The Forlorn Cavern next to Ransin Donner
- Spawns a **Gurky** companion murloc next to Garel, mirroring Tharl and Gurky in Orgrimmar
- Repositions **Murky** in Ironforge to stand next to Ransin Donner in the correct orientation
- Sets `ScriptName` and the gossip flag on **Edward Cairn** (29095) and **Ian Drake** (29093)

> **Edward Cairn and Ian Drake already exist** in the stock AzerothCore world database and are
> left exactly where it places them — Undercity and Stormwind respectively. The module does
> **not** move or re-spawn them; it only attaches the gossip script and the gossip flag. If
> you have relocated or removed them, spawn them yourself.

`Update_Warbot_Fuel.sql` adds four `spell_linked_spell` rows linking the two War Fuel
consumption triggers (65673, 65683) to their "Filled Up" auras. It does not touch the Warbot
companion itself or any AI.

> **These files do replace existing rows.** `zzz_tcg_vendors_setup.sql` issues a blanket
> `DELETE FROM creature WHERE id1 = <entry>` for 16070, 16069 and 15186 before re-inserting
> them. Any spawn you added for those three entries yourself — including ones placed in-game —
> is removed on every application. If you keep custom spawns for them, add them back after
> applying, or comment the corresponding `DELETE` out. Item templates and every other table
> are left untouched.

> **Load order:** The file is prefixed `zzz_` so that the DB auto-updater applies it after
> all other module SQL patches, preventing other modules from inadvertently overwriting these
> creature or NPC text entries.

> **Database names:** The commands above use the standard AzerothCore database names
> (`acore_characters`, `acore_world`). Substitute your own database names if they differ.

### 3. Recompile

Reconfigure CMake and rebuild the core. The module is detected and compiled automatically.
Verify it appears in the CMake output:

```
* Modules configuration
  ...
  + mod-tcg-vendors
```

### 4. Configure

The build already installs `conf/mod-tcg-vendors.conf.dist` to
`<build>/bin/<cfg>/configs/modules/mod-tcg-vendors.conf` for you. Copy the `.dist` to that
directory without the extension and edit it, or create the file yourself from the template in
this repository. See [Configuration](#configuration) for all available options.

Note that config files are read at startup: after changing `TCGVendors.Mode` or any
`TCGVendors.BossDrop.*` value, restart the server (or confirm your setup supports a config
reload). `BossDrop.CreatureIds` in particular is parsed once during startup.

### 5. Populate redemption codes (Modes 2 and 3 only)

If you are running Mode 2 or Mode 3, generate codes using the included tool and import them
into your characters database before players begin redeeming:

```bash
# Launch the interactive dashboard (recommended for first-time setup)
python3 tools/generate_codes.py

# Or generate a full batch non-interactively
python3 tools/generate_codes.py --all --count 10 --output codes.sql
mysql -u root -p acore_characters < codes.sql
```

See [Code Generator](#code-generator) for full usage.

---

## Configuration

All options live in `mod-tcg-vendors.conf`.

### `TCGVendors.Mode`

Controls the overall behaviour of all seven NPCs.

| Value | Behaviour |
|-------|-----------|
| `0` | **Disabled** — NPCs revert to default database gossip. No items offered. |
| `1` | **Free** — No codes. All players browse and claim items directly. |
| `2` | **Blizz-like** *(default)* — Single root code-entry dialog at Landro Longshot and the promo vendors. Blizzcon vendors list items directly with per-item code dialogs. Codes are single-use, tracked at account level. |
| `3` | **Item-Specific Code** — Browse first, then enter a code for the selected item. |

### `TCGVendors.LandroBoxesMultiRedeem`

Controls whether Landro's Gift Box (54218) and Landro's Pet Box (50301) can be redeemed more
than once per character.

| Value | Behaviour |
|-------|-----------|
| `0` *(default)* | One-time per character. Both boxes are recorded in `character_tcg_redeemed`. |
| `1` | Unlimited. Both boxes are treated as consumables. In Mode 2/3 each code is still single-use; this only removes the per-character uniqueness gate. |

### `TCGVendors.BossDrop.Enabled`

Master switch for the boss drop system. Default: `0` (disabled).

### `TCGVendors.BossDrop.CreatureIds`

Comma-separated list of creature_template entry IDs for bosses that should trigger a code
drop on kill. Example: `TCGVendors.BossDrop.CreatureIds = 11502,15990,36597`

On every server startup, the item 9311 loot rows **owned by this module** are purged from
`creature_loot_template` and then re-inserted exclusively for the entries listed here (subject
to `TCGVendors.BossDrop.MailParticipants` — see below). The loot tables are reloaded in-process
immediately after. Adding or removing a boss takes effect on the next restart.

The purge is scoped by the row's `Comment` marker (`TCG code scroll...`), so Simple Stationery
rows belonging to any other system are left untouched. The purge runs even when
`BossDrop.Enabled = 0`, which is what makes disabling the module — or emptying `CreatureIds` —
actually clear rows left over from an earlier configuration.

### `TCGVendors.BossDrop.ItemIds`

Comma-separated list of item entry IDs from the TCG/promotional catalogs. Each boss kill
randomly selects one item from this pool; the generated stationery code will be redeemable
for that item. All entries must exist in the module's reward catalog.
Example: `TCGVendors.BossDrop.ItemIds = 23720,33224,38576`

### `TCGVendors.BossDrop.MailParticipants`

Controls how stationery is delivered to players and whether a loot-window drop is generated.

**Exactly one participant is mailed per kill.** Candidates are the members of the killer's
group/raid who are on the same map instance; the killer is always eligible. Being the killer
does not disqualify anyone — the recipient is a uniform random draw from the candidate pool.
This option's name is historical: it used to mail every member, which made the reward worthless
in a full raid.

| Value | Behaviour |
|-------|-----------|
| `0` *(default)* | **Loot window only.** A single stationery item appears on the boss corpse. Loot template rows for item 9311 are inserted on startup for all configured boss entries. |
| `1` | **Mail only.** One randomly chosen participant receives a stationery in their mailbox with a unique code. No loot template rows are inserted — item 9311 does NOT appear on the corpse. |
| `2` | **Mail AND loot.** One randomly chosen participant receives a mailed stationery with their own unique code, AND a single stationery item appears on the boss corpse as a loot-window drop. Up to two codes can therefore be issued per kill. |

Solo players (no group) receive mail to themselves when mail delivery is active. The server log
records the selected recipient and the candidate pool size at `INFO` level under the `module`
logger, which is the quickest way to confirm the roll is behaving.

---

## Boss Drop System

When enabled, the boss drop system hooks into the kill event for each configured boss. On
kill, a reward item is selected at random from `TCGVendors.BossDrop.ItemIds`, a unique
`XXXX-XXXX-XXXX-XXXX` code is generated and inserted into `account_tcg_codes` as unredeemed,
and delivery proceeds according to `TCGVendors.BossDrop.MailParticipants`.

When the loot-window path is active, each player who picks the stationery up off the corpse
receives their own freshly generated code, stamped onto the item as it is created. When the
mail path is active, the single randomly selected participant receives their code on a mailed
stationery instead. The two paths generate codes independently.

The stationery item (Default Stationery, item 9311) is readable in-game by right-clicking it
in the player's inventory. It contains the generated code, the name of the boss defeated,
the name of the redeemable item, and directions to the correct vendor NPC. The code can then
be redeemed at the appropriate NPC using the normal redemption flow.

**Loot table management** is handled automatically on every server startup — no manual SQL
or `.reload creature_loot_template` is ever required. The module purges only its own item 9311
loot rows (identified by the `Comment` marker) and re-inserts exactly the set dictated by the
current config, ensuring the live loot tables always mirror the config file without disturbing
unrelated loot tables.

---

## GM Tools

Game Masters with GM mode active (`.gm on`) receive a special menu at all seven NPCs in every
operation mode, including Mode 0. GMs playing without GM mode active see the same menu as
regular players.

### Item Delivery

At Landro Longshot, the GM browses through the full expansion tree. At the Blizzcon and
Promo vendors, all items are listed directly. Clicking any item opens a text input prompting
for a character name. The item is then mailed to that character — online or offline — without
consuming a code or checking bag space.

### Force-Delivery Override

If the target character has already received a unique item, the GM is shown an override
confirmation dialog. Typing the character's name a second time confirms the forced
re-delivery, bypassing the redemption flag check entirely.

### Clear Redemption Flags

A dedicated menu option at each NPC prompts for a character name and deletes all rows in
`character_tcg_redeemed` for that character, resetting their eligibility for all unique items.
The count of cleared records is reported back as a whisper.

### Mail an Item Code

This functions exactly like Item Delivery does, except instead of delivering the item to the
player's mailbox, a code and some flavor text are generated into a Default Stationery item,
and mailed to the player so that they can redeem the item themselves in-game.

### `.tcgcodes` — Remaining Code Supply

A chat command (Game Master level, also usable from the server console) that reports how many
redemption codes are left for each reward. Format is `name free/total`, several rewards per
line, **most depleted first** so exhausted rewards appear immediately:

```
.tcgcodes
TCG codes: 1340 free of 1400 across 70 reward(s); 0 exhausted.
Tabard of Flame 20/20 | Deepmolt's Hide 20/20 | Riding Turtle 20/20 | ...
```

Optional numeric argument filters to rewards at or below that many remaining codes, which is
the form most useful for "what needs replenishing":

```
.tcgcodes 0      -- only rewards with no codes left
.tcgcodes 5      -- only rewards with 5 or fewer left
```

Three details worth knowing:

- Rewards present in `REWARD_GROUPS` but absent from `account_tcg_codes` are reported as `0/0`,
  so an unstocked reward is visible rather than missing from the report.
- A `reward_group` found in the database that `REWARD_GROUPS` does not know about is listed
  with its raw key and the suffix `[unknown key]`, flagged with `?`. This is how a stale or
  mistyped key — the same class of problem as the `PROMO_TYRAELS_HILT` bug — becomes visible.
  Note that Red/Blue War Fuel are deliberately never issued, so they never appear here.
- The header totals count every reward group, including ones the filter hides.

If `account_tcg_codes` cannot be read at all, the command says so instead of printing an empty
report that would be indistinguishable from "no codes issued".

---

## Code Generator

`tools/generate_codes.py` generates cryptographically random redemption codes in
`XXXX-XXXX-XXXX-XXXX` format using an unambiguous character set (no `I`, `L`, `O`, `0`, or
`1`). Output is `INSERT IGNORE INTO account_tcg_codes` SQL statements ready to apply directly
to your characters database.

**No external dependencies required.** Uses only the Python standard library.

> **Note:** Red War Fuel, Blue War Fuel, and boss drop codes are intentionally absent from
> the code generator. War Fuel is always redeemed freely at the NPC. Boss drop codes are
> generated automatically at kill time and inserted directly by the server.

### Interactive Dashboard

```bash
python3 tools/generate_codes.py
```

| Option | Description |
|--------|-------------|
| **Single Item** | Select one item, enter a count and optional output path. |
| **Multi-Select** | `Space` to toggle items, `A` to select all, `N` to clear, `Enter` to confirm. |
| **Full Batch** | Generates codes for all code-redeemable items. Prompts for count and output path. |
| **Quick Batch** | Full batch with a single count prompt — fastest option for populating a new server. |
| **Exit** | Quit. |

### Non-Interactive CLI

```bash
# List all valid reward group keys
python3 tools/generate_codes.py --list-groups

# Generate 10 codes for the Riding Turtle
python3 tools/generate_codes.py --reward-group TCG_RIDING_TURTLE --count 10

# Generate codes for multiple items in one run
python3 tools/generate_codes.py --reward-group TCG_RIDING_TURTLE TCG_SPECTRAL_TIGER --count 5

# Generate codes for every available item and write to a file
python3 tools/generate_codes.py --all --count 10 --output full_batch.sql

# Preview codes without generating SQL
python3 tools/generate_codes.py --reward-group BLIZZCON_MURKY --count 3 --dry-run

# Force the interactive dashboard even if other flags are present
python3 tools/generate_codes.py --interactive
```

### Applying Codes

```bash
mysql -u root -p acore_characters < full_batch.sql
```

---

## Database Tables

### Characters database

**`character_tcg_redeemed`** — tracks which unique items have been delivered to each
character. Prevents a character from receiving the same permanent unlock more than once,
regardless of how many valid codes are presented.

| Column | Type | Description |
|--------|------|-------------|
| `guid` | `INT UNSIGNED` | Character GUID |
| `item_entry` | `MEDIUMINT UNSIGNED` | Item template entry ID |
| `redeemed_date` | `TIMESTAMP` | When the item was delivered |

Primary key: `(guid, item_entry)`

**`account_tcg_codes`** — stores pre-generated redemption codes. Populated by
`tools/generate_codes.py` for vendor redemptions, and by the boss drop system at kill time.

| Column | Type | Description |
|--------|------|-------------|
| `code` | `VARCHAR(19)` | The code in `XXXX-XXXX-XXXX-XXXX` format |
| `reward_group` | `VARCHAR(64)` | Identifies which item(s) the code awards |
| `redeemed` | `TINYINT(1)` | `0` = unused, `1` = redeemed |
| `account_id` | `INT UNSIGNED` | Account that redeemed the code (set on use) |
| `character_guid` | `INT UNSIGNED` | Character that received the items (set on use) |
| `redeemed_date` | `TIMESTAMP` | When the code was consumed (set on use) |

Primary key: `code`

The `reward_group` values match the keys shown by `--list-groups` exactly. To add codes
manually without using the generator, insert rows with a valid `reward_group` key and
`redeemed = 0`.

---

## Technical Notes

### Inventory Full — Mail Fallback

If a player's bags are full at the time of redemption, all items for that redemption are
mailed to the character rather than blocking the delivery. The redemption is recorded
immediately so a full-bags situation cannot be exploited to obtain items repeatedly.

If an item could not be created at all, the redemption is **not** recorded and the player is
told so explicitly — a failed delivery never burns a code.

### Crash Safety (Modes 2 and 3)

In code-entry modes, the `account_tcg_codes` row is claimed *before* items are delivered. If the
server dies between the claim and the delivery, a Game Master can inspect the row and use the GM
delivery menu to re-deliver without consuming an additional code. The reverse order would let a
crash hand out a second copy.

### Startup Validation

`Item::CreateItem()` calls `ABORT()` — taking the worldserver down — when `item_template` has no
row for the requested entry. To make sure a typo in a catalogue cannot reach that point, every
item entry referenced by a vendor catalogue or a reward group is validated against
`item_template` during startup. Any entry that does not exist is logged with its source at
`ERROR` level, together with a summary line:

```
mod-tcg-vendors: All catalog item entries validated against item_template.
```

Watch for this line after every catalogue change; a non-zero count in the error line means the
affected rewards will abort the server if redeemed until the entry is fixed.

### Unique vs. Consumable — Companion Items

Several items have `spellcharges = -1` in `item_template`, which might suggest they are
consumable. However, items that teach a **permanent companion spell** are classified as
**Unique** (one-time per character) because the spell is learned once and the player has no
reason to receive the item again. This applies to Soul-Trader Beacon (38050), Banana Charm
(32588), and Tuskarr Kite (49287), among others. True consumables — Papa Hummel's Pet
Biscuit, Path of Illidan, Sandbox Tiger, Paint Bomb, and similar charged toys — are items
where the player genuinely benefits from having more than one.

### Scourgewar Mini-Mount Faction Handling

The Scourgewar Mini-Mount has two item entries: Little Ivory Raptor Whistle (49288) for
Horde and Little White Stallion Bridle (49289) for Alliance. The delivery system resolves
the correct variant from the *receiving character's* race — not the GM's or the redeeming
player's. The redemption key stored in `character_tcg_redeemed` is the variant that was
actually delivered (49288 for Horde, 49289 for Alliance), so the per-character gate works
correctly and independently for both factions.

### Boss Drop Stationery Readability

The stationery item uses `ITEM_FIELD_FLAG_READABLE` (flag `0x00000200`) to signal to the
WoW 3.3.5a client that the item has readable text, and `Item::SetText()` to populate the
string. `Item::SaveToDB()` persists that text into `item_instance.text`, and
`WorldSession::HandleItemTextQuery` answers the client's read request straight from the live
`Item` object — so no additional direct `UPDATE item_instance` is performed.
`SetState(ITEM_CHANGED)` makes sure the readable flag reaches the client on the next update
cycle.

### Code Single-Use Enforcement

A redemption code can only be used once. Two independent mechanisms enforce this:

1. The claim statement carries `AND redeemed = 0`, so the database itself refuses to mark an
   already-used code a second time even under concurrent submissions.
2. An in-process mutex serialises the read-validate-claim sequence. This is required because
   AzerothCore's `DatabaseWorkerPool::Execute()` is asynchronous and returns `void`, and
   `Query()` and `Execute()` run on separate connection pools (`IDX_SYNCH` vs `IDX_ASYNC`) —
   so neither affected-row counts nor read-after-write ordering are observable from module
   code. The claim therefore uses `DirectExecute`, which blocks on the synchronous pool, and the
   lock is released again before the item is delivered so the serialised round-trip stays short.

For the same read-after-write reason, the `character_tcg_redeemed` insert and the code insert
also use `DirectExecute`: an asynchronous write would not yet be visible to the next click's
verification query, which would let a one-per-character reward be handed out twice.

Server-generated codes (boss drops and GM send-code) use the OpenSSL-backed CSPRNG
(`Acore::Crypto::GetRandomBytes`) rather than `urand()`, so they are not predictable from
observed random output.

### GM Force-Delivery Encoding

For the GM force-delivery override at Landro Longshot, the `action` value passed to the
confirmation handler encodes both the expansion sub-menu sender and the item index as
`(origSender << 8) | origAction`. This allows the override handler to reconstruct exactly
which item is being re-delivered without any additional state. Safe as long as sender values
and item counts per expansion remain below 256, which they do with considerable headroom.

---

## Compatibility

| Core | Status |
|------|--------|
| [mod-playerbots fork](https://github.com/liyunfan1223/azerothcore-wotlk) | ✅ Developed and tested here |
| [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) mainline | ✅ Should be compatible |

The script registration function is named `Addmod_tcg_vendorsScripts()`, following the
Playerbots fork build system conventions. This name is also valid on standard AzerothCore
(the convention was standardised in a 2022 core update), so no changes should be required
for either target.

---

## Changelog

### File index

| File | Description |
|------|-------------|
| `src/mod_tcg_vendors.cpp` | All C++ logic — NPC scripts, delivery, code redemption, boss drop, `.tcgcodes` |
| `conf/mod-tcg-vendors.conf.dist` | Configuration template — copy and remove `.dist` to activate |
| `conf/conf.sh.dist` | Legacy DB-assembler path declarations. Not read by the runtime auto-updater; kept for older tooling only |
| `data/sql/characters/base/create_tcg_redeemed_table.sql` | Creates `character_tcg_redeemed` table |
| `data/sql/characters/base/create_tcg_codes_table.sql` | Creates `account_tcg_codes` table |
| `data/sql/characters/updates/zzz_tcg_tyraels_hilt_reward_group.sql` | Migration: renames the Tyrael's Hilt reward group key |
| `data/sql/world/base/zzz_tcg_vendors_setup.sql` | NPC script names, gossip flags, NPC texts, three creature spawns |
| `data/sql/world/base/Update_Warbot_Fuel.sql` | Links the War Fuel consumption triggers to their "Filled Up" auras |
| `tools/generate_codes.py` | Interactive and CLI code generation tool |
| `tools/full_batch.sql` | Pre-generated batch of 1400 codes (70 rewards × 20). Regenerate with `generate_codes.py` for your own server |

### v1.0 — Initial implementation

- `npc_landro_longshot` — full TCG expansion browse tree with 46 items across 13 set categories,
  Mode 0–3 support, GM free-browse and delivery.
- `npc_blizzcon_vendor` — Ransin Donner and Zas'Tysh handling Murky, Murloc Costume, and
  Big Blizzard Bear, Mode 0–3 support.
- Four operation modes: Disabled, Free, Blizz-like (root code entry), Item-Specific Code.
- `character_tcg_redeemed` and `account_tcg_codes` database tables.
- Inventory-full mail fallback, crash-safe code marking, faction-aware mount delivery.
- `tools/generate_codes.py` — interactive TUI dashboard and full CLI for code generation.
- `npc_promo_vendor` — new script handling both Garel Redrock (Alliance, entry 16070) and
  Tharl Stonebleeder (Horde, entry 16076), covering 23 additional promotional items across
  four categories: Murloc Companions, Classic & Special Promotions, Blizzard Store, and
  Special Events & Tournaments.
- `zzz_tcg_vendors_setup.sql` — automatically spawns Garel Redrock and Gurky in The Forlorn
  Cavern (Ironforge), repositions Murky next to Ransin Donner, prefixed `zzz_` for correct
  DB auto-updater load order.
- All promo items added to `generate_codes.py`; War Fuel explicitly excluded (no codes).
- GM force-delivery override — when a target character has already received a unique item,
  GM is shown a confirmation dialog rather than a silent block. Typing the character's name
  a second time bypasses the redemption flag.

### v1.1 — Boss drop system

- `tcg_boss_drop_world_script` — on startup, purges all item 9311 rows from
  `creature_loot_template` and re-inserts exactly the set dictated by `BossDrop.CreatureIds`
  and `BossDrop.MailParticipants`, then reloads creature loot tables in-process.
- `tcg_boss_drop_script` — `PlayerScript` using `OnPlayerCreatureKill` to detect configured
  boss kills, generate a code, park metadata for the loot hook, and optionally mail unique
  stationery to all group/raid participants.
- `tcg_boss_drop_player_script` — `PlayerScript` using `OnPlayerLootItem` to inject the
  generated code text onto the freshly-created stationery instance when a player loots it
  from the boss corpse.
- `StampItemText()` — sets `ITEM_FIELD_FLAG_READABLE` and `Item::SetText()` to make the
  stationery right-clickable and readable in-game.
- `DirectWriteItemText()` — immediately writes text to `item_instance.text` as a safety net
  for forks where `SaveToDB` does not include `m_text`.
- `TCGVendors.BossDrop.MailParticipants` expanded from bool to three-value int: `0` = loot
  only, `1` = mail only (no loot rows inserted), `2` = mail and loot.
- New config keys: `TCGVendors.BossDrop.Enabled`, `TCGVendors.BossDrop.CreatureIds`,
  `TCGVendors.BossDrop.ItemIds`, `TCGVendors.BossDrop.MailParticipants`.
- `#include "Group.h"` and `#include "LootMgr.h"` added.

### v1.2 — GM Send Item Code
- `BuildGMCodeText`, `HandleGMSendCode`, `ShowExpansionListForCode`,
  `ShowExpansionItemsForCode`, `ShowPromoCategoryListForCode`, and `ShowPromoItemsForCode` 
  added to handle generating the custom text for the Stationery item, and the
  new menu option to allow GM's in GM mode to send an item code to a player.

### v1.3 — Worldwide Invitational vendors (Edward Cairn & Ian Drake)

- `NPC_EDWARD_CAIRN = 29095` (Horde, Undercity) and `NPC_IAN_DRAKE = 29093`
  (Alliance, Stormwind) added to `TCGNPCEntries`.
- `NPC_TEXT_WWI = 90004` added to `TCGNpcTextIds` with Worldwide Invitational
  flavour text referencing the 2008 Paris event.
- `TYRAELS_CATALOG` — new single-item catalog containing only Tyrael's Hilt.
- Tyrael's Hilt removed from `PROMO_CATALOG` `SENDER_PROMO_CLASSIC` section;
  its `REWARD_GROUPS` key was renamed `PROMO_TYRAELS_HILT` -> `WWI_TYRAELS_HILT`
  to match the vendor prefix convention (`TCG_*`, `PROMO_*`, `BLIZZCON_*`,
  `WWI_*`). Existing codes stored under the old key are migrated by
  `zzz_tcg_tyraels_hilt_reward_group.sql`.
- `npc_tyraels_vendor` — new `CreatureScript` handling both NPCs, with full
  Mode 0–3 support, GM deliver, GM send-code, force-delivery override, and
  redemption flag clear — consistent with all other vendor classes.
- `generate_codes.py` updated: Tyrael's Hilt moved from the
  "Classic & Special Promotions" group to a new "Worldwide Invitational" group.
- `zzz_tcg_vendors_setup.sql` updated: `npc_text` 90004, `ScriptName` and
  `npcflag` updates for entries 29095 and 29093.

### Unreleased — correctness and hardening

- **`.tcgcodes` command.** New Game Master chat command (also usable from the console) that
  reports remaining redemption-code supply per reward as `name free/total`, most depleted
  first, with an optional numeric threshold (`.tcgcodes 0`, `.tcgcodes 5`) to show only what
  needs replenishing. Rewards with no rows are reported as `0/0` rather than omitted, and a
  `reward_group` unknown to `REWARD_GROUPS` is flagged so a stale or mistyped key is visible
  without hand-written SQL. See [`.tcgcodes`](#tcgcodes--remaining-code-supply).
- **Startup validation of catalog item entries.** Every item entry referenced by a catalogue or
  a reward group is checked against `item_template` at startup, because
  `Item::CreateItem()` calls `ABORT()` on a missing row. See
  [Startup Validation](#startup-validation).
- **Code redemption race.** The `SELECT redeemed` → branch → `UPDATE` sequence is no longer
  open to a double redemption. The claim statement now carries `AND redeemed = 0` (the database
  refuses to mark a used code twice) and an in-process mutex serialises the check-and-claim.
  Both are required — see
  [Code Single-Use Enforcement](#code-single-use-enforcement) for why the asynchronous database
  layer makes a database-only fix insufficient.
- **Database writes made synchronous.** The code claim, the `character_tcg_redeemed` insert, the
  code insert and the GM flag clear all use `DirectExecute` instead of the asynchronous
  `Execute`, closing a read-after-write window across the two connection pools.
- **Boss-drop loot sync made atomic and ordered.** `OnStartup` runs the purge-and-insert as a
  single `DirectCommitTransaction`, which blocks and commits atomically, instead of enqueueing
  `Execute()` writes on the async pool and immediately re-reading the table on the sync pool.
- **Item burned when creation fails.** `MailItemsToPlayer()` now reports whether every item was
  created. If any `Item::CreateItem()` fails, the redemption is *not* recorded and the player
  (or GM) is told the reward was neither delivered nor consumed. Previously the flag was set and
  the caller reported success while nothing existed. Same fix in `HandleGMDelivery()`.
- **Faction-mount redemption key.** The recorded key is now the variant actually delivered
  (49288 Horde / 49289 Alliance) on both the player and GM paths. Previously the player path
  recorded `entries[0]` for both factions while the GM path recorded the resolved variant,
  which let an Alliance mount be claimed twice.
- **Boss-drop loot purge scoped.** `OnStartup` deletes only item 9311 rows carrying this
  module's `Comment` marker instead of every item 9311 row in the database, so unrelated
  Simple Stationery loot survives a restart.
- **Boss-drop reward delivery is now a single random recipient.** `MailParticipants >= 1` used
  to mail a stationery with a unique code to *every* member of the killer's group/raid, so a
  full raid meant the whole group received the same reward. It now draws one recipient uniformly
  from the participants present on the same map (the killer is always eligible, and solo players
  mail to themselves). With `MailParticipants = 2` a kill can therefore still issue two codes —
  one mailed, one from the corpse. The option name is unchanged for compatibility.
- **Boss-drop state thread safety.** `s_pendingBossDrops` is guarded by a mutex — AzerothCore
  runs one update thread per map *instance*, so two concurrent raids could corrupt the shared
  map. Entries also expire after 5 minutes instead of leaking for the process lifetime.
- **Config parsing.** `std::stoul` (which throws) replaced with `strtoul` plus `errno`/`endptr`
  validation, so a malformed `BossDrop.CreatureIds` or `ItemIds` entry is logged and skipped
  instead of aborting the worldserver from inside `OnPlayerCreatureKill`. Trailing and doubled
  commas are tolerated. The two id lists are parsed once at startup rather than on every boss
  kill and every stationery looted.
- **CSPRNG for server-generated codes.** `GenerateRandomCode()` uses
  `Acore::Crypto::GetRandomBytes` (OpenSSL) with rejection sampling for an unbiased draw, instead
  of `urand()` whose SFMT state is recoverable. Offline codes in `tools/generate_codes.py`
  already used `secrets`.
- **`MODE_DISABLED` enforced in all click handlers.** All four vendor `OnGossipSelect`
  implementations now refuse to act when the module is disabled, instead of only Landro's.
  Blizzcon and Tyrael's additionally only deliver on the Mode 1 confirmation click.
- **`IsItemConsumable` on every promo path.** The Landro box override is now honoured in the
  promo menus, GM delivery and GM force-delivery, so menu labels and delivery decisions can no
  longer disagree between vendor classes.
- **Promo vendor navigation.** The send-code category list's "Back" button no longer lands in
  the delivery tree, and the (player-reachable) promo category list finally has a "Back" entry.
- **`DirectWriteItemText()` removed.** `Item::SaveToDB()` already persists `m_text`, and
  `HandleItemTextQuery` answers from the live `Item`, so the three extra synchronous
  `UPDATE item_instance` writes were dead work — including one on the loot hot path. The
  surrounding comments claiming the server re-reads `item_instance.text` were incorrect for this
  core and have been rewritten.
- **`BuildItemLabel()`** takes the already-computed redeemed flag instead of issuing a second
  identical blocking `HasRedeemed` query per menu entry.
- **`InsertCodeToDatabase()`** uses `INSERT IGNORE` (matching `generate_codes.py`) and documents
  that `Execute()` cannot report failure.
- **`NormalizeCode()`** casts to `unsigned char` before `std::toupper`, avoiding undefined
  behaviour on non-ASCII input.
- Item `9311` replaced by the named `ITEM_SIMPLE_STATIONERY` constant.
- `tools/generate_codes.py` now verifies its `EXPANSIONS` keys against `REWARD_GROUPS` in the C++
  source on every run (both directions, plus catalog keys that resolve to no group) and exits
  non-zero on drift instead of emitting SQL the server will reject.
  
---

## Credits

- [AzerothCore](https://www.azerothcore.org/) — the core emulator and module framework.
- The AzerothCore community for module conventions and reference implementations.
- Blizzard Entertainment — original designers of the WoW TCG promotional item system and the
  NPCs this module brings to life.

---

## License

This module is released under the [MIT License](LICENSE).

---

*AzerothCore: [repository](https://github.com/azerothcore/azerothcore-wotlk) •
[website](https://www.azerothcore.org/) •
[Discord](https://discord.gg/gkt4y2x)*

</div>
