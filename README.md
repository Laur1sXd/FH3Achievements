# FH3 Achievements

A local achievement mod for the Xbox One build of Forza Horizon 3 running in Azimuth. It lists all 135 Xbox One achievements and their original 3,000G. All 135 entries now have local tracking paths when the native event and PR readers are active. Save data supplies full or partial backfill for 77 goals; live-only conditions are recorded as you play. Each entry shows its requirement, points, and progress.

Encore's FH3MP shim installation is recognized without relying on the platform's
older FH2MP detector. The journal keeps its normal progress and unlocked status
in offline and FH3MP play. Installed multiplayer files do not mean you have
completed an online requirement.

[Version 1.0.2 changelog](release-assets/FH3MP-1.0.2.md)

## Install

1. Close Azimuth.
2. Use the Xbox One FH3 build **1.0.125.0** with [FH3 Mod Platform 1.3.0 or newer](https://github.com/Levilente/FH3_ModPlatform/releases). The current FH3 platform loader is DINPUT8.dll;
3. Extract the install ZIP into the folder containing azimuth.exe. The DLL goes in **fh3-mods\FH3Achievements.dll**.
4. Start FH3. Open **Pause → Progress → Achievements** from the original Progress tile.

The in-game page follows the FH2 mod's two-column layout. Browse the sections on the left and press **A** to move into the achievement list on the right. Use **B** to go back and **LB** or **RB** to switch sections. Pop-ups can be turned off in **Home → MODS → Installed mods → Achievements**.

## Save progress

The mod checks the latest complete Azimuth FH3 save revision about every two seconds. It authenticates the save and reads the career data in memory. A fresh career starts at **0G**. A progressed career gets credit for supported goals already recorded in its save. Existing records and new unlocks are kept in **fh3-mods\FH3Achievements\User_*.txt**, separate from the game save.

Multiple saves can remain in the same **player-1** folder. The mod reads the
game's active local-player identity and selects only that career's revisions.
Offline and Encore profiles keep separate achievements, counters and unlock
dates. Switching careers restores the matching journal. A missing active save
starts at zero rather than importing a different player's progress.

Driver-level achievements also use saved XP: the cached `Main/Level` field can
lag behind the level shown in FH3. The original XP curve is 20,000 XP per level
through both level goals. A career with 395,385 XP therefore gets level 20
credit even if its cached level still says 5.

The first time the mod reads a career with completed achievements, it shows the imported count and Gamerscore at the bottom of the driving HUD. The notice waits until free roam and follows the **Achievement pop-ups** setting. Profiles from earlier builds also get this one-time summary when they update.

New unlocks use FH3's own driving HUD notification and its Achievement trophy
icon. A single unlock reads **ACHIEVEMENT UNLOCKED**, followed by the achievement
name and points, such as **Welcome to Your Horizon - 10G**. Two or more unlocked
in the same detection pass share one **ACHIEVEMENTS UNLOCKED** notification with
their count and total Gamerscore. Separate batches queue in order and wait while
the native notification component is busy. Notices wait for free roam with the
game in focus and respect **Achievement pop-ups**. These are local mod unlocks;
they do not create Xbox account achievements or Windows Xbox system toasts.

The event reader covers Adventure/Free Roam finishes, Auction House claims and sales,
Co-op events, community Blueprints, Airstrip car classes, and Infected in the
Hoonigan Bel Air. Blizzard Mountain and Hot Wheels online goals require their
own map and a matching route. Mountain Co-op races are counted separately from
its Adventure/Free Roam races. Hot Wheels' online total accepts its supported
Co-op, Adventure and Free Roam race events. Quits, incomplete results and Rivals
are excluded; repeated notifications for one race are de-duplicated by RoundId.

**Tracking support does not add a missing FH3MP service.** If the installed
FH3MP version cannot run Co-op, community Blueprints, DLC multiplayer, Infected,
or launch the retired Forza Hub app, those goals wait for the genuine native
event. Merely going online does not unlock them. The four final Forzathon goals
use Forza Support's documented combined race/skill alternatives locally.

Blizzard Mountain, Hot Wheels and Hoonigan car challenges use the actual car,
map, skill type/grade and completed result. The PR reader watches the game's
native completion callback for Danger Signs, Drift Zones, Speed Traps and Speed
Zones. It validates the source entity and its three-star threshold, then reads
the player's current car, class and mass. Goals requiring different stunts,
classes or Hoonigan cars retain distinct identifiers across game restarts and
F9 reloads. Repeating one stunt cannot count as several different stunts.

Past progress is restored only where the save proves the requirement. An old PR
personal best does not prove which car performed it. Car-specific stunts, skills,
blueprint authorship and other live-only evidence must be performed with the
updated tracker running when that history is absent. Owning a DLC does not
unlock its achievements automatically. A new career starts with zero progress.

The updated local record is **FH3ACH4** and reads existing FH3ACH2/FH3ACH3 records.
Back up the journal before downgrading: old DLLs cannot read the new evidence
rows. All Gamerscore remains local; the mod does not submit Xbox account unlocks
or change game saves. Details and verification limits are in
[docs/tracking.md](docs/tracking.md).

Check **fh3-mods\platform.log** for the native page hook, live event hook, and save sync status. To uninstall, close Azimuth and remove **FH3Achievements.dll**. You can keep the **FH3Achievements** data folder to preserve the journal.

## Build and verification

Use PowerShell and an x64 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) toolchain.

    .\Build.ps1 -ToolchainBin 'C:\path\to\llvm-mingw\bin' -SmokeTest

Run the smoke test with your own game executable and a progressed Azimuth **player-1** save folder. Give it a new, writable scratch directory.

    .\build\smoke.exe .\build\FH3Achievements.dll '<game exe>' '<player-1 folder>' '<new scratch directory>' mediaoverride User_9000000000000

When several profiles exist, give the smoke harness the profile to use as its
save fixture. Run **build\save-profiles-test.exe** for active-player memory
guards, multiple-profile selection, case-insensitive identity matching and
revision fallback within the selected career. The smoke harness also switches
between two populated profiles and an unsaved third player, checking journal
restoration and preventing live achievement evidence from crossing careers.

The harness checks the original tile hook target, page generation with fresh and progressed saves, the queued summary across reloads, previous ledger migration, a simulated live event, and save integrity. It writes the two generated XAML documents to the scratch directory for inspection. Run **build\native-notices-test.exe** for the native HUD ABI fixture: wording, trophy token, two-or-more grouping, queue order, first-run acknowledgement, busy/retry gates, pop-up settings, profile changes and Off/On component lifetime checks. Run **build\runtime-test.exe** with a new scratch-directory argument for the Load/Accept/F9 controls.

Run **build\online-test.exe** for FH3MP installation detection, normal journal
status, online-finish backfill and Auction House payload validation;
**build\tracking-test.exe** for map/route isolation, race completion,
Co-op, skill/car guards and distinct car classes; and
**build\pr-tracking-test.exe** for the native memory decoder, PR score boundaries,
unique stunts, Hoonigan conditions, invalid pointers, native forwarding and Off.
The PR fixture also checks that all 135 goals have a target and start at zero
when both native readers are available. The smoke harness verifies evidence
persistence, legacy record migration, profile isolation and repeated F9 cycles.

## Credits

- [Trinculo54's FH2 Achievements v1.0.2](https://github.com/Trinculo54/fh2-achievements/releases/tag/v1.0.2) provided the design for offline backfill, the achievement journal, and live event tracking.
- [Levilente's FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform) supplies the loader and API. Its SDK header is included under the MIT license.
- [Charelene's FH3MP](https://github.com/Charelene/FH3MP) — provided FH3MP (through Encore launcher), which makes online multiplayer possible in Azimuth FH3.
- Save format compatibility follows the Azimuth FH3 Save Editor, with format research by Doliman100 / ForzaTech and Nenkai / ForzaTools. The mod reads the necessary lookup tables from the running game.
- The Xbox One achievement catalog was cross-checked against Xbox achievement listings and Forza Support's career totals. [TrueAchievements catalog](https://www.trueachievements.com/game/Forza-Horizon-3/achievements), [Forza Support career totals](https://support.forza.net/hc/en-us/articles/360005370173-FH3-Progress)
- SQLite is public domain. The mod source is GPL-3.0. The package contains the mod only.
