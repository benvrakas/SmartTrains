# SmartTrains chat commands

All commands start with `/st` (or `/smarttrains`). They run on the host. "Look at" means aim at the platform, station
or train from up to 60 m away.

Who may use which command is in [Admin-only commands](#admin-only-commands) below.

## Readouts

- `/st status`: managed trains, platform counts, Freight Car Control availability, and per item the planned flow,
  trains needed and trains that fit.
- `/st jobs`: each managed train's job, its current stop and ETA. In observe-only mode, idle trains show what they
  would do.
- `/st fit`: for the train you drive or look at, its consist and the dock map (which car stops beside which
  platform) at every station with platform roles, seen or predicted.
- `/st monitor`: opens the throughput monitor. For each item it shows what limits deliveries over the last half hour:
  - not enough supply;
  - not enough trains;
  - no train that fits;
  - a congested station.

  It also lists starved and backed-up platforms, and why idle trains wait. `/st monitor text` prints it in chat;
  from the server console it always prints.

## Platforms (look at a freight or fluid platform)

- `/st platform`: shows the platform's role, its items with stock, rate and target, inbound trains and recent jobs.
- `/st platform supply Iron Plate`: picked up by managed trains. Several items: `supply Iron Plate, Screw`.
- `/st platform demand Coal target 60 priority high`: managed trains keep it at 60%.
- `/st platform demand any`: a sink that takes surplus of any item (lowest priority).
- Options:
  - `target <percent>` (demand);
  - `sendat <percent of a train load>` (supply; 0 = the default, 50 %);
  - `priority low|normal|high|critical`;
  - `rate <per minute>`, which replaces the measured rate (m³ for fluids).
- `/st platform off`: back to vanilla. The platform gets its own load/unload setting back.
- `/st platform copy`: copies this platform's settings to the other platforms of its station of the same kind.

Setting a role switches the platform to load (supply) or unload (demand).

## Stations (look at a station or one of its platforms)

- `/st station depot on|off`: idle managed trains park at the nearest depot.

## Station set-up

- `/st setup`: what the set-up would do for the station you look at: a role and an item per platform from the
  machines its belts and pipes reach, and a name from the factory, the map area and the most per minute.
- `/st setup apply`: sets it up like that. `/st setup apply keepname` sets the platforms and keeps the name.
- `/st setup all`: the same for every station, as a report in the server's `Saved/SmartTrains/StationSetup-*.csv`
  and the log (`SETUP` lines). `/st setup all apply [keepname]` applies it everywhere (admin only).
- `/st setup format <text>`: how names are built, per world. `{area}`, `{product}`, `{role}`, `{n}` and `{rate}` are
  filled in; empty ones drop out with their brackets. Default: `{area} {product} {role} {n} ({rate})`.
- `/st setup words <loads> <unloads several items> <unloads one item> [both]`: the words for `{role}` (`-` for
  none). Default `Out In In`, no word for a station that does both. Example: `/st setup words SRC RSC SNK`.
- `/st setup default`: the default format and words again.
- Server console: `SmartTrains.Setup [apply] [keepname]` runs `/st setup all`.
- Server console: `SmartTrains.EmptyCars` deletes the cargo of every managed train, as `/st emptycars all confirm`
  (no undo; for tests that start from empty trains on a dedicated server).

## Trains (drive or look at a train)

- `/st train manage`: SmartTrains takes over its timetable. The old timetable and autopilot setting are saved.
- `/st train unmanage`: gives the saved timetable and autopilot back.
- `/st train keepvanilla on|off`: never managed or proposed by the migration tool.
- `/st job <item> from <station> to <station>`: a test job for a managed train, using the cars that fit.
- `/st emptycars`: deletes all cargo in the freight cars of the train you drive or look at (admin only, no undo).
  `/st emptycars all confirm` does it for every managed train. A managed train's trip is planned again. Logged as
  `EMPTY <train> by <player>: removed ...`.

## Item groups

- `/st group`: every item group: home trains and trains needed, trains working on it now, routes, and `[short N]` when
  it lacks trains while every train that fits it is busy.
- `/st group <item>`: one group: its routes (supplier, consumer, rate, trains needed) and what each home train does.
- `/st group <item> trains <least> <most>`: limits on its home trains (`0 0` = automatic). Saved per item.
- `/st train stay on|off`: the train you drive or look at keeps its home group (the share-out doesn't move it).

## Snapshots

- `/st snapshot` (admin): the next dispatch round writes every free train's planning inputs (nodes, bookings, travel
  times, cars, platforms, dock maps) to `Saved/SmartTrains/Snapshots/snapshot-<time>.json` on the server (log line
  `SNAPSHOT`). Copy it to `Mods/SmartTrains/Tests/Snapshots` and the automated test `SmartTrains.Replay.Snapshots`
  plans those trains again offline.

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

## Settings

- `/st settings`: the host's or dedicated server's settings with their keys and ranges. Settings changed from their
  default are marked `*`. It also says whether you count as an admin.
- `/st set <setting> <value>`: changes one, e.g. `/st set ArrivalGapSeconds 45` or `/st set DetailedLog on`. The
  key's case doesn't matter, and a unique part of a key or name works. It takes effect within a second and is saved
  to the server's `Configs/SmartTrains.cfg`.

On a dedicated server, this is the way to change settings in game. Your own Mods menu only changes your copy,
which the server doesn't read.

## Admin-only commands

An admin is the host, or on a dedicated server a player whose privilege level is Administrator: one who signed in
to the server with the admin password in the Server Manager. The server console counts as an admin.

- **Always admin only:**
  - `/st set`;
  - `/st emptycars`;
  - `/st migrate apply` and `undo`, and Apply and Undo in the migration window;
  - `/st testcopy` and `/st backup`, also from the migration window.
- **Admin only when "Only admins change SmartTrains" is on** (`/st set AdminsOnly on`, off by default): platform,
  station and train changes, in chat and in the menus, including pasted settings and `/st job`.
- **Open to everyone:** readouts (`status`, `jobs`, `fit`, `monitor`, `settings`), `/st migrate preview` and copying a
  platform's settings.

A refused change is answered in chat or under the menu, and logged as `ACCESS refused: <player> (<level>) tried to
...`.
