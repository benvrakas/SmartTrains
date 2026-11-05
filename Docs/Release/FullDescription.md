## SmartTrains

Trains without fixed timetables. Give your freight and fluid platforms a role and the items they handle. SmartTrains
then sends managed trains from where items pile up to where they're needed, by stock, production rate, distance and
how long each consumer has waited. You don't need a train per factory.

**Beta.** Try it on a copy of your save first (`/st testcopy`).

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

### With and without Freight Car Control

[Freight Car Control](https://ficsit.app/mod/FreightCarControl) is optional. It lets each car skip a stop.

- **With it**, a train may stop at a station where only some of its cars have work. The other cars skip that stop,
  so cargo for later stops stays on board.
- **Without it**, every car loads or unloads at the platform it stops beside, as in vanilla. A trip is only used if
  every car fits the platform it stops beside: no car may load an item nobody planned, or unload where it shouldn't.
  Platforms still have their own roles and items either way. Fewer trips work without it, mostly at stations where
  platforms differ along the train.

### Tools

- **Migration** (`/st migrate`): reads your network and proposes roles, items and managed trains, with preview,
  apply and undo.
  - It traces belts and pipes back to the machines behind each platform, and follows splitters, smart splitters,
    Modular Load Balancers and lifts.
  - Platforms the trace can't settle get items from their stock, their timetables, or the routes of trains that
    stop there.
- **Monitor** (`/st monitor`): shows per item what limits deliveries:
  - not enough supply;
  - not enough trains;
  - no train that fits the platforms;
  - a congested station.

  It also lists starved and backed-up platforms, and why idle trains wait.
- **Status and jobs** (`/st status`, `/st jobs`, `/st fit`): fleet advice, each train's trip, and which car stops
  beside which platform.

### Works with

- **Freight Car Control** (optional): skips cars at stops.
- **Auto Trains**: drives and routes the trains.
- **Modular Stations.**
- **DynamicTimeTable**: migration reads its station groups. Don't keep it installed with managed trains, as both mods
  would write the same timetables.

### Multiplayer

All settings live on the host. Every player's menus show the host's state, and edits are logged with the player's
name.
