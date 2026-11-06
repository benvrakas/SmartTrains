# Changelog

## 0.6.4 (beta)

- **Fixed: a supply platform only some trains can reach was left full.** Pickups promised to trains counted against
  the whole station, so trains loading from platforms 1-3 also used up platform 4's share. A train whose car beside
  platform 4 could load then found nothing left, and that car skipped the stop. Seen at the sulfur stations, where most
  trains' 4th car is a fluid car. Promised pickups now count against the platform they're planned at.
- **Repeats run the whole trip:** a train repeats the last trip it was given that picks up and delivers, as planned.
  It no longer parks because its last job ended early, or was only a delivery of leftovers.
- **Log:**
  - When a stop moves none of what was planned (`NOPROGRESS`), the log lists every platform of that station: load or
    unload, role, slots used and what it holds, so you can see why.
  - Self-driving errors are logged only once they last 5 s. Short "No Path" blips as trains re-route on leaving a
    station no longer fill the log.
- **Monitor:** trains repeating their last job count as free, not busy ("repeating their last job (free for new
  trips)", tile "Free now"). Arrivals on repeats no longer make stations look hours late.

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.6.4.

## 0.6.3 (beta)

- **Fixed: a train docked at the same platform over and over.** A train that finished at a consumer set to keep 100%
  still held some cargo, and was sent straight back to dock there again, blocking the trains behind it. A trip no
  longer starts at the station the train left in the last minute; its leftovers go elsewhere.
- **Fixed: a trip's first stop was sometimes skipped** ("legs were missed" in the log). A timetable written as the
  train pulled out could be moved on by the game's departure right after. For 10 s after a write SmartTrains now puts
  the current stop back (`TIMETABLE ... set back to stop ...`).
- **Trains leave stations at once** (new setting, on by default). Every stop is load/unload once: a train moves what it
  can in one transfer and leaves. A train with a trip starts it straight away, instead of holding in the station while
  its next stop is busy or Auto Trains sees no way yet.
- **No more parking** (new setting **Repeat the last job instead of parking**, on by default). A train with no new trip
  runs its last job again, like a timetable loop. It takes a new trip as soon as one is worth it. Only a train that
  never had a job yet parks.
- **Fixed: trains stayed docked for minutes after their work was done.** A train that had unloaded what the platform
  had room for waited out the whole "Delivery: longest wait" and more. Meanwhile it blocked the station for trains
  behind it, and you couldn't edit it or its platforms.
  - A stay now ends as soon as every car has moved what was planned for that stop.
  - Otherwise it ends when nothing has moved for 20 s.
  - Cargo a car keeps for a later stop no longer holds up "fully load/unload".
  - Shorter stays also make SmartTrains' trip time estimates realistic again, so fewer trips are turned down as too
    long.
- **`/st emptycars`** (admin): deletes all cargo in the train you drive or look at; `/st emptycars all confirm` empties
  every managed train. Useful for cars left with mixed or unwanted cargo.
- Detailed log: each free train's best moves and their scores, and why deliveries to High and Critical consumers
  score nothing by themselves (`PLANNER <train>: best moves alone: ...`).
- **Migration shares items out between platforms.** Some platforms' belts lead to machines that use several items,
  e.g. Copper Ingot and Caterium Ingot for one cable factory, and nothing they hold decides between them. Those
  platforms now get different items instead of the same one. Platforms count together when their belts reach the same
  machines, also at different stations. A platform's own stock still decides first. Changed platforms say "check it".

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.6.3.

## 0.6.2 (beta)

- **Fixed: empty platforms could get a role.** Modular Stations' empty platforms can't move freight or fluid. They
  now show as "Empty platform" in the migration window, can't be set there, and are refused a role everywhere else:
  the platform menu, pasted settings, `/st platform` and the copy to the station's other platforms. Freight and fluid
  platforms with nothing attached yet can still be set, so a station can be migrated before it's connected to the
  factory.

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.6.2.

## 0.6.1 (beta)

- **Fixed: cars came out with mixed cargo.** A car holding one item could load another into its free slots, e.g. a
  stack of concrete beside aluminium ingots. It would then have unloaded both wherever the concrete was going, and
  clogged that factory.
  - A car holding cargo now only tops up the item it holds.
  - A car only stops at an unloading platform that takes everything it holds.
  - A platform with one item only gives that item.
  - Cars only come out mixed from a platform with several items selected.
- Cars that are already mixed are skipped at stops that would take only part of their cargo. Empty them by hand, or
  let them go to a sink (a demand platform set to any item).

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.6.1.

## 0.6.0 (beta)

- **Set platforms by hand in the migration window:** in the Stations tab, click a station to open its platforms.
  Click a platform's role to change it, or `[items]` to pick its items from a searchable list. Applying the station
  uses your setting, and a new Preview keeps it.

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.6.0.

## 0.5.0 (beta)

- **Admin-gated controls** for multiplayer and dedicated servers:
  - Always admin only: changing settings, migration apply and undo, test copies and backups. An admin is the host,
    or on a dedicated server a player signed in with the admin password.
  - New setting **Only admins change SmartTrains** (off by default): only admins can then change platforms, stations
    and trains.
  - Refused changes are answered in chat or under the menu, and logged with the player's name.
- **Server settings in game:** `/st settings` lists the server's settings, and `/st set <setting> <value>` changes one
  (admins). On a dedicated server your own Mods menu doesn't reach the server's settings.
- **Fixed: the migration window's Preview did nothing on a dedicated server.** The preview of a large network was too
  big for one network message, so the server's answer never arrived. It now comes in parts. The monitor window is
  kept under the same limit on very large networks.

**Good to know**

- Everyone in a multiplayer session needs SmartTrains 0.5.0.

## 0.4.0 (beta)

First release. Trains without fixed timetables: give platforms a role and items, and managed trains move them from
supply to demand.

- **Platform settings:** freight and fluid platforms get a role (Supply, Demand or Off) and their items, each with a
  priority, a target and a minimum batch. "Open SmartTrains settings" beside the platform menu opens them; the
  section there shows the platform's items with their icons. The game's copy and paste settings carries them too.
- **Managed trains:** switch **Managed by SmartTrains** on in the locomotive menu. SmartTrains writes the train's
  timetable for each trip, and the game (or Auto Trains) drives it. Switching it off brings the old timetable back.
- **Trips planned car by car:** one trip can visit up to 4 stations and serve several platforms and items. Train
  length, car order and platform order are checked for every stop. Trains with a locomotive at each end are handled
  both ways round. After every stop the rest of the trip is planned again from what the cars really hold.
- **Who gets served:** trips are chosen by stock, production rate, distance and how long each consumer has waited.
  Free trains are matched to trips by an exact assignment.
- **Traffic:**
  - Trains are spaced at stations (**Gap between trains at a station**, 30 s by default).
  - A train whose next station is busy waits where it is instead of queueing on the line.
  - Parked trains move to a depot or a free station when a trip needs their station.
  - In a dead-end station, a parked train keeps the station until it leaves.
- **Fluids:** fluid cars and platforms use their real capacity. Fluid deliveries unload once instead of waiting out
  the stop's timeout.
- **`/st monitor`:** a window with Overview, Items, Stations, Platforms and Trains tabs. Per item it shows what limits
  deliveries: supply, trains, fit or a congested station, each with advice.
- **`/st migrate`:** reads your network and proposes roles, items and managed trains, with preview, apply and undo.
  It traces belts and pipes back to the machines behind each platform. It also reads DynamicTimeTable's station
  groups.
- **Beta tools:** `/st testcopy` saves your game as a separate session to try SmartTrains on. `/st backup` makes a
  backup save.
- **Commands:** `/st help`, `/st status`, `/st jobs` and `/st fit`.

**Highly recommended**

- **[Auto Trains](https://ficsit.app/mod/AutoTrains)** 1.3.9 or later. Before a train leaves a station, SmartTrains
  asks Auto Trains whether the way is clear, including trains coming the other way on a single track. Its station
  queues and travel times feed into the trip timing.
- **[Freight Car Control](https://ficsit.app/mod/FreightCarControl)** 1.1.0 or later. Cars can skip a stop, so a
  train can stop where only some of its cars have work. Without it, every car must fit the platform it stops beside.

**Good to know**

- Beta: try it on a copy of your save first (`/st testcopy`).
- Don't keep DynamicTimeTable installed with managed trains. Both mods would write the same timetables.
- Everyone in a multiplayer session needs SmartTrains 0.4.0.
