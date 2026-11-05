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
  - **Demand:** managed trains keep it stocked to a target.
  - **Off:** vanilla.
  - Each item gets a priority, a target and a minimum batch.
- **Trains:** in the locomotive menu, switch **Managed by SmartTrains** on. SmartTrains writes the train's timetable
  for each trip. The game, or Auto Trains, drives it.
- **Trips:** one trip can visit up to 4 stations and serve several platforms and items. It plans car by car: which
  car loads what where, and which car unloads where. After every stop the rest of the trip is planned again from what
  the cars really hold.
- **Fit:** train length, the order of the cars and the order of the platforms are checked for every trip. Push-pull
  trains, with a locomotive at each end, are handled both ways round.

### Traffic

- **Spacing:** trips are timed so trains arrive at a station at least **Gap between trains at a station** (Mods
  menu, 30 s) apart.
- **Waiting in the station:** a train whose next station is busy waits where it is instead of queueing on the line,
  where it would block junctions.
- **Parked trains give way:** idle trains park at a depot (a station setting) or where they finished. A parked train
  moves to a depot or a free station when a trip needs its station.
- **Dead ends:** a train parked in a dead-end station keeps it until it leaves, so no second train is sent into a
  bay it couldn't enter.

### Highly recommended mods

Both are optional: SmartTrains finds them while the game runs and uses them when they're there.

**[Auto Trains](https://ficsit.app/mod/AutoTrains)** drives the trains without signals and books track ahead.
SmartTrains works with it:
- Before a train leaves a station, SmartTrains asks whether its first leg can be driven now, around every other
  train, including trains coming the other way on a single track. If not, the train waits in the station instead of
  meeting the other one head on.
- Station queues, real travel times and which train waits for which feed into SmartTrains' trip timing.
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
    stop there.
- **Monitor** (`/st monitor`): a window with Overview, Items, Stations, Platforms and Trains tabs. It shows per item
  what limits deliveries, with advice:
  - not enough supply;
  - not enough trains;
  - no train that fits the platforms;
  - a congested station.

  It also lists starved and backed-up platforms, and why idle trains wait.
- **Status and jobs** (`/st status`, `/st jobs`, `/st fit`): fleet advice, each train's trip, and which car stops
  beside which platform.

### Works with

- **Auto Trains** (highly recommended): drives and routes the trains, and checks a train's way before it leaves.
- **Freight Car Control** (highly recommended): skips cars at stops.
- **Modular Stations.**
- **DynamicTimeTable**: migration reads its station groups. Don't keep it installed with managed trains, as both mods
  would write the same timetables.

### Multiplayer

All settings live on the host. Every player's menus show the host's state, and edits are logged with the player's
name.
