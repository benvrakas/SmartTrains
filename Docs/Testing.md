# Tests

Test builds go on GitHub pre-releases and are installed with SDMM. Use a test copy (`/st testcopy`) or a creative
world. Judge a run by the rules the mod keeps (Architecture.md, "Loops"), not by how much it moved.

## In game

### L1: dock maps

Drive a train with 1 locomotive and 3 freight cars to a station with 4 platforms. Run `/st fit`: it lists the
consumers its cars line up with. Let it dock: the log has a `Dock map` line and no `DOCK MAP MISMATCH`. Repeat with a
push-pull train (locomotives at both ends) arriving each way.

### L2: one loop

1. A supplier station A with a fed platform `/st platform supply Iron Plate` and a consumer B with
   `/st platform demand Iron Plate`; set B up from its factory (station menu) so its rate is traced (`FACTORY` line).
2. Manage a train with a freight car beside B's platform. Within 5 s: a `SPLIT` line, `LOOP <train>: serves B`, a
   `JOB ... loop` and a `WRITE` line with both stops.
3. The train fills at A until full, unloads at B until empty, and starts again on its own (`LOOP <train>: round N
   done`). Its timetable is written once.
4. Save and load: the loop goes on.

### L3: the split

1. Two consumers whose factories use 1x and 2x the same rate, about as far from their suppliers, and three managed
   trains that fit both: the `SPLIT` line gives them 1 and 2.
2. Make one consumer twice as far: its share grows.
3. Unmanage trains until there are fewer than consumers: the smallest consumer shows as unserved (monitor
   Consumers tab, red).
4. Nothing changes: no new `SPLIT` line, no train changes consumer (learned travel times don't move trains).

### L4: switching patterns

1. Give a loaded train another consumer (change a pattern, or unmanage the trains of another consumer): it first
   delivers at its old consumer (`switching patterns ... deliver`), then goes to a Sink platform if cargo is left,
   then starts the new loop empty.
2. Without a Sink platform: a warning, and it starts its loop with the cargo.

### L5: migration

On a test copy of a network:
1. `/st migrate preview`: the CSV is written; stations and trains are proposed.
2. Turn on Observe only, `/st migrate apply all`, and watch the `WOULD JOB` lines.
3. Turn off Observe only and watch the network.
4. `/st migrate undo`: platforms and trains are back as they were.

### L6: menus

1. Start-up log: `4 of 4 UI hooks registered`.
2. Freight platform menu: the SmartTrains panel opens beside the window.
   - Set Supply, add an item through the picker (search works, items on the platform come first).
   - The platform's load/unload switch follows the role. `/st platform` shows the same settings.
   - Copy, open another platform, Paste; "Copy to the station's other platforms" gives every platform of that kind
     in the station these settings, once.
   - The game's copy/paste-settings on a platform also copies and pastes the SmartTrains settings
     (`Game clipboard: copy from ...`); a fluid platform's settings don't paste onto a freight platform.
   - The readout updates every second: stock and the traced rate.
3. Fluid platform menu: the same, with fluids only.
4. Locomotive menu: the SmartTrains section is in the Timetable column under the stop list. Managed on locks "Edit
   timetable"; off unlocks it and puts the old timetable back. Hold / Resume works. A train with
   `/st train keepvanilla on` shows why it can't be managed.
5. Station menu: the set-up bar under the name; the SmartTrains panel shows the layout and the station's loop.
6. `/st migrate` and `/st monitor`: the windows open with the mouse free; Close and Escape give the controls back.
7. Multiplayer, if possible: a client sees the same readouts, and the host logs its edits with its name.

## Test saves

Test runs start from one save each series, so runs compare like with like (`D:\SFMod\TestEnv\LinuxServer\TESTING.md`).
To make a starting save: load it, run `SmartTrains.EmptyCars` (managed trains' cargo) and
`SmartTrains.EmptyPlatforms` (every freight and fluid platform) on the server console, then save it under a new name.
`SmartTrains.EmptyPlatforms supply` empties only Supply platforms: consumers keep their stock, so the factories run on
and draw on it from the start.

## Automated tests

`D:\SFMod\Tools\BuildScripts\sfmod test SmartTrains` builds the editor (FactoryEditor, Development), runs the
`SmartTrains` automation tests and prints a pass/fail summary (`Source/SmartTrains/Private/Tests`). The report is in
`SatisfactoryModLoader/Saved/Logs/SmartTrainsTests.log`. In the editor's console: `Automation RunTests SmartTrains`. The
tests are compiled only where `WITH_DEV_AUTOMATION_TESTS` is on, so not into the packaged mod. Their item classes
(`USTTestSolid`, `USTTestSolidB`, `USTTestFluid`) are abstract and hidden.

| Test | Checks |
|---|---|
| `SmartTrains.Loops.Split` | Trains split in proportion to need by largest remainder; every train is given out; with enough trains even a tiny need gets one. |
| `SmartTrains.Loops.Unserved` | With fewer trains than consumers the largest needs get one each, ties to the lower index; the rest are unserved. |
| `SmartTrains.Loops.Suppliers` | An item comes from the nearest supplier with spare rate, then the next nearest; an unknown rate takes all that is left; what nobody has goes to the nearest. |
| `SmartTrains.Setup.ShareOutByRate` | Platforms feeding the same machines share items by car loads per minute; a set-up platform counts and is left alone. |
| `SmartTrains.Setup.ShareOutLanes` | Platforms reaching different inputs of the same machines get different items: the most used item where the most belts go. |
| `SmartTrains.Setup.BeltMaxFlow` | The belt network's maximum flow and its narrowest cut (two belts after a merge). |
| `SmartTrains.Setup.Format` | Station names fill the format, drop empty values with their brackets, format rates like the game. |
| `SmartTrains.Setup.Number` | A station keeps its number; a new one takes the smallest free number. |

Not covered by an automated test (they need a live world): the fit of a train's cars, the loop's stops, the
rebalancing and switching patterns. Rates and products: check them with `SmartTrains.Setup` on a test server and
`D:\SFMod\TestEnv\compare-setup-names.py`, which compares the report with rates players wrote into station names.

The editor's game classes are stubs: inventories report no slot sizes. Fluid slot sizes can only be checked in game.
