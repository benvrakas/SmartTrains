## SmartTrains

Timetables that make themselves. Give your freight and fluid platforms a role and the items they handle. SmartTrains
works out what has to move where, turns that into **services** (loops of stations, each run by a few trains as a real
looping timetable) and keeps them adjusted as your factory grows. You don't plan a train per factory.

**Beta.** Try it on a copy of your save first (`/st testcopy`).

**Highly recommended:** [Auto Trains](https://ficsit.app/mod/AutoTrains) and
[Freight Car Control](https://ficsit.app/mod/FreightCarControl). SmartTrains works without them, but much better with
them (see below).

### How it works

- **Platforms:** open a freight or fluid platform and click **Open SmartTrains settings**.
  - **Supply:** managed trains pick items up here.
  - **Demand:** managed trains keep it stocked to a target. When it would run short and the only trains whose cars
    line up with it hold other cargo, one of them is emptied first and sent.
  - **Sink:** managed trains unload cargo here that has nowhere else to go, any item: cargo a car can't unload at any
    consumer of it, and production nobody on the network uses.
  - **Off:** vanilla.
  - Each item gets a target and a priority (demand), or a **Least per visit** (supply, 50 % by default): a service
    skips that pickup for a cycle when it would load less than that share of what its cars there carry.
  - **Copy**, **Paste** and **Copy to the station's other platforms** carry every setting of the platform.
- **Trains:** in the locomotive menu, switch **Managed by SmartTrains** on. SmartTrains writes its timetable; the
  game, or Auto Trains, drives it.
- **Services:** every consumer station gets a gathering loop: its suppliers, then the station, on the car layout that
  carries the most of what it needs. A factory's mixed inputs come in one loop, each car carrying the item whose
  platforms it lines up with, and one train can drop different cars at different consumers. Loops of the same layout
  merge when that saves trains (a train carrying something both ways instead of running back empty). Each service runs
  as a looping timetable, with the trains its flows need spaced evenly around it. `/st service` and the monitor's
  Services tab show them.
- **Spare trains** are spread over the services of their layout as extra trains, so they keep carrying something.
  Depots are optional, for trains no service can use.
- **Changing jobs:** a train joining a service first clears cargo that isn't the service's: delivered, or dumped into
  a sink on its way if those goods aren't needed soon. Goods a consumer needs are always delivered.
- **Recipes:** SmartTrains follows each platform's belts and pipes to its machines. Their recipes give the real rate
  of a full supplier or a starved consumer, and a consumer whose machines wait on another missing input counts its
  shortage less until that input arrives.
- **Each cycle** loads what the stock forecasts call for, and only what the loop delivers.
- **Fit:** train length, the order of the cars and the order of the platforms are checked for every trip. Push-pull
  trains, with a locomotive at each end, are handled both ways round.

### Traffic

- **Spacing:** trips are timed so trains arrive at a station at least **Gap between trains at a station** (Mods
  menu, 30 s) apart.
- **Leaving at once** (default): a train moves what it can in one transfer and leaves. A train with a trip starts it
  straight away. No trip makes more trains wait in front of a station than fit on the track before its switch
  (measured from the track), and never more than **Most trains waiting before a station** (2), so trains don't queue
  across junctions. Switch **Trains leave stations at once** off for the old behaviour: deliveries and pickups wait
  up to their longest wait, and a train whose next station is busy waits in its station.
- **Trains no service uses park** at the nearest free **depot** (a rail yard: switch it on under the station's name in the
  station menu), one train per depot, or else where they are, and move when a trip needs that station. A depot may
  have sink or other platforms too: it is booked like any station, so a train coming to unload and a train coming to
  park never queue behind each other.
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
