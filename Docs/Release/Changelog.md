# Changelog

## 0.22.2 (beta)

Fixes for the gridlock on the IB save (from about 14:00, down from about 200 finished jobs an hour to about 30):

- **An idle train no longer parks in a station that trips are heading for.** A train that had just finished a
  one-stop delivery and found nothing to do drove back to the stop it had just left, the consumer, and parked inside
  it; the next delivery then waited at the door, standing on the parked train's way out (Dune Desert Crystal
  Oscillator In 1, Southern Forest Nitric Acid In 1, No Man's Land Coal In 1). Now an idle train parks where it stands
  only in a station no train needs; otherwise it drives to the nearest station no train needs (after trying a depot
  signal) and parks there.
- **Trains waiting at the door of a station a parked train can't leave now go elsewhere.** Their trips used to be
  re-planned only with deliveries to that same station, so they came straight back. Now those trips end, the station
  takes no new trips for a minute, and the trains get a trip elsewhere or park out of the way, which clears the way
  out.
- **A parked train whose way out is blocked tries the next way out** (the next parking station) instead of the same
  one every few seconds, and logs it once; with every way blocked it says so once and waits. On the IB save one train
  logged "moving it" 1,811 times in under two hours.
- A trip to a station that turns out unreachable ends the same way (it used to be re-planned only for that station).

## 0.22.1 (beta)

- **"In" stations unloading several items get no rate in their name.** The set-up named them after the factory's
  product and showed the product's rate (e.g. "Grass Fields Modular Frame In 1 (288)"), which isn't what the station
  takes in. A station unloading one item still shows how much of it the factory uses; "Out" and mixed stations keep
  their rates. Rename a station again (or `/st setup all`) to update its name.

## 0.22.0 (beta)

- **Trains really drive to depot signals now.** Until now SmartTrains emptied a train's timetable and gave the game a
  path to the signal, but the game ignores such a path unless the train is in its follow-path state: it flagged "no
  timetable", and Auto Trains didn't manage the train either. On the IB save most depot-bound trains stood on the main
  line: 27 sent in an hour, 2 arrived, the network went from 85 % of its trains moving to about 20 %, and Auto Trains
  deadlocked.
  - **With Auto Trains** (1.4.15 or later, its new drive-to API) Auto Trains plans and drives the trip like any other
    and stops the train at its spot.
  - **Without Auto Trains** the train follows the path in the game's follow-path state, along your signals, and the
    signal it stops before (the depot signal, or the far-end signal of a dead end) shows red while it parks there.
    The siding rules for networks without Auto Trains are back: an entry signal after the siding's switch, and a
    signal facing in at a dead end's far end.
  - A train is parked once it has arrived. If there's no way there, that depot signal isn't offered to the train for
    10 minutes.

## 0.21.1 (beta)

- **Fixed: idle trains on their way to a depot signal were sent there again every dispatch round** (every few
  seconds). Each time their timetable was emptied (the game's "no timetable" warning flashed) and their path replaced,
  so they kept stalling on the line and held other trains up. A train already on its way with its path is now left
  alone until it parks or gets a trip. (Trains do drive to depot signals and park there: seen on the IB save.)

## 0.21.0 (beta)

- **Several trains per depot siding.** Depot signals one after another along a long siding make several parking spots.
  Trains fill the spot furthest in first, and a parked train only takes a job once the spots on its way out are empty
  (its status says which train it waits for). Which spots are taken is worked out from where trains stand, so it holds
  after a restart.
- **Dead-end depots (trains back out).** A depot signal at the entrance of a dead-end siding, facing in, makes it a
  depot for trains with a locomotive at each end: they drive in to the end and leave backwards. Chains work there too
  (deepest spot first, the one nearest the entrance leaves first). Without Auto Trains, a second signal at the track
  end, facing out, is needed. New Mods menu setting **Dead-end depots (trains back out)**, on by default.
- A depot-bound train is parked once it stands near where its front should stop, rather than near the signal.

## 0.20.1 (beta)

- **Depot signals no longer need an entry signal with Auto Trains.** The siding now runs back from the depot signal to
  its switch; on a network without signals every depot signal said "not usable: a switch lies between it and the
  signal before it". Without Auto Trains an entry signal after the switch is still needed (the game's own blocks would
  otherwise put a parked train in the main line's block). The game's "Signal loops into itself" warning on a depot
  signal doesn't matter with Auto Trains.

## 0.20.0 (beta)

- **Car patterns work with sushi platforms.** A car's part of the pattern is now a set of items: beside a platform
  taking iron and coal it is "iron + coal", and it carries both (loaded together, or at different stops). A station
  lines up when the platform beside each car takes exactly that car's items; a platform taking only iron is another
  group. Cargo lines up when everything in the car is in its set. `/st patterns` shows sets as "Iron Plate + Coal".
- Trains start with an open pattern after updating (0.18.0's one-item patterns aren't read any more) and take their
  sets from their first trips.
- Fixed in the trip planner: a car offered several items at one stop that filled up on the first counted the others
  as loaded and never delivered, so the car skipped the stop entirely.

## 0.19.2 (beta)

- **Fixed: placing a depot signal crashed the server** (and would have crashed every client that saw it). The settings
  copied from the vanilla Block Signal at startup only reached the depot signal's class defaults, not the signals
  built from it; each new depot signal now takes them as it's created.

## 0.19.1 (beta)

- **Fixed: equipping the depot signal crashed the game.** Its left/right-handed mesh data wasn't copied from the
  vanilla Block Signal, and the build gun's hologram read it as soon as it appeared.
- **The depot signal unlocks with the Block Signal**, next to it in the build menu (Vehicles). It was an AWESOME Shop
  item in 0.17.0 and 0.18.0; saves that have block signals get it within seconds of loading.

## 0.19.0 (beta)

- **Station depots are gone; depot signals replace them.** The station menu has no depot switch any more (the bar
  under the station's name is just **Rename from factory...**), and `/st station depot` is gone. Idle managed trains
  park at the nearest free depot signal whose siding fits them, else where they stopped. A parked train in a station
  a trip needs moves to a free depot signal, else to the nearest station no trip needs. Stations switched to depot in
  older saves simply stop being depots.
- **Platforms keep the item they hold.** Autodetect, the station set-up and the migration now always give a platform
  the item or fluid it holds (the one it holds most of); the belt and pipe trace only picks the item for an empty
  platform. Before, the trace won whenever it found machines that don't use the held item. Where they disagree, the
  preview notes it ("holds X; its machines take Y"), so a stray item from an old wrong delivery is easy to spot.
- Platforms that already have a role are still left as they are (Autodetect in a platform's own menu redoes that
  platform).

## 0.18.0 (beta)

**Car patterns: a car only ever carries its one item.** On the IB save trains still came out mixed after `/st emptycars
all confirm`: every trip was planned for one consumer, but a platform fills a car to the brim, not to the planned
amount. The consumer only took what it had room for, and the rest rode along on the train's next trip for another
factory (e.g. 10,000 Concrete left in a car after a Steel Pipe factory took 6,000, then a Coal trip).

- Each managed train keeps a **pattern**, the item per freight car, and only serves consumer stations whose
  unloading platforms line up with it. Stations with the same pattern share trains; a station with fewer platforms
  only lines up with the front of the train.
- A trip only loads a car with its pattern item. A car beside a platform that would fill it with another item skips
  the stop (Freight Car Control), or the trip isn't used.
- Leftovers that line up stay on board; the car takes less next time.
- New trains (and every train after updating) start open and take their pattern from their first trips.
- **Changing group:** after 5 minutes with nothing to do in its pattern, a train may serve a consumer of another
  pattern. If cargo on board doesn't line up there, it first unloads those cars, at consumers of the cargo or else at
  a sink platform; cars that line up keep their cargo. Trains carrying mixed cargo from before do this first.
- A trip's name follows its re-plans (it kept the first plan's items before).
- `/st patterns` lists the train groups and each consumer station's platforms in order; the train's status line shows
  its pattern.
- Removed: "cargo on board first" from 0.16.0 (patterns make it unnecessary).

## 0.17.0 (beta)

- **Depot signals (new, not yet tried in game).** A **Depot Signal** (AWESOME Shop, SmartTrains Depot Signal, 1
  coupon; built like a Block Signal, same look, same cost, next to it in the build menu) marks the siding behind it as
  a depot. A managed train with nothing to do drives onto the nearest free depot siding that fits it and waits at the
  signal; it leaves as soon as it gets a trip. No station needed.
  - The siding is checked: from the signal back to the siding's entry signal there must be no switch and no station,
    and it must be longer than the train plus 8 m. Look at a depot signal to see "ready, 312 m of siding (fits trains
    up to 304 m)" or why it can't be used. `/st depots` lists them all.
  - Only passing sidings for now (an entry signal after the switch, the depot signal at the exit); dead-end sidings
    aren't supported yet.
  - Station depots still work; trains use depot signals first. They'll go once depot signals have proved themselves.
- **Hold / Resume and Cancel job** in the locomotive menu of a managed train (also `/st train hold|resume|canceljob`).
  A held train stops (autopilot off) and gets no new trips until Resume, which carries on with its job. Cancel job
  ends its job; it gets a new trip at the next dispatch round, or parks.
- **Manage new trains** (Mods menu, off by default): a new train with freight cars and no timetable is managed 30 s
  after its cars last changed. Trains with a timetable of their own, kept vanilla or switched off are left alone.
- **Notice when other train mods are installed:** DynamicTimeTable, Dynamic Train Routes and RailDispatch also steer
  trains; a chat line after loading names them and what they do to managed trains (and the log too).

## 0.16.0 (beta)

- **The migration and station set-up leave platforms that are already set up alone.** A platform with a SmartTrains
  role keeps it; the preview shows it as "set up". On the IB save the migration had set the four Encased Uranium
  Cell platforms of a Uranium Fuel Rod station to Rotor, EM Control Rod and Crystal Oscillator. Autodetect in a
  platform's own menu still sets that platform afresh.
- **Items a factory makes for itself are no train input.** When the machines a station feeds get an item from the
  factory's own machines or resource nodes as fast as they use it, its unloading platforms aren't given that item.
  A Rocket Fuel plant that makes its own Fuel kept two Nitrogen Gas platforms as Fuel before.
- **Several input stations of one factory share items out together, by rate.** Platforms (of any of the factory's
  stations) feeding the same machines get items in proportion to car loads per minute: a fast input such as Encased
  Uranium Cell (1,440/min) gets several platforms, slower ones one each. Platforms set up before, and platforms that
  hold one of the items, count as they are.
- **A train with cargo on board delivers it first.** When a platform took less than planned (it was full), the rest
  stayed on board and the train's next trip, for another factory, loaded beside it, so trains carried cargo for
  several factories at once. Now such a train first goes to a consumer of its cargo and only then takes other work,
  unless no consumer needs that cargo now.
- **`/st emptycars` ends the trains' jobs.** An emptied train used to keep its trip (and its consumer) and was only
  re-planned, so it kept heading for the same factory with nothing on board. Now its job ends and the next dispatch
  round gives it a new trip, or it parks.

## 0.15.3 (beta)

- **Migration window: apply names, platforms or both.** The toolbar button that was Rename stations on/off now says
  what Apply sets and cycles on click: **platforms only**, **platforms and names**, **names only**. Names only renames
  the ticked stations and leaves their platforms as they are. The New name column shows while names are included.
  Chat: `/st migrate apply all namesonly`.
- **Fixed: the station menu's depot and rename buttons ran off the menu** and their preview text covered the power
  graph (0.15.1, 0.15.2). They are now one row under the station's name, **SmartTrains depot** and **Rename from
  factory...**; the first click shows the new name on the button ("Rename to: ..."), the second renames. A name that
  wouldn't change says "Name is up to date" instead of asking to confirm.

## 0.15.2 (beta)

- **Migration window tidied up.** One toolbar instead of three rows: Preview, Apply ticked, Undo, **Rename stations
  on/off**, and (with beta tools) Test copy and Backup. Three tabs: **Stations**, Trains and Result; the Overview tab
  is gone (the tab names carry the counts, and the "To check" filter lists what needs a look).
- **New name column:** while Rename stations is on, the Stations tab shows each station's new name next to its
  current one ("stays" when it keeps its name, "..." while names are still being worked out). The table has column
  headings, and clicking a station opens its platforms (the separate [+] column and "Close all" are gone).

## 0.15.1 (beta)

- **Fixed: Autodetect could swap a factory's inputs between its platforms.** Where the belts of several platforms
  reach the same machines (a factory with several inputs), each platform got one of the machines' items in turn, not
  the one it carries: at MF Resource SNK the platform full of Reinforced Iron Plate became Steel Pipe and the one full
  of Steel Pipe became Reinforced Iron Plate. Of the items its machines use or make, a platform now takes the one it
  already holds; only an empty platform gets one shared out.
- **Rename station from its factory** is now under the station's name on the station menu's first tab, next to the
  depot switch. It used to be in a part of the menu that didn't show.
- **Removed: Autodetect all platforms** from the station menu. Set platforms one at a time with **Autodetect** in
  each platform's menu. `/st setup` and the migration still set every platform.

## 0.15.0 (beta)

**The migration tool is the station set-up for every station, plus the trains.** Only the platforms' roles and items
and the trains to manage need migrating, and the station set-up does the platforms better.

- The preview sets every station's platforms as **Autodetect** does (direction from the belts and pipes, items from
  the machines they reach), names them if **Rename stations** is on, and ticks every train that can dock.
- Removed from the migration: DynamicTimeTable group reading (and "dedicated" trains kept vanilla), items from
  timetable filters and train routes, editing platforms by hand in the window, the trace limit and "every traced
  item" options. Change a platform afterwards in its own menu.
- Undo puts back platforms, station names and managed trains, as before.

## 0.14.0 (beta)

- **Fixed: the migration preview froze the server** for about 18 s per click on a large save (the station names of
  0.11.0 were all worked out in one go), and nothing else, platform menus included, was answered meanwhile ("Waiting
  for the server"). The preview now comes at once and the names follow a few stations at a time; the window shows them
  when they are ready. The Preview button ignores clicks while a preview is on its way. `/st setup all` works the
  same way.
- **Station menu:** two buttons instead of Show set-up / Apply: **Rename station from its factory...** and
  **Autodetect all platforms...**. Each shows what it would do; click it again to do it.
- **Platform menu:** **Autodetect from its belts and pipes** sets that platform's role and item from what it is
  connected to, at once.
- Server console: `SmartTrains.MigratePreview` runs the migration preview (for tests on a dedicated server).
- **Much faster station analysis:** the walk through a factory now visits each belt and building once. Naming all
  112 stations of a large save takes about 3 s instead of 30, and the slowest station 0.2 s instead of 15.
- **No background traces any more.** Platforms remember the machines their belts and pipes reached when they were
  set up (Autodetect, station set-up or the migration), saved with the world, and their recipe rates come from those.
  After rebuilding a factory, press Autodetect again. A save from before 0.14.0 has its platforms traced once after
  loading, a few at a time.
  The "belts now carry something else" hint of 0.11.0 is gone with the background trace.

## 0.13.0 (beta)

**Item groups as a dispatcher are gone.** Consumer-anchored trips (0.12.0) delivered more, so the old way is removed.

- Removed: home groups, lending, **Stay in its home group** (locomotive menu and `/st train stay`), per-item train
  limits (`/st group <item> trains`) and the **Item groups (0.10 dispatcher)** setting. Saved homes and limits are
  dropped on load.
- `/st group` and the monitor's **Groups** tab stay as a readout: each item's routes in the flow plan, the trains
  they need, the trains moving it now, and whether it is short of trains.

## 0.12.0 (beta)

**Every trip is built around one consumer.** Like a DynamicTimeTable group per factory, made automatic.

- A free train looks at each consumer station it can deliver to, fills the cars that line up with that station's
  unloading platforms from the suppliers that pay best, and unloads there. A factory with several inputs gets one
  gathering trip (car 1 iron, car 2 wire...) in its platform order; a one-item consumer gets a plain shuttle trip.
- The train runs the trip worth the most per second. Trains whose cars line up better, and trains nearby, win by
  themselves: no home groups, no lending.
- After each stop the rest is planned again, but only deliveries to the trip's consumer are added. Cargo for
  somewhere else stays on board and goes on the next trip.
- **Item groups (0.10 dispatcher)** in the Mods menu switches back, for comparison. It will be removed.
- On a copy of a large save (26 trains), 20 minutes from the same start: about 19 % more car loads delivered than
  with item groups, full or emptied trains, in more and shorter trips.
- Server console: `SmartTrains.EmptyCars` empties every managed train (as `/st emptycars all confirm`), for tests.

## 0.11.0 (beta)

**Station set-up from the factory.** Build a station and its platforms, connect them, and let SmartTrains set it up.

- **Show set-up** in the station menu: per platform a role from what is attached (belts or pipes only into it: it
  loads; only out of it: it unloads; nothing: off) and its item from the machines its belts and pipes reach (one per
  platform; platforms serving the same machines share the items out). Then a name from what the factory makes, the
  map area it stands in, a number and its most per minute, e.g. "Grass Fields Modular Frame In 1 (288)".
- **The rate** is the factory's theoretical most with trains bringing all it needs, limited by the weakest link apart
  from the trains: usually the belts out of an unloading platform (two Mk5 belts of plastic into an AI Limiter
  factory fix its output), else the machines (clock speed and somersloop boost counted, miners and wells at their node
  purity). Byproducts such as empty canisters feed the recipes that need them. The readout says which link limits.
- **Names:** a station that loads is named by what it loads; a station unloading one item by that item and how much
  of it the factory can use ("Southern Forest Heavy Oil Residue In 1 (600)"); a station unloading several items by
  what the factory makes. Two stations loading from one factory each get their share.
- On a copy of a large save, 33 of the 52 stations whose names carried a rate got the same rate within 10 %; most of
  the others were labelled with their belts' capacity rather than what their machines make.
- **Apply set-up** sets it up as shown; **Apply, keep the station's name** only sets the platforms. Nothing changes
  until you click.
- `/st setup [apply] [keepname]` does the same in chat, `/st setup all [apply]` for every station, with a report.
  `/st setup format` and `/st setup words` choose how names look (for example `SRC`, `RSC`, `SNK`), per world.
- **Migration window:** the preview shows each station's new name, and **Rename stations** renames the ticked
  stations when applying. Undo puts the names back.
- **Hint:** when a platform's belts or pipes now carry something else than it is set to (a new recipe or belt), its
  readout and the station menu say so.
- The connection trace now stops at other train platforms instead of walking through them.
- **Recipe rates count production boost** (somersloops): a boosted machine makes twice as much, so a full supplier
  behind boosted machines offers twice the recipe rate in the flow plan (it counted the plain recipe before).

## 0.10.2 (beta)

- **Fixed: the monitor window could lose rows on a big network.** The view sent to a player's game is trimmed to fit
  one network message, but the Groups tab (like the Services tab before it) wasn't counted, so the view could be
  larger than measured while other tabs lost rows. On the IB server 25 rows were left out. The Groups tab is now
  counted and trimmed like the others: details of its least urgent groups go first, short groups stay.

## 0.10.1 (beta)

- **Fixed: on a big network no train got a trip.** Each dispatch round set up every free train first (listing and
  scoring all its moves, a few ms each), and on the IB save that used the round's planning time (25 ms) before any
  train was planned, so every round ended with nothing. Trains are now set up only when their turn comes, the train
  that waited longest for a turn goes first, and every round plans at least one train with planning time of its own.

## 0.10.0 (beta)

**Services are replaced by item groups.** Trains no longer belong to fixed loops; they take work as it comes up.

- **Item groups:** every item the plan moves is a group of its suppliers and consumers, and a new station joins its
  item's group by itself. Each managed train gets a **home group** its cars fit, shared out by what the groups need.
  With too few trains every group gets its share, and spare trains spread the same way.
- **Trips:** a free train takes its home group's work first and may add other work on the way (another item for the
  same factory, a load back instead of running empty). When its group has nothing to move, it is **lent** to whatever
  is worth running elsewhere (log `LEND`). Only with nothing anywhere does it park.
- **Fixed: "short of trains" with trains waiting at the depot.** A service owned its trains, so a service train
  with nothing to move on its loop parked while other services were short. On the IB save the monitor called 25
  trains 11 to 18 short while service trains stood at Train Station 114 to 118. A group is now short only while it
  lacks trains and no train that fits it is idle: idle trains that fit mean too little to move, not too few trains.
- **Steadier:** a group's need is smoothed over flow plans, and trains keep their homes while their group still
  wants them. Needs that swung from 6.6 trains to 0.2 between plans used to send trains across the map and back.
- **Monitor:** the Services tab is now **Groups**: home trains and trains needed, trains working on it now (home or
  lent), routes, and what each home train does. The Overview shows the groups short of trains, or says the idle
  trains have nothing they fit to move.
- **Commands:** `/st group` lists the groups, `/st group <item>` shows one, and `/st group <item> trains <least>
  <most>` sets limits (saved per item, so they survive re-planning). `/st train stay on|off` keeps a train's home
  group. The locomotive menu's **Stay in its home group** replaces **Stay in this service**.
- **Removed:** service loops, spacing and loop locks (`/st service`), **Keep out of services** (switch a train's
  management off instead), and code left from earlier versions: the roster and shuttles, platform requests (their
  hidden button too), repeat and loop jobs, the unused horizon search and train assignment, and the conversions for
  saves from 0.6.8 and older.
- **Saves:** services, their settings and train service flags from 0.9.x are dropped; groups form from the first flow
  plan after loading (about 30 s). Saves from 0.6.8 or older should be loaded once in 0.9.5 first.

## 0.9.5 (beta)

- **Fixed: a service train clearing other cargo could be sent off on unrelated trips.** A train joining a service first
  delivers (or dumps) cargo that isn't the service's. When that trip was planned again from scratch, for example
  because another train needed its station, it was planned like any free train's and picked up new work: on the IB
  save, one drop became a 5-stop nitrogen and sulfur trip. It now stays deliveries-only, as it was planned, and its
  timetable is rewritten less often (each rewrite makes Auto Trains plan the train again).

- **Fixed: a train standing in a station could be given that station as its next stop, and then stood there for
  good.** The game doesn't dock a train that is already in the station, so it waited "at its goal" and held the
  station track. On the IB save four trains queued behind one for up to 12 minutes. A train counted as being at a
  station only while docked or just after leaving; now any vehicle on the station's or its platforms' tracks counts.

## 0.9.4 (beta)

- **Services whose flows dip keep their loop.** A service used to end after about two minutes without flows (three
  flow plans), which happens whenever a consumer reaches its target or rates are measured again after a load, and came
  back minutes later as a new service with a new number, without its lock, spacing or train limits. It now keeps its
  loop for 15 minutes without flows. Meanwhile it asks for no trains beyond the player's "at least", so its trains
  serve other services at once instead of one of them standing parked in an empty loop.
- **Fixed: a service cycle could be dropped after its first stop with the cargo on board.** After each stop the rest of
  a trip is checked again, and that check refused it when the next station had no room to wait before its switch, a
  rule meant for choosing new trips. A service cycle then ended on the spot: on the IB save, a train loaded 3200 copper
  ingots and then abandoned the 7 stops that would have used them. A trip under way now keeps its stops and waits.
- **Trains with the same name are told apart.** Trains sharing a name (every train nobody named is "Train") show the
  end of their actor name in the log and menus, as in `Train #5678`. The number is kept in the save, and Auto Trains
  1.4.13 uses the same one, so the two mods' log lines about a train can be matched.

## 0.9.3 (beta)

- **Fixed: trains of a service with more than 6 stops never left.** Services merge loops up to 8 stops and an hour, but
  each cycle was still checked against the trip limits ("Most stops per trip", at least 6, and "Longest trip"), so every
  train of such a service parked with "nothing to move this cycle (too many stops)". On the IB save, 12 trains of an
  8-stop service stood idle while the monitor reported the layout 14 trains short. A service's cycles now use the
  service's own limits, and the re-plan of a trip under way is checked against its own length.

## 0.9.2 (beta)

- **Fixed: dismantling a station a managed train was heading for crashed the server**, or left the train with "next
  stop invalid". The game drops a dismantled station from timetables; SmartTrains then wrote the train's job back with
  the station that no longer exists, and building station bookings read it. Now a job's stops whose station is gone are
  left out (the job ends if none is left and the train is planned again), bookings skip them, a lasting "next stop
  invalid" gets the timetable written again, and a service with a station that is gone ends and is designed again.

## 0.9.1 (beta)

- **Uses Auto Trains' congestion data** (Auto Trains 1.4.12 or later): how busy the switches and crossings around each
  station are over the next three minutes, and how long trains already wait there.
  - A stop at a station whose way in is busy costs more, so trains clearing cargo go elsewhere when they can.
  - A service doesn't send its next train into a way in that is fully booked with trains already waiting, unless the
    service is well behind its spacing.
  - The station menu shows how busy its way in is.

## 0.9.0 (beta)

**Services run the network.** SmartTrains no longer plans trains trip by trip. It turns the flow plan into services:
loops of stations that trains run as real, looping timetables, adjusted as the factory changes.

- **What a service is:** a loop of pickups and drops, run by trains of one car layout, with each car's work at each
  stop. A factory's mixed inputs come in one loop (car 1 iron, car 2 coal...), and one train can drop different cars at
  different consumers along the way (spacer platforms).
- **How they form:** every consumer station gets a gathering loop (its suppliers, then it) on the car layout that
  carries the most of it. Loops of the same layout merge into one bigger loop when that saves trains, for example two
  plants at opposite ends of the map served by one loop instead of two that each run back empty. They change only when
  that clearly pays, and end when their flows stay gone.
- **Trains:** each service gets the trains its flows need, spaced evenly around its loop. Trains left over are spread
  over the services of their layout as extra trains, so they keep carrying something instead of sitting in depots;
  depots are optional, for trains no service can use.
- **Joining and leaving:** a train that joins a service first clears what isn't the service's cargo: delivered, or
  dumped into a sink on its way when the goods aren't needed soon (needed goods are always delivered).
- **Each cycle** loads what the forecasts call for; a pickup that would load less than its platform's **Least per
  visit** is left out of that cycle, so the service keeps its spacing instead of waiting.
- **Monitor:** a Services tab (each loop, its trains, spacing, loop time, what each train is doing), tiles for
  services, short services and flows not covered, and the fleet by car layout with advice on which trains to add.
- **Menus:** the platform menu says which services serve it (or why none does); the station menu lists the services
  calling there, its room to wait and the stations sharing its switch; the locomotive menu shows the train's role and
  has **Stay in this service** and **Keep out of services**.
- **Commands:** `/st service`, `/st service S3`, `/st service S3 lock|unlock|spacing <minutes>|trains <least> <most>`,
  `/st service <item> dedicated|auto`, `/st train stay|reserve on|off`.
- Removed: the trip-by-trip planner for free trains, the roster and per-item shuttles, the Services and roster switch.

## 0.8.0 (beta)

One way of deciding instead of many: the parts that each patched a problem of their own are replaced by a few
general rules.

- **One planner.** The horizon planner, which plans every free train against a forecast of every platform, is the
  only planner; the classic planner and the request fallback are gone (the planner already empties a train for a
  platform that runs short). Every trip is scored the same way: what it is worth, less what the train's time costs.
- **Idle trains park.** A train with nothing to do goes to a free depot, or parks where it is and moves when a trip
  needs that station. Repeating and looping the last trip are gone, with their settings.
- **Room to wait comes from the track.** SmartTrains measures the track in front of each station up to its switch and
  plans no trip that would make more trains wait there than fit. A station with no room in front (like a short depot
  siding) never gets a queue on the main line, whatever the trains come for. Stations reached through the same switch
  count each other's trains as traffic.
- **Sinks take only what has nowhere else to go:** cargo a car can't unload at any consumer of it (no consumer, or
  none where that car stops beside the right platform), and production nobody on the network uses. Everything else
  waits for its consumer.
- **Cars load only what the trip delivers.** No more leftovers loaded on purpose, which tied cars to one item.
- **Services.** An item whose flow keeps a whole train busy gets dedicated trains that run all of its routes, picking
  up at a steady spacing; they stay through short dips. `/st service` lists them and lets you pin an item, keep it in
  the pool, set its spacing or choose its trains.
- **Recipes.** SmartTrains reads the machines behind each platform. A starved consumer's or full supplier's rate comes
  from their recipes, and a consumer whose machines wait on another missing input counts its shortage less until
  that input arrives (the monitor names these).
- **Snapshots** (`/st snapshot`, admin): writes the planner's inputs to a file that the automated tests can replay.
- Removed settings: Repeat the last job instead of parking, Horizon planner, Request fallback, Re-plan threshold,
  Request a train this early. The platform menu's Request a train button is hidden.

## 0.7.7 (beta)

- **Sinks only get cargo nobody needs.** Cargo a train's current trip didn't deliver counted as unwanted after a few
  minutes, and then all of it could go to a sink, even items consumers were waiting for. Once the server's depot
  sinks became reachable, trains dumped coal, concrete, copper, sulfur and even AI Limiters there and queued onto the
  main line for the depot stations. Now an item only goes to a sink if no consumer needs it within the hour.
- The UNREACHABLE warning says to check which way the station faces first: a station built backwards also reads as
  "unreachable with signals".

## 0.7.6 (beta)

- **A shuttle that can't run no longer blocks its item.** The copper shuttle was holding leftover cargo nobody
  wanted, so it couldn't run copper and kept looping its last trip, while other trains were kept off every copper
  route: copper consumers starved with full suppliers and idle trains. Other trains are now kept off a shuttle's routes
  only while it actually runs them, and a train only becomes a new shuttle with empty cars or cars holding its item.
- **Unreachable stations are left out.** When the game finds no way to the station a train is heading for (for
  example a one-way signal passed from behind on the way into a depot) for 20 seconds, that train leaves the station
  out for 30 minutes and goes elsewhere, instead of standing there. The log names the station (UNREACHABLE).

## 0.7.5 (beta)

- **Idle trains stop moving cargo nobody planned.** With no free depot, a train with nothing to do keeps driving its
  last trip so it doesn't block the line, but its stops no longer load or unload. On the server these loops were
  almost half of all jobs and nearly all of them delivered nothing, while loading cargo no one asked for.
- **Depots hold one train each.** An idle train only goes to a depot no other train is at or heading for, and a train
  moved out of a busy depot goes to another free depot first. The monitor says how many trains loop because no depot
  is free.
- **Full suppliers count properly.** A full platform can't take anything from its belts, so its measured supply was
  only what trains took (coal: 207/min counted while it was backing up, against 685/min of demand). A full supplier
  now also offers its stock, so the flow plan gives it the trains it needs.
- **Shuttles per item.** Shuttles are now sized by everything an item needs, not per supplier and consumer pair: sulfur
  needed two trains over three routes, so no route alone kept its shuttle. A shuttle now runs all of its item's routes.
- **Shuttles carry full loads.** A shuttle takes the route that delivers most, and only leaves with at least half a
  train; before, a quarter of one car was enough.
- **No tiny pickups.** A car doesn't load less than a twentieth of a car of an item.

## 0.7.4 (beta)

- **Shuttles actually shuttle.** In 0.7.3 a train made a shuttle in the middle of a trip kept getting new stops added
  at every stop, so it never ran its route (on the server the sulfur shuttle spent 22 minutes on other cargo), while
  other trains were kept off that route. Now a new shuttle drops its other pickups, delivers what it carries and starts
  its route; other trains are kept off the route only while the shuttle is running it.
- **The flow plan sticks with its routes.** When two suppliers are about as close, the plan no longer swaps between
  them every few minutes (the copper shuttle swapped routes four times in 15 minutes).
- **Plans far ahead don't block trips.** Only trains arriving within five minutes count towards the queue limit;
  long plans booked stations 20 minutes ahead and new trips were turned away although trains hardly ever stood still.
- **Fewer trips to full platforms.** A platform that took much less than planned (it isn't drained by its belts as
  fast as the station's use suggested) only counts its free room for the next 30 minutes.

## 0.7.3 (beta)

- **Shuttles.** A route big enough to keep a whole train busy, like sulfur from Crater Lake to RockPow on the server,
  now gets a train whose cars all fit it, running only that route. One dock fills every car, instead of several
  trains each picking up a car or two and queueing for the station. Other trains leave that pickup and delivery to
  the shuttle unless the supplier is full. The roster shows shuttles in the log (ROSTER) and in the monitor.
- **Every stop costs a little**, more at a station other trains are using at the time. Before, a trip's value only
  counted how full its cars got, so three trains bringing a car each scored like one train bringing three, and each
  visit blocked the station for the others. Trips now gather more per stop and make fewer stops.
- **Trips that leave cars empty are preferred.** A car still holding cargo after a trip can only take that item
  next; a trip that empties a car counts that as a gain, one that leaves leftovers as a cost (less when the leftover
  is wanted somewhere soon). No train detours just to dump a few items.
- **Monitor: Congested instead of Fleet saturated.** A train standing still on a trip (queueing for a station or at
  signals) was counted as busy, so the monitor said to build more trains while the fleet queued at a few stations.
  Standing still is now shown apart, with a **Congested** line when it is 15 % of the time or more.

## 0.7.2 (beta)

- **The roster really stays steady.** 0.7.1 worked every duty out again whenever the flow plan's list of routes
  changed, which on the server was 50 times in 30 minutes while its measurements filled in. Now a route joins the
  roster once the flow plan has had it three times in a row; routes keep their trains and shares while the trains they
  need change by less than a quarter, and only what they need more is taken from spare trains. A new route doesn't move
  the other trains' duties. The log names only the trains whose duty changed.
- **The horizon planner spends its time better.** It only tries emptying a train's car for trains carrying cargo,
  and only for platforms that fall below target within 30 minutes, and its search stops after six tries that found
  nothing better. On the server it used its whole planning time in nearly every round.
- **Monitor:**
  - **Unreachable** platforms: a supply platform holding stock, or a demand platform that ran out, that no managed
    train has a car of its kind beside. Its goods are never picked up or delivered there; the row says which position
    needs a fluid or freight car.
  - **Fleet saturated**: shown when trains are on trips 85 % of the time or more; then items waiting for trains need
    more trains, not better planning.
  - **Requested** rows only for platforms that fall below target within 15 minutes, at most 10.

## 0.7.1 (beta)

- **New: steady roster** (Mods menu **Steady roster (beta)**, on, with the horizon planner). When rates hold steady,
  the best routing repeats like a timetable, so trains shouldn't keep switching what they do. The flow plan's routes
  are now shared out over the trains whose cars fit them, and each train runs its routes in turn, sharing a train
  between small routes. The roster stays the same while the routes and how many trains they need hardly change, and
  is logged (ROSTER) and shown per train in the monitor. The horizon planner only plans trains without a roster trip
  worth running (it delivers less than a quarter of a car) and those without a roster, to correct for what the
  roster doesn't cover.
- **The same situation gives the same plans.** The horizon planner's search no longer depends on the clock, and
  trains free at the same moment are taken in a fixed order.
- **Trips change less after a stop.** The rest of a trip is only replaced if the new plan is also worth clearly more
  (5 car-load minutes), so small changes in measured rates or travel times don't switch trips.

## 0.7.0 (beta)

- **New: horizon planner** (Mods menu **Horizon planner (beta)**, on). The classic planner gave each free train its
  best next trip, one train after another, so anything that paid off later needed a rule of its own. The horizon
  planner looks at every platform's stock over the next hour and plans all free trains together:
  - **What a delivery is worth** is how much it shortens the time platforms spend below their target over the next
    hour, weighted by priority and counted in car loads (a car of sulfur counts like a car of concrete). The sooner a
    delivery ends a shortfall, the more it is worth; one to a platform that stays above target is worth nothing. This
    replaces the old urgency and fairness weights, for both planners.
  - **Plans may hold several trips** (up to 6 stops), re-planned after every stop.
  - **A train can be emptied** to serve one of the platforms that will be shortest, when that does more good than
    its other trips.
  - **It searches**: each round it plans one or two trains again in another order and keeps the result if the
    network as a whole ends up less short.
  - **Classic planner**: switch Horizon planner off.
- **Requests stay as a fallback** (Mods menu **Request fallback**, on). With the horizon planner on, a request that
  fires is logged "missed by the horizon planner".
- **Requests: fixed a platform being skipped as "covered" when no train could reach it at all**, and requests are
  only planned for platforms that fall below target within the next half hour, which also saves planning time.
- **Production lost at a full supplier counts very little.** On a network where most suppliers are full and nobody
  needs their goods, hauling them away isn't worth a train.

## 0.6.10 (beta)

- **Requests, reworked.** A demand platform (**Request a train**, Automatic by default) now gets a train emptied
  for it when it needs one, not once it has run dry:
  - **Each platform asks on its own.** A station with several platforms for one item was judged as a whole, so one
    empty platform beside full ones never asked.
  - **It asks ahead.** It works out when its stock, with the deliveries already on the way to it, falls below its
    target at its use rate (On: below 90 %).
  - **Only if no other train makes it.** If a train with a car that lines up with it, empty or already holding the
    item, can bring it before then, busy or not, a normal trip does that and no train is emptied.
  - **Started in time, not too early.** A train is emptied for it once its request trip would otherwise deliver
    less than **Request a train this early** (Mods menu, 5 minutes) before the platform falls below its target;
    until then the train keeps working.
  - **Whatever empties the car soonest**, consumers and sinks alike: two nearby consumers that take it all beat a
    far sink, and a near sink beats two far consumers.
  - **The request's cars stay clean.** The cars a request uses only unload other items and then load the requested
    one, without a minimum batch. Before the pickup their contents are checked: transfers are only measured
    afterwards, so if some of the old item is left, the rest of the trip is planned again from what they hold, and
    the pickup is cancelled rather than mixing cargo. Other cars of the train may still load on the way.
  - **One trip can serve several requests** that the same train can deliver.
  - **A train serving a request** keeps it through saves, waits for its next request trip between them instead of
    repeating its last job, and shows in the monitor: Trains tab "Emptying for a request" or "Bringing a request",
    Platforms tab "Requested" rows with the train on it, or why there is none yet.
- **The platform settings window keeps its size.** It grew and shrank with what it showed, so it jumped when the
  role changed; it is now always the same height and its content scrolls.
- **Pickups wait for half a load by default.** **Send at % of a load** on supply platforms now defaults to 50 %
  instead of a full load. Platforms where you chose a value keep it. As before, a station 90 % full ships what it has,
  and a Critical consumer gets even a partial load.
- **No priority on supply platforms.** Priority only ever counted where cargo is delivered, so the button is gone
  from supply platforms and their readout no longer shows one.

## 0.6.9 (beta)

- **Demand platforms can ask for a train to be emptied for them** (new platform setting **Request a train**:
  Automatic, On, Off; click steps forward, right click back). A train whose car for that platform held another item
  was never sent: the pickup scored nothing until the car was empty, and emptying it was only planned when that
  delivery was worth something by itself. Now a platform that asks gets a free train: the trip first unloads the
  other item at its consumers, then at sinks if they can't take it all, then picks up and delivers. If that doesn't
  fit in one trip (4 stops), the train only empties its car first and is kept for the platform until it can bring
  it. The train's other cars may still load on the way. **Automatic** (default) asks when the platform has been
  empty for 30 s and nothing is on the way; **On** asks whenever it is below its target and nothing is on the way;
  **Off** never asks. The log has REQUEST lines, and the platform's readout says which train is emptying a car for
  it, or why none can.
- **Sink is a role of its own.** A sink platform takes any cargo no consumer wants; it was a demand platform with
  "Any item" picked. Existing ones become Sink when the save loads. "Any item" is no longer in the item picker.
- **Platform menu: one Role button** that steps through Off, Supply, Demand and Sink (right click goes back), and
  **Copy to the station's other platforms** moved down beside Copy and Paste. Copy, Paste and Copy to the station
  carry every setting of the platform, the request setting included.
- **Station menu: the depot switch is on the first tab**, under the station's name ("SmartTrains depot (rail yard
  for waiting trains)"). It was only at the bottom of the train and station lists.
- **Fixed: trains skipping a stop after a very short dock.** A dock with nothing to move lasts about 0.3 s, a few
  frames on a busy server, and SmartTrains checked docking every 0.25 s, so it sometimes missed one. It then wrote
  the next timetable a stop behind, and the train drove past a stop ("legs were missed" in the log). Docks are now
  seen when they start.
- **Quieter log for trains running their last trip again.** A repeat that moves nothing at a full platform is
  expected, so its "moved nothing" line is only in the detailed log.

## 0.6.8 (beta)

- **Queue limit counts trains, not seconds** (new setting **Most trains waiting before a station**, 2; it replaces
  **Longest wait on the line**). A trip is no longer planned when more than 2 trains, itself included, would wait on
  the line before a station. The old 60 s limit stacked each train's estimated stay and the arrival gap, so it really
  allowed one train ahead, and it was by far the most common reason trips were rejected on the server. A longer wait still makes a
  trip score lower, since it counts as trip time.
- **Removed: setting back a timetable the game moved on right after it was written.** It hasn't fired since jobs
  chosen while docked are written as the train leaves.

## 0.6.7 (beta)

- **A newly managed train gets its first trip at once, also when it stands docked at a station.** A docked train
  without a job only counted as free once the station's estimated stay had passed (about a minute), and then waited
  out its old timetable's stop rules. It is now free at once, and the stay ends as soon as it has a trip.
- **Monitor: the Platforms tab says which platforms.** Each row is one station's platforms for one item, judged
  together; it showed only the station's name. Under the name it now lists those platforms by position (P1 is the
  first from the station building, as in the log) with how full each is now, for example "4 platforms: P1 empty,
  P2 46%, P3 empty, P4 empty". The column is "Station and its platforms", and the tab's note explains the rows.
- **Monitor: "Partly starved".** A station counted as starved as soon as one of its platforms for an item was
  empty, so RockPow- Sulfur SNK showed "Starved" while another of its sulfur platforms held stock. "Starved" now
  means all of them were empty most of the time; "Partly starved" means some were empty while others held stock.
  The details give both: "some of its platforms were empty 86% of the time, all of them 10%".

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
