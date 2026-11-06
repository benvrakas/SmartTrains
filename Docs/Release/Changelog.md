# Changelog

## 0.6.6 (beta)

- **Repeats are planned, not copied.** A train with no new trip still runs its last trip again instead of parking,
  but the repeat is now simulated like a new trip, from what its cars hold and what the stations have now:
  - stops that would move nothing are left out (an empty supply, a full consumer), and amounts are real, not copied
    from the old plan. Repeats used to run with empty cars: one delivered 213 of 7.2 million litres planned;
  - a repeat isn't run if another train already runs the same stations. Several trains looped on the nitrogen
    route at once and filled its stations;
  - repeats don't book stations, so new trips are no longer turned away because of them;
  - a repeat is looked at again at every stop it leaves, so the train takes a new trip as soon as there is one.
  When the repeat has nothing worth doing, a train goes to a depot if there is one (the monitor says "Going to
  wait") and tries again from there every 30 s. With no depot, it keeps running its last trip as it was until it
  gets a new one (it takes a new trip at any time, and is looked at again at every stop), so it never stands still on
  a line. It no longer parks because other trains head for its stations. `REPEAT` log lines
  say why a planned repeat isn't run.
- **Monitor: "Low supply".** An item whose demand platforms are starved and whose supply takes over 30 minutes to fill
  one freight car is "Low supply", with how long a car takes. A trip for it carries a nearly empty car, so trains
  rarely go. It used to show "No trips planned", since starved platforms use nothing and their demand can't be
  measured.
- **Monitor:** a starved platform whose item is fine elsewhere now says so ("other demand platforms for it are fine;
  this one gets too few trips") instead of pointing at the item's "OK".
- **Log:** `PLAYER` lines show when a player sits in a managed train's driver seat: with autopilot on it keeps getting
  trips; driving it by hand (autopilot off) holds it until autopilot is on again.

## 0.6.5 (beta)

- **Fixed: fluid trips beat everything else.** Deliveries were valued by amount, and fluids count in litres, so a
  car of fluid was worth about 500 times a car of sulfur. Trains kept choosing fluid trips, and starved items stayed
  starved even at Critical priority. A delivery's value now counts only how full it makes the car, solid or fluid:
  100 sulfur is worth 500 concrete, and a full car is worth the same whatever it carries. High-stack items like
  concrete no longer outscore low-stack ones either.
- **Fixed: wrong car capacity for cars that last carried fluid.** Such a car counted as holding 1,600,000 of a solid,
  so the monitor showed "needs about 0.0 trains", and the planner could plan more cargo into a car than it holds. A
  car's capacity now comes from its wagon type. The log shows the size it reads once ("Freight car size").
- **Fixed: trips to unload cargo that was already unloaded.** The next trip of a docked train was planned from the
  cargo on board while it was still unloading, so it could drive to another station with an empty car, dock for 0 s
  and leave. Platforms keep loading and unloading while a train is docked, so what a stop moved is only known once
  the train leaves. A train whose last stop moves cargo now gets its next trip as it leaves, planned from what its
  cars really hold.
- **No more top-up trips to full platforms.** A train doesn't fill part-full stacks, and the stay ends at once when a
  platform has no room the moment the train docks. A car now unloads at least 2 slots' worth of an item (or all it
  holds of it), into free slots only. Small deliveries to full platforms made up most of the 0-second stops.
- **Fixed: a train with a player in it got no new trips.** A player sitting in the locomotive now only holds the
  train while driving it by hand (autopilot off). With autopilot on, the train keeps getting trips.
- **Monitor: starvation or congestion.** A starved platform now says which: "Starved: trains held up" when a
  delivery was on its way most of the time it was empty (trains held up on the way or queueing at the station, with
  how late arrivals there are), or "Starved: no trips" when none was (no train was sent; see the item's verdict). The
  item's details give the same split over all its consumers.
- **Monitor:** the item verdict "Blocked" is now "No trips planned": its supply is there and trains are free, but
  SmartTrains plans no trip for it.
- **Monitor:** the items tab's Trains column is now "Needed / able": trains the flow plan needs, and managed trains
  that could serve the item's routes. It was easy to read the second number as trains sent there.

## 0.6.4 (beta)

- **No more queues before busy stations** (new setting **Longest wait on the line**, 60 s). While trains leave
  stations at once, a trip is no longer planned if a train would wait longer than this before any of its stops,
  because other trains are booked or queued there. A repeat doesn't head for a station other trains are already on
  their way to. Ten or more trains had queued for Aluminum Ingot RSC across a crossing and deadlocked there.
- **Monitor: bottleneck stations.** A station that several managed trains head for at once on average is flagged
  "Bottleneck", with how many are on their way and advice (more bays, a second station, or a waiting bay).
- **Fixed: "needs about 0.0 trains"** in the monitor for items. Trains needed per route now count a car's capacity
  as its slots times the item's stack size, as the planner does.
- **Fewer leftover trips:** deliveries promised to a consumer now count against the platform they're planned at, like
  pickups. Two trains no longer both plan to fill the same platform's room, where the second one would unload nothing
  and then need a trip for its leftovers. A platform's room also counts what its machines use before the train
  arrives.
- **Steadier trip times:** once a station pair has been driven twice, one slow trip (a queue or a deadlock) counts at
  most 1.5 times the estimate.
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
- **Platform readouts:** "N managed trains fit" counts every train whose cars line up with the platforms, also while
  it carries something else (it delivers that first). It used to count only trains that were empty right now, and it
  called trains repeating their last job busy. The monitor's "No train fits" check and the fleet advice count the same
  way.
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
