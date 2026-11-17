# FH3 Achievements

A local achievement mod for the Xbox One build of Forza Horizon 3 running in Azimuth. It lists all 135 Xbox One achievements and their original 3,000G. The mod reads 66 achievements from saves and implements nine additional live event rules. Each entry shows its requirement, points, and local progress where supported.

## Install

1. Close Azimuth.
2. Use the Xbox One FH3 build **1.0.125.0** with [FH3 Mod Platform 1.3.0 or newer](https://github.com/Levilente/FH3_ModPlatform/releases). The current FH3 platform loader is DINPUT8.dll;
3. Extract the install ZIP into the folder containing azimuth.exe. The DLL goes in **fh3-mods\FH3Achievements.dll**.
4. Start FH3. Open **Pause → Progress → Achievements** from the original Progress tile.

The in-game page follows the FH2 mod's two-column layout. Browse the sections on the left and press **A** to move into the achievement list on the right. Use **B** to go back and **LB** or **RB** to switch sections. Pop-ups can be turned off in **Home → MODS → Installed mods → FH3 Achievements**.

## Save progress

The mod checks the latest complete Azimuth FH3 save revision about every two seconds. It authenticates the save and reads the career data in memory. A fresh career starts at **0G**. A progressed career gets credit for supported goals already recorded in its save. Existing records and new unlocks are kept in **fh3-mods\FH3Achievements\User_*.txt**, separate from the game save.

The first time the mod reads a career with completed achievements, it shows the imported count and Gamerscore at the bottom of the driving HUD. The notice waits until free roam and follows the **Achievement pop-ups** setting. Profiles from earlier builds also get this one-time summary when they update.

New unlocks use FH3's own driving HUD notification and its Achievement trophy
icon. A single unlock reads **ACHIEVEMENT UNLOCKED**, followed by the achievement
name and points, such as **Welcome to Your Horizon - 10G**. Two or more unlocked
in the same detection pass share one **ACHIEVEMENTS UNLOCKED** notification with
their count and total Gamerscore. Separate batches queue in order and wait while
the native notification component is busy. Notices wait for free roam with the
game in focus and respect **Achievement pop-ups**. These are local mod unlocks;
they do not create Xbox account achievements or Windows Xbox system toasts.

The **Online** section now tracks these FH3MP-compatible achievements:

| Achievement | Tracking |
| --- | --- |
| Welcome to Horizon Online | Five completed Adventure/Free Roam events, using the saved online-finish counter. |
| Online Adventurer | 25 completed Adventure/Free Roam events, using the same counter. |
| Auction Fever | The native Auction House purchase/claim event. |
| 5 Star Seller - A+++++ | Five native successful-sale/collection events. |

Saved online finishes are recognized when the career is loaded. Auction tracking
starts with this version; total Auction House transactions are not used to guess
how many were sales. Live counters and unlocks persist per profile, using the
existing settings, native notifications, two-or-more grouping and F9 controls.
The mod is no longer marked offline-only in its platform registration.

FH3MP's current [documented services](https://github.com/Charelene/FH3MP#readme)
include Free Roam, Free Roam races, Adventure and the Auction House. Remaining
online entries still show **Online only**: Co-op, community Blueprints, Airstrip
class completion, expansion-specific online goals, Infected/Hoonigan, Forza Hub
and the old community Forzathon goals have no verified tracking path in this
version. They are not unlocked simply because the game is online.

**Untracked** means this version has no supported trigger for an achievement.
Live-only counters, such as Skill Song participation, convoy race finishes and
auction sales, start when the mod begins receiving those events. Barn Find
restoration is credited when the restored car is claimed. All Gamerscore remains
local to the mod; this does not submit achievements to Xbox or FH3MP's servers.

Check **fh3-mods\platform.log** for the native page hook, live event hook, and save sync status. To uninstall, close Azimuth and remove **FH3Achievements.dll**. You can keep the **FH3Achievements** data folder to preserve the journal.

## Build and verification

Use PowerShell and an x64 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) toolchain.

    .\Build.ps1 -ToolchainBin 'C:\path\to\llvm-mingw\bin' -SmokeTest

Run the smoke test with your own game executable and a progressed Azimuth **player-1** save folder. Give it a new, writable scratch directory.

    .\build\smoke.exe .\build\FH3Achievements.dll '<game exe>' '<player-1 folder>' '<new scratch directory>'

The harness checks the original tile hook target, page generation with fresh and progressed saves, the queued summary across reloads, previous ledger migration, a simulated live event, and save integrity. It writes the two generated XAML documents to the scratch directory for inspection. Run **build\native-notices-test.exe** for the native HUD ABI fixture: wording, trophy token, two-or-more grouping, queue order, first-run acknowledgement, busy/retry gates, pop-up settings, profile changes and Off/On component lifetime checks. Run **build\runtime-test.exe** with a new scratch-directory argument for the Load/Accept/F9 controls.

Run **build\online-test.exe** to check saved online-finish thresholds, Auction
House event payload validation, persistent-counter merging, journal status,
locked unsupported entries and forwarding while Off. The smoke harness also
records auction unlocks and reloads them without duplication or ledger rejection.

## Credits

- [Trinculo54's FH2 Achievements v1.0.2](https://github.com/Trinculo54/fh2-achievements/releases/tag/v1.0.2) provided the design for offline backfill, the achievement journal, and live event tracking.
- [Levilente's FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform) supplies the loader and API. Its SDK header is included under the MIT license.
- Save format compatibility follows the Azimuth FH3 Save Editor, with format research by Doliman100 / ForzaTech and Nenkai / ForzaTools. The mod reads the necessary lookup tables from the running game.
- The Xbox One achievement catalog was cross-checked against Xbox achievement listings and Forza Support's career totals. [TrueAchievements catalog](https://www.trueachievements.com/game/Forza-Horizon-3/achievements), [Forza Support career totals](https://support.forza.net/hc/en-us/articles/360005370173-FH3-Progress)
- SQLite is public domain. The mod source is GPL-3.0. The package contains the mod only.
