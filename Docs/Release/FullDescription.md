## SmartTrains

Trains that serve your factories on their own. Mark which platforms supply and which unload, and SmartTrains gives every
consumer station trains that carry its car pattern, looping between its suppliers and it. How many trains each consumer
gets follows how much it needs compared with the rest of your network. You don't plan a train per factory.

**Beta.** Try it on a copy of your save first (`/st testcopy`).

**Highly recommended:** [Auto Trains](https://ficsit.app/mod/AutoTrains) and
[Freight Car Control](https://ficsit.app/mod/FreightCarControl). SmartTrains works without them (see below).

### How it works

- **Platforms:** open a freight or fluid platform: the SmartTrains panel beside it shows its role, items, rate and the
  next train; **Settings** opens the editor.
  - **Supply:** managed trains pick items up here.
  - **Demand:** managed trains deliver here. Several items on one platform make an item group: the car beside it
    carries one of them.
  - **Sink:** a train switching to another station's loop unloads cargo here that its old station had no room for.
  - **Off:** vanilla.
  - **Copy**, **Paste** and **Copy to the station's other platforms** carry the platform's settings.
- **Set up a station from its factory:** build the station and its platforms, connect the belts and pipes, and open
  the station menu. **Autodetect** (per platform, or **Autodetect all platforms** under the station's name) follows
  the belts and pipes to the machines and sets each platform's role and item; the item a platform holds wins.
  **Rename station from its factory** names it from what the factory makes, its map area, a number and its most per
  minute, e.g. "Grass Fields Modular Frame Out 1 (288)". Each shows what it would do first; click again to do it.
  `/st setup all` does every station; the name format and words are yours (`/st setup format`, `/st setup words`).
- **Trains:** in the locomotive menu, switch **Managed** on. SmartTrains writes its timetable; the game, or Auto
  Trains, drives it. The menu shows its pattern as car tiles with fill bars, its loop's stops and how many trains share
  its pattern; while you ride it, the same shows as a card at the top right of the screen.
- **Depot Signals** (build menu, next to the Block Signal): when a factory runs well below its most (machines on
  standby, without power or backed up), its spare trains wait at the nearest free depot signal's passing siding and go
  back when it picks up. Without one they keep circulating.
- **Car patterns:** a consumer station's unloading platforms, in order from the station, make its pattern: car 1 iron,
  car 2 wire, and so on. Every train serving that station carries exactly that pattern, so a car only ever carries
  its one item (or one of its group). A train only serves stations its cars line up with.
- **Loops:** each train serving a consumer runs one loop, over and over: it loads what each supplier of its pattern has,
  then unloads what fits at the consumer, and goes round again. It never waits at a platform for more cargo or room,
  so it doesn't hold up the trains behind it; partly filled cars just mean another round. Its timetable repeats by
  itself. The Mods menu setting **Wait for full cars** (off by default) makes it wait at suppliers until its cars are
  full and at the consumer until they are empty.
- **How many trains each consumer gets:** what its factory uses at most, its theoretical rate from the belts and
  machines behind it (the same scan that names stations), in car loads per minute, times how long its loop takes.
  Every managed train is shared out over the consumers in that proportion: a consumer using twice as much, or twice as
  far from its suppliers, gets twice the trains. Because the rate is the factory's most, not what it happens to use
  right now, a factory starting up doesn't send trains back and forth.
- **Suppliers:** each item comes from the nearest supplier with rate to spare, then the next nearest for the rest;
  a supplier's most rate is shared out over the consumers that use it.
- **Fewer trains than consumers:** the consumers with the smallest needs get none; the monitor lists them as
  unserved.
- **Changes are slow and clean:** trains are only shared out again when a factory's rate, a station's pattern or the
  set of managed trains changes, and then as few trains as possible change station. A train switching station first
  delivers what it holds to its old station, takes what that station had no room for to a Sink platform, and only
  then starts its new loop, empty.

### Highly recommended mods

Both are optional: SmartTrains finds them while the game runs and uses them when they're there.

**[Auto Trains](https://ficsit.app/mod/AutoTrains)** drives the trains without signals. SmartTrains uses its travel
times for each loop's round trip and tells it when a timetable changes. Without it, the game's own self-driving and
your signals keep trains apart, as in vanilla: you need a properly signalled network.

**[Freight Car Control](https://ficsit.app/mod/FreightCarControl)** (1.1.0 or later) lets each car skip a stop: at a
supplier only the cars loading its item stop at its platforms, so no car picks up an item it doesn't carry. Without
it, every car loads or unloads at the platform it stops beside, as in vanilla, so loops work best where each
supplier's platforms line up with the cars that load there.

### Tools

- **Migration** (`/st migrate`): sets up a whole network at once, with preview, apply and undo: every station's
  platforms as Autodetect would, optionally every station's name, and which trains SmartTrains manages.
- **Monitor** (`/st monitor`): tiles, then Overview, Consumers, Suppliers and Trains tabs. Each consumer's pattern as
  icons, a pip per train against its share, its rate and round trip; each supplier's rate and what the loops take of
  it; each train's loop, or why it has none. **More** on a row opens its details.
- **Depots** (`/st depots`): each depot signal's spot, or why it can't be used, and the train parked there.
- **Status, jobs, fit, patterns** (`/st status`, `/st jobs`, `/st fit`, `/st patterns`): the loop plan in chat, each
  train's loop, which consumers a train fits, and every consumer's pattern with its trains.
- **Empty cars** (`/st emptycars`, admin): deletes the cargo of the train you drive or look at;
  `/st emptycars all confirm` empties every managed train.
- **Hold / Resume** in a managed train's locomotive menu: a held train stops and gets no loop until Resume.
- **Manage new trains** (Mods menu, off by default): new trains with freight cars and no timetable are managed 30 s
  after you last added a car.

### Works with

- **Auto Trains** (highly recommended).
- **Freight Car Control** (highly recommended).
- **Modular Stations.**
- **DynamicTimeTable, Dynamic Train Routes, RailDispatch**: they also steer trains, so don't keep them installed
  with managed trains. SmartTrains names any it finds in chat after loading.

### Updating from 0.43 or earlier

0.50.0 replaces trip-by-trip dispatching with loops. Loading an older save drops the trips trains were on; each
managed train then gets its loop. Platform roles and items carry over; the removed settings (targets, least per visit,
priority, manual rates) are ignored. Depot Signals (0.52.0) now only hold spare trains of a factory running below
its most.

### Multiplayer

All settings live on the host. Every player's menus show the host's state, and edits are logged with the player's
name.

- **Server settings in game:** on a dedicated server, an admin's changes in their own Mods menu go to the server.
  `/st settings` lists the server's settings and `/st set <setting> <value>` changes one.
- **Always admin only:** settings, migration apply and undo, test copies and backups. An admin is the host, or a
  player signed in with the server's admin password.
- **Optional:** with **Only admins change SmartTrains** on, only admins can change platforms, stations and trains.
