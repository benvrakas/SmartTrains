# SmartTrains chat commands

All commands start with `/st` (or `/smarttrains`). They run on the host. "Look at" means aim at the platform, station
or train from up to 60 m away.

Who may use which command is in [Admin-only commands](#admin-only-commands) below.

## Readouts

- `/st status`: platform counts, Freight Car Control availability, and the loop plan's fleet lines.
- `/st jobs`: each managed train's loop (or migration step, or why it has none) and its stops.
- `/st fit`: for the train you drive or look at, which consumers' patterns its cars line up with, and why not the
  others.
- `/st patterns`: every consumer's pattern (the item for the car beside each unloading platform) and the trains on its
  loop.
- `/st monitor`: opens the monitor: consumers (pattern, rate, round trip, share, trains), suppliers (rate, taken by
  loops) and trains (state, consumer, what it does). `/st monitor text` prints it in chat; from the server console it
  always prints.

## Platforms (look at a freight or fluid platform)

- `/st platform`: shows the platform's role, its items with stock and traced rate, the next train and recent stops.
- `/st platform supply Iron Plate`: picked up by managed trains. Several items: `supply Iron Plate, Screw`.
- `/st platform demand Coal`: managed trains deliver it here. Several items make an item group: the car beside the
  platform carries one of them.
- `/st platform sink`: trains switching patterns unload cargo here that their old consumer had no room for.
- `/st platform off`: back to vanilla. The platform gets its own load/unload setting back.
- `/st platform copy`: copies this platform's settings to the other platforms of its station of the same kind.

Setting a role switches the platform to load (supply) or unload (demand and sink).

## Stations (look at a station or one of its platforms)

- `/st station`: its platform layout and, for a consumer, its loop (rate, round trip, trains).

## Station set-up

- `/st setup`: what the set-up would do for the station you look at: a role and an item per platform from the
  machines its belts and pipes reach, and a name from the factory, the map area and the most per minute.
- `/st setup apply`: sets it up like that. `/st setup apply keepname` sets the platforms and keeps the name.
- `/st setup all`: the same for every station, as a report in the server's `Saved/SmartTrains/StationSetup-*.csv`
  and the log (`SETUP` lines). `/st setup all apply [keepname]` applies it everywhere (admin only).
- `/st setup format <text>`: how names are built, per world. `{area}`, `{product}`, `{role}`, `{n}` and `{rate}` are
  filled in; empty ones drop out with their brackets. Default: `{area} {product} {role} {n} ({rate})`.
- `/st setup words <loads> <unloads several items> <unloads one item> [both] [sink]`: the words for `{role}` (`-`
  for none), and the `{product}` of a station whose cargo platforms are all Sinks (`-` keeps its name). Default
  `Out In In`, no word for a station that does both, `Sink`. Example: `/st setup words SRC RSC SNK - SINK`.
- `/st setup default`: the default format and words again.
- Server console: `SmartTrains.Setup [apply] [keepname]` runs `/st setup all`. `SmartTrains.MigratePreview` runs the
  migration preview.
- `/st setup all` and the migration preview's station names are worked out a few stations at a time (about 15 ms
  every 0.05 s), so the server keeps running; the report and the names follow when they are done.
- Belts and pipes are only traced when you press Autodetect, run `/st setup` or the migration. Press Autodetect again
  after rebuilding a factory, so its recipe rates follow.
- Server console: `SmartTrains.EmptyCars` deletes the cargo of every managed train, as `/st emptycars all confirm`
  (no undo; for tests that start from empty trains on a dedicated server).

## Trains (drive or look at a train)

- `/st train manage`: SmartTrains takes over its timetable. The old timetable and autopilot setting are saved.
- `/st train unmanage`: gives the saved timetable and autopilot back.
- `/st train keepvanilla on|off`: never managed or proposed by the migration tool.
- `/st train hold` / `resume`: stops the managed train (autopilot off, no loop) until resume, which carries on with
  its job.
- `/st emptycars`: deletes all cargo in the freight cars of the train you drive or look at (admin only, no undo).
  `/st emptycars all confirm` does it for every managed train. Logged as `EMPTY <train> by <player>: removed ...`.

## Migration (see Migration.md)

- `/st migrate`: opens the migration window (preview with tickable rows, apply, undo, test copy, backup). From the
  server console it runs the preview instead.
- `/st migrate preview`: proposes every station's platforms (as Autodetect does), names and the trains to manage, and writes a CSV report to the game's log folder.
- `/st migrate apply all [rename | namesonly] [confirm]`: applies every unflagged station and every ticked train. Without
  either word it sets the platforms only; `rename` also renames the stations; `namesonly` renames them and leaves the
  platforms as they are.
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
- `/st set <setting> <value>`: changes one, e.g. `/st set AverageSpeedKmh 100` or `/st set DetailedLog on`. The
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
  station and train changes, in chat and in the menus, including pasted settings.
- **Open to everyone:** readouts (`status`, `jobs`, `fit`, `patterns`, `monitor`, `settings`), `/st migrate preview` and copying a
  platform's settings.

A refused change is answered in chat or under the menu, and logged as `ACCESS refused: <player> (<level>) tried to
...`.
