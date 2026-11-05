# SmartTrains chat commands

All commands start with `/st` (or `/smarttrains`). They run on the host. "Look at" means aim at the platform, station
or train from up to 60 m away.

## Readouts

- `/st status`: managed trains, platform counts, Freight Car Control availability, and per item the planned flow,
  trains needed and trains that fit.
- `/st jobs`: each managed train's job, its current stop and ETA. In observe-only mode, idle trains show what they
  would do.
- `/st fit`: for the train you drive or look at, its consist and the dock map (which car stops beside which
  platform) at every station with platform roles, seen or predicted.

## Platforms (look at a freight or fluid platform)

- `/st platform`: shows the platform's role, its items with stock, rate and target, inbound trains and recent jobs.
- `/st platform supply Iron Plate`: picked up by managed trains. Several items: `supply Iron Plate, Screw`.
- `/st platform demand Coal target 60 priority high`: managed trains keep it at 60%.
- `/st platform demand any`: a sink that takes surplus of any item (lowest priority).
- Options:
  - `target <percent>` (demand);
  - `sendat <percent of a train load>` (supply; 0 = a full load);
  - `priority low|normal|high|critical`;
  - `rate <per minute>`, which replaces the measured rate (m³ for fluids).
- `/st platform off`: back to vanilla. The platform gets its own load/unload setting back.
- `/st platform copy`: copies this platform's settings to the other platforms of its station of the same kind.

Setting a role switches the platform to load (supply) or unload (demand).

## Stations (look at a station or one of its platforms)

- `/st station depot on|off`: idle managed trains park at the nearest depot.
- `/st station same on|off|auto`: whether editing one platform sets every platform of that kind in the station.
  Turning it on copies the platform you look at (if any) to the others. Auto: on without Freight Car Control while the
  platforms match.

## Trains (drive or look at a train)

- `/st train manage`: SmartTrains takes over its timetable. The old timetable and autopilot setting are saved.
- `/st train unmanage`: gives the saved timetable and autopilot back.
- `/st train keepvanilla on|off`: never managed or proposed by the migration tool.
- `/st job <item> from <station> to <station>`: a test job for a managed train, using the cars that fit.

## Migration (see Migration.md)

- `/st migrate`: opens the migration window (preview with tickable rows, apply, undo, test copy, backup). From the
  server console it runs the preview instead.
- `/st migrate preview [tracelimit <n>] [allitems]`: proposes (one item per platform unless `allitems`) roles and managed trains and writes a CSV report to the game's log folder.
- `/st migrate apply all [confirm]`: applies every unflagged station and every ticked train.
- `/st migrate apply stations 1,2 trains 3`: applies chosen ones by their preview numbers.
- `/st migrate undo`: puts the last apply back.

Apply asks for `confirm` unless `/st testcopy` or `/st backup` ran in this session.

## Beta tools (Mods menu: Beta testing tools)

- `/st testcopy [session name]`: saves this world as a separate session, by default "<session> (SmartTrains test)".
  Load it from the main menu to test. The world you're in keeps saving as its own session.
- `/st backup`: a timestamped save in the current session.
