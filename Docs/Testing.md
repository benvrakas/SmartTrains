# In-game tests

Test builds are installed locally (SmartTrains is not on SMR). Use a test copy (`/st testcopy`) or a creative world.

## M0: control and docking (0.1.0)

1. **Dock map prediction.** Drive a train with 1 locomotive and 3 freight cars to a station with 4 platforms. Run
   `/st fit`: each car should be predicted at P1, P2, P3. Let it dock, then run `/st fit` again: it should say
   "seen" and show the same cars. Repeat with a push-pull train (locomotives at both ends) arriving each way. Look
   for `DOCK MAP MISMATCH` in the log.
2. **Manual job.**
   - `/st platform supply Iron Plate` on a fed platform at station A.
   - `/st platform demand Iron Plate` on a platform at station B.
   - Drive a train, `/st train manage`, get out, `/st job Iron Plate from A to B`.

   The train should drive to A, load only iron plates, drive to B, unload, and then park (no depot) or go to a
   depot. Check the `WRITE`, `docked`, `left` and `DONE` lines.
3. **Stop rules.** At A, does the train leave once full or after 30 s? At B, once empty or after 120 s? Do platforms
   at A in unload mode leave it alone (unload filter none)?
4. **Re-docking at the same station.** Give a job whose pickup is the station the train stands at. Does it dock
   again at once?
5. **Unmanage.** `/st train unmanage`: is the old timetable back, with the autopilot as it was?

## M1: dispatching

1. Set two suppliers and one consumer for one item, and manage two trains. Wait for the rates to be measured (2
   minutes). Trains should pick up and deliver on their own (`JOB` lines), the consumer should stay near its
   target, and `/st status` should show the planned flow and trains needed.
2. Set a Critical consumer that runs empty: it should be served first, even with part loads.
3. Add a depot: idle trains should go there and park.
4. Without a depot, make a parked train stand in a station a job needs: it should move to a free station.
5. With Freight Car Control 1.0.4 or later: a station where only some cars fit should send the others as Skip
   (`skipped cars` in `WRITE`). Without it, the same job should be refused (`NO FIT` at VeryVerbose).
6. Observe only on: jobs are logged as `WOULD JOB`, trains untouched.
7. Save and load: jobs continue, learned trips are kept.

## M1b: migration

On a test copy of the user's network:

1. `/st migrate preview`.
   - Are the quartz shuttle's stations flagged "used only by dedicated trains"?
   - Are the pull-based trains ticked?
   - Is the CSV written?
2. Turn on Observe only, then `/st migrate apply all`, and watch the `WOULD JOB` lines.
3. Turn off Observe only and watch the network.
4. `/st migrate undo`: platforms and trains should be back as they were.

## M2: menus (0.2.0)

1. Start-up log: `4 of 4 UI hooks registered`.
2. Freight platform menu: the SmartTrains panel stands right of the window, inside it (check the geometry log line).
   - Set Supply, add an item through the picker (search works, items on the platform come first), step send-at,
     cycle priority (right click goes back), type a manual rate and press Enter.
   - The platform's load/unload switch follows the role. `/st platform` shows the same settings.
   - Copy, open another platform, Paste.
   - "Copy to the station's other platforms": every platform of that kind in the station gets these settings, once;
     editing one afterwards changes only that one.
   - The game's copy/paste-settings (keys or menu) on a platform also copies and pastes the SmartTrains settings: the
     log shows `Game clipboard: copy from ...`, the panel shows "Copied SmartTrains settings: ...". A fluid
     platform's settings don't paste onto a freight platform.
   - The readout updates every second: stock, rate, inbound trains, and a reason when nothing comes.
3. Fluid platform menu: same, with fluids only and m³/min.
4. Locomotive menu (0.4.0): the SmartTrains section is in the Timetable column under the stop list; the power and
   speed stats are clear. Turning Managed on locks "Edit timetable"; turning it off unlocks it and puts the old
   timetable back. A train with `/st train keepvanilla on` shows why it can't be managed.
5. Station menu: depot switch; no pool tag and no migration tool there.
6. `/st migrate`: the migration window opens over the game with the mouse free, and the player doesn't move while it
   is open. Preview, untick a row, Apply (it asks for a second click without a test copy), Undo. Save a test copy and
   a backup (beta tools on). Close and Escape both give the controls back.
7. Multiplayer, if possible: a client sees the same readouts, and the host logs its edits with its name.

## M4: platform-to-platform trips (0.4.0)

1. Station A supplies Copper Ingot on platform 1 and Wire on platform 2. Station B takes Copper Ingot on platform 1
   and Wire on platform 2. With FCC, one train with two cars carries both on one trip. The `JOB` line lists
   `A (+Copper Ingot +Wire) -> B (-Copper Ingot -Wire)`, and the locomotive menu shows per stop which car does what.
2. Two suppliers of one item that each hold about half a load, one consumer below target: one trip picks up at both
   (`A (+item) -> B (+item) -> C (-item)`). The first pickup is "load/unload once".
3. One load, two consumers that each have room for about half: one pickup, two drops.
4. Leftovers: let a consumer fill up during a delivery. The cars keep the rest; `REPLAN` shows it delivered to
   another consumer on the way, or `carries cargo no trip wants now`. After **Unwanted cargo after** minutes it goes
   to any consumer with room.
5. Backhaul: a station that takes one item and supplies another. A train delivering there also picks up there if it
   fits.
6. Without FCC: a station whose platforms are all the same works for every car. A station set platform by platform
   only gets trips where no car would move the wrong item; `/st jobs` and the platform menu explain the rest.
7. Push-pull train: the first visit each way logs the learned map (`Dock map`), and `DOCK MAP MISMATCH` if the
   prediction was wrong. The next trip uses it.
8. Mods menu "Most stops per trip" = 2: only direct trips.
9. Old save from 0.3.0 with a job running: the log says `Converted n job stops from an older save to per-car work`,
   and the job carries on.

## After 2026-11-05: push-pull, monitor, route inference

1. Water push-pull shuttles: both trains get trips again (`JOB ... Water SRC ... -> Water SNK ...`). The water
   platforms no longer say "no managed train fits" while a car holds water.
2. A train with no trip: its `IDLE` line lists reasons with counts (`... (4 times); ...`).
3. `/st monitor`: the window opens; after a minute items show verdicts. Starve a consumer by setting its supplier to
   Off: within a few minutes it shows "No supply" or "Supply short". `/st monitor text` prints the same in chat, and
   the log gets `MONITOR` lines every 5 minutes.
4. Migration on a station with an empty platform that no belt reaches: the preview fills it from a train's route
   ("inferred from ... route"), at medium confidence.
5. Idle trains are named "ST: parked at <station>".
6. A busy consumer with several trains heading for it: trains standing in stations show "holding at ..." in the
   monitor's Trains tab, then leave so they arrive a gap after the train before. Fewer Auto Trains DEADLOCK and
   "can't stop short" lines near that station.

## Automated tests

`D:\SFMod\Tools\BuildScripts	est-smarttrains.bat` builds the editor (FactoryEditor, Development) and runs the
`SmartTrains` automation tests (`Source/SmartTrains/Private/Tests`). The report is in
`SatisfactoryModLoader/Saved/Logs/SmartTrainsTests.log`. In the editor's console: `Automation RunTests SmartTrains`. The
tests are compiled only where `WITH_DEV_AUTOMATION_TESTS` is on, so not into the packaged mod. Their two item classes
(`USTTestSolid`, `USTTestFluid`) are abstract and hidden.

| Test | Checks |
|---|---|
| `SmartTrains.Planner.FirstFreeArrival` | Bookings with start and end and the arrival gap: a slot before, between or after bookings. |
| `SmartTrains.Planner.PushPullIdenticalPlatforms` | A push-pull train at four identical platforms in both ways round: no car skipped, each platform's stock and room counted once per way round. |
| `SmartTrains.Planner.WrongPlatformNeedsSkip` | A car beside a load-mode platform without a supply role: the trip is invalid without Freight Car Control, and the car skips with it. |
| `SmartTrains.Planner.Assignment` | The Hungarian assignment beats the greedy choice; rows worth nothing get nothing. |
| `SmartTrains.FlowPlan.MinCostFlow` | The flow plan fills the cheap route first and sends all the consumer takes. |

The editor's game classes are stubs: inventories report no slot sizes. The tests therefore give a fluid a 2,400 m3
stack to stand in for a platform or car. Fluid slot sizes themselves (the 50 m3 bug) can only be checked in game.
