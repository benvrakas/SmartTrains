# SmartTrains

A Satisfactory mod (SML 3.12.1, game build 520140 or later): every consumer station gets trains that carry its car
pattern, looping between its suppliers and it.

Give freight and fluid platforms a role (supply, demand or sink) and the items they handle. A consumer station's
unloading platforms make its pattern, the item of each car. Managed trains are shared out over the consumers in
proportion to what each one needs (its factory's most rate times its loop's round trip), and each runs one loop: fill
at the suppliers, unload at the consumer, over and over. Everything is opt-in: unmanaged trains and platforms without
a role work as in vanilla.

Status: 0.50.0 beta ([changelog](Docs/Release/Changelog.md)). Settings are in the platform, locomotive and station
menus, and in chat (`/st help`).

- Commands: [Docs/Commands.md](Docs/Commands.md)
- Moving an existing network over: [Docs/Migration.md](Docs/Migration.md)
- How it works: [Docs/Architecture.md](Docs/Architecture.md)
- Logs and troubleshooting: [Docs/Debugging.md](Docs/Debugging.md)
- Tests: [Docs/Testing.md](Docs/Testing.md)
- Notes from the unreleased betas: [Docs/DevLog.md](Docs/DevLog.md)

Highly recommended (both optional): Auto Trains (traffic without signals; its travel times feed the loops' round
trips) and Freight Car Control 1.1.0+ (cars skip the stops that aren't theirs). Also works with Modular Stations.
DynamicTimeTable's station groups are read for migration; don't keep it installed with managed trains.
