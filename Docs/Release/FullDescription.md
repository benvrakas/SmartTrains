## SmartTrains

Timetables that make themselves. Give your freight and fluid platforms a role and the items they handle. SmartTrains
works out what has to move where, groups the stations by item, and sends each train where its cars are needed most,
trip by trip, as your factory grows. You don't plan a train per factory.

**Beta.** Try it on a copy of your save first (`/st testcopy`).

**Highly recommended:** [Auto Trains](https://ficsit.app/mod/AutoTrains) and
[Freight Car Control](https://ficsit.app/mod/FreightCarControl). SmartTrains works without them, but much better with
them (see below).

### How it works

- **Platforms:** open a freight or fluid platform and click **Open SmartTrains settings**.
  - **Supply:** managed trains pick items up here.
  - **Demand:** managed trains keep it stocked to a target.
  - **Sink:** managed trains unload cargo here that has nowhere else to go, any item: cargo a car can't unload at any
    consumer of it, and production nobody on the network uses.
  - **Off:** vanilla.
  - Each item gets a target and a priority (demand), or a **Least per visit** (supply, 50 % by default): a train
    only picks up there when it can load at least that share of what its cars there carry.
  - **Copy**, **Paste** and **Copy to the station's other platforms** carry every setting of the platform.
- **Set up a station from its factory:** build the station and its platforms, connect the belts and pipes, and open
  the station menu. Each platform's menu has **Autodetect**: it follows the belts and pipes to the machines and sets
  the platform to load or unload its item (where the machines take several items, the one the platform already holds).
  **Rename station from its factory**, under the station's name, names it from what the factory makes, its map area,
  a number and its most per minute, e.g. "Grass Fields Modular Frame In 1 (288)". Each shows what it would do first;
  click again to do it. The rate is the factory's theoretical most, limited by its
  weakest link apart from the trains: the belts out of its unloading platforms, or its machines. `/st setup all`
  does every station, and the migration window can rename stations the same way. The name format and words are
  yours to choose (`/st setup format`, `/st setup words`).
- **Trains:** in the locomotive menu, switch **Managed by SmartTrains** on. SmartTrains writes its timetable; the
  game, or Auto Trains, drives it.
- **Trips:** every trip is built around one consumer station. A free train looks at each consumer it can deliver to,
  fills the cars that line up with that station's unloading platforms from the suppliers that pay best, and unloads
  there. A factory with several inputs gets one gathering trip in its platform order (car 1 iron, car 2 wire); a
  one-item consumer gets a plain shuttle trip. The train runs the trip worth the most per second, so trains whose cars
  line up and trains nearby win by themselves. After every stop the rest of the trip is planned again from what the
  cars really hold. With nothing worth a trip, it parks.
- **Short of trains** in the monitor means an item's routes call for more trains than are moving it and no train
  that fits is idle: then more trains help. Idle trains that fit mean there's too little to move, not too few trains.
  `/st group` and the monitor's Groups tab show each item's routes, trains needed and trains moving it.
- **Recipes:** SmartTrains follows each platform's belts and pipes to its machines. Their recipes give the real rate
  of a full supplier or a starved consumer, and a consumer whose machines wait on another missing input counts its
  shortage less until that input arrives.
- **Each trip** loads what the stock forecasts call for, and only what the trip delivers.
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
- **Trains with nothing to move park** at the nearest free **Depot Signal** whose siding fits them, else at the
  nearest free station **depot**, else where they are, and leave as soon as they get a trip. One train per depot.
  - **Depot Signal** (AWESOME Shop, 1 coupon): a block signal that marks the siding behind it as a depot. Put it at
    the exit of a passing siding, with a signal at the siding's entry. No switch or station may lie between the two,
    and the siding must be at least 8 m longer than the train. Look at it to see whether it's ready and which trains
    fit, or why not; `/st depots` lists them all. New in 0.17.0 and not yet tried in game.
  - **Station depot** (switch it on under the station's name in the station menu): a station used as a rail yard. It
    may have sink or other platforms too: it is booked like any station, so a train coming to unload and a train
    coming to park never queue behind each other.
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

- **Migration** (`/st migrate`): sets up a whole network at once, with preview, apply and undo: every station's
  platforms as Autodetect would, optionally every station's name, and which trains SmartTrains manages.
  - It traces belts and pipes back to the machines behind each platform, and follows splitters, smart splitters,
    Modular Load Balancers and lifts. Platforms whose belts reach the same machines share the items out.
  - Change a platform afterwards in its own menu.
- **Monitor** (`/st monitor`): a window with Overview, Groups, Items, Stations, Platforms and Trains tabs. It shows per item
  what limits deliveries, with advice:
  - not enough supply;
  - not enough trains;
  - no train that fits the platforms;
  - a congested station.

  It also lists starved and backed-up platforms, and why idle trains wait.
- **Status and jobs** (`/st status`, `/st jobs`, `/st fit`): fleet advice, each train's trip, and which car stops
  beside which platform.
- **Empty cars** (`/st emptycars`, admin): deletes the cargo of the train you drive or look at;
  `/st emptycars all confirm` empties every managed train. Emptied trains get new trips.
- **Hold / Resume and Cancel job** in a managed train's locomotive menu: a held train stops and gets no trips until
  Resume; Cancel job drops its trip so it gets a new one.
- **Manage new trains** (Mods menu, off by default): new trains with freight cars and no timetable are managed 30 s
  after you last added a car.

### Works with

- **Auto Trains** (highly recommended): drives and routes the trains, and feeds its travel times and station queues
  into SmartTrains' planning.
- **Freight Car Control** (highly recommended): skips cars at stops.
- **Modular Stations.**
- **DynamicTimeTable, Dynamic Train Routes, RailDispatch**: they also steer trains, so don't keep them installed
  with managed trains. SmartTrains names any it finds in chat after loading.

### Multiplayer

All settings live on the host. Every player's menus show the host's state, and edits are logged with the player's
name.

- **Server settings in game:** on a dedicated server, admins change the settings with `/st settings` and
  `/st set <setting> <value>`. Your own Mods menu doesn't reach the server.
- **Always admin only:** settings, migration apply and undo, test copies and backups. An admin is the host, or a
  player signed in with the server's admin password.
- **Optional:** with **Only admins change SmartTrains** on, only admins can change platforms, stations and trains.
