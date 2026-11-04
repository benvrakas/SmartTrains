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
   - Copy, open another platform, Paste. "To station" fills the station's other freight platforms.
   - The readout updates every second: stock, rate, inbound trains, and a reason when nothing comes.
3. Fluid platform menu: same, with fluids only and m³/min.
4. Locomotive menu: turning Managed on locks "Edit timetable"; turning it off unlocks it and puts the old timetable
   back. With Keep vanilla on, the train can't be managed. The pool tag saves on Enter.
5. Station menu: depot toggle and pool tag. In the Migration tool: Preview, untick a row, Apply (it asks for a second
   click without a test copy), Undo. Save a test copy and a backup (beta tools on).
6. Multiplayer, if possible: a client sees the same readouts, and the host logs its edits with its name.
