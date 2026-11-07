## SmartTrains

Trains without fixed timetables. Give your freight and fluid platforms a role and the items they handle. SmartTrains
then sends managed trains from where items pile up to where they're needed, by stock, production rate, distance and
how long each consumer has waited. You don't need a train per factory.

**Beta.** Try it on a copy of your save first (`/st testcopy`).

**Highly recommended:** [Auto Trains](https://ficsit.app/mod/AutoTrains) and
[Freight Car Control](https://ficsit.app/mod/FreightCarControl). SmartTrains works without them, but much better with
them (see below).

### How it works

- **Platforms:** open a freight or fluid platform and click **Open SmartTrains settings**.
  - **Supply:** managed trains pick items up here.
  - **Demand:** managed trains keep it stocked to a target. **Request a train** (Automatic by default): when the
    platform will fall below its target, even with the deliveries on the way, before any train that can carry the
    item could bring it, a train whose car lines up with it is emptied of what it holds (at consumers or sinks,
    whatever is soonest) and sent for it, in time to arrive **Request a train this early** (5 minutes) before. **On**
    asks until it and its deliveries are 90 % full; **Off** never asks.
  - **Sink:** managed trains unload cargo here that no consumer takes, any item.
  - **Off:** vanilla.
  - Each item gets a target and a priority (demand), or a minimum batch (supply: **Send at % of a load**, 50 % by
    default; a station 90 % full or a Critical consumer ships less).
  - **Copy**, **Paste** and **Copy to the station's other platforms** carry every setting of the platform.
- **Trains:** in the locomotive menu, switch **Managed by SmartTrains** on. SmartTrains writes the train's timetable
  for each trip. The game, or Auto Trains, drives it.
- **Planning (horizon planner, beta, on by default):** SmartTrains forecasts every platform's stock over the next
  hour and plans all free trains together, so the platforms that would run short soonest are served first. A train
  holding the wrong cargo can be emptied first when a platform needs it. Switch **Horizon planner** off in the Mods
  menu for the classic planner, which gives each free train its best next trip.
- **Steady roster (beta, on):** with the horizon planner, the routes of the flow plan are shared out over the trains
  that fit them and each train runs its routes in turn, like a timetable, while rates stay the same. The planner only
  corrects for what the roster doesn't cover.
- **Trips:** one trip can visit up to 4 stations and serve several platforms and items. It plans car by car: which
  car loads what where, and which car unloads where. After every stop the rest of the trip is planned again from what
  the cars really hold.
- **Fit:** train length, the order of the cars and the order of the platforms are checked for every trip. Push-pull
  trains, with a locomotive at each end, are handled both ways round.

### Traffic

- **Spacing:** trips are timed so trains arrive at a station at least **Gap between trains at a station** (Mods
  menu, 30 s) apart.
- **Leaving at once** (default): a train moves what it can in one transfer and leaves. A train with a trip starts it
  straight away. No trip makes more than **Most trains waiting before a station** (2) wait on the line before a
  busy station at once, so trains don't queue across junctions. Switch **Trains leave stations at once** off for the old behaviour: deliveries and pickups wait up
  to their longest wait, and a train whose next station is busy waits in its station.
- **No parking** (default): a train with no new trip runs its last job again, like a timetable loop, until a new trip
  is worth it, or waits at a **depot** (a rail yard: switch it on under the station's name in the station menu). With **Repeat the last job instead of parking** off, idle trains park at a depot
  or where they finished, and move to a depot or a free station when a trip needs their station.
- **Dead ends:** a train parked in a dead-end station keeps it until it leaves, so no second train is sent into a
  bay it couldn't enter.

### Highly recommended mods

Both are optional: SmartTrains finds them while the game runs and uses them when they're there.

**[Auto Trains](https://ficsit.app/mod/AutoTrains)** drives the trains without signals and books track ahead.
SmartTrains works with it:
- Station queues, real travel times and which train waits for which feed into SmartTrains' trip timing.
- With **Trains leave stations at once** off, SmartTrains also asks before a train leaves a station whether its
  first leg can be driven now, around every other train, including trains coming the other way on a single track.
  If not, the train waits in the station instead of meeting the other one head on.
- A parked train moves out of the way when Auto Trains reports a train waiting for it.
- The monitor shows trains Auto Trains reports as stuck or deadlocked.

Auto Trains 1.3.9 or later.

Without it, the game's own self-driving and your signals keep trains apart, as in vanilla: you need a properly
signalled network, above all on single track.

**[Freight Car Control](https://ficsit.app/mod/FreightCarControl)** (1.1.0 or later) lets each car skip a stop.
- With it, a train may stop at a station where only some of its cars have work. The other cars skip that stop, so
  cargo for later stops stays on board.
- Without it, every car loads or unloads at the platform it stops beside, as in vanilla. A trip is only used if every
  car fits the platform it stops beside: no car may load an item nobody planned, or unload where it shouldn't. Platforms
  still have their own roles and items either way, but far fewer trips work, mostly at stations where platforms differ
  along the train.

### Tools

- **Migration** (`/st migrate`): reads your network and proposes roles, items and managed trains, with preview,
  apply and undo.
  - It traces belts and pipes back to the machines behind each platform, and follows splitters, smart splitters,
    Modular Load Balancers and lifts.
  - Platforms the trace can't settle get items from their stock, their timetables, or the routes of trains that
    stop there. Platforms whose belts reach the same machines share the items out between them.
  - Click a station to open its platforms, and change a platform's role or items by hand before applying.
- **Monitor** (`/st monitor`): a window with Overview, Items, Stations, Platforms and Trains tabs. It shows per item
  what limits deliveries, with advice:
  - not enough supply;
  - not enough trains;
  - no train that fits the platforms;
  - a congested station.

  It also lists starved and backed-up platforms, and why idle trains wait.
- **Status and jobs** (`/st status`, `/st jobs`, `/st fit`): fleet advice, each train's trip, and which car stops
  beside which platform.
- **Empty cars** (`/st emptycars`, admin): deletes the cargo of the train you drive or look at;
  `/st emptycars all confirm` empties every managed train.

### Works with

- **Auto Trains** (highly recommended): drives and routes the trains, and feeds its travel times and station queues
  into SmartTrains' planning.
- **Freight Car Control** (highly recommended): skips cars at stops.
- **Modular Stations.**
- **DynamicTimeTable**: migration reads its station groups. Don't keep it installed with managed trains, as both mods
  would write the same timetables.

### Multiplayer

All settings live on the host. Every player's menus show the host's state, and edits are logged with the player's
name.

- **Server settings in game:** on a dedicated server, admins change the settings with `/st settings` and
  `/st set <setting> <value>`. Your own Mods menu doesn't reach the server.
- **Always admin only:** settings, migration apply and undo, test copies and backups. An admin is the host, or a
  player signed in with the server's admin password.
- **Optional:** with **Only admins change SmartTrains** on, only admins can change platforms, stations and trains.
