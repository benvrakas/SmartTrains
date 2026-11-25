# Debugging SmartTrains

**Detailed log** (Mods menu) shows the Verbose lines below without any console command; turn it on before
reproducing a problem. For even more (every move and fit check), use the console: `log LogSmartTrains VeryVerbose`.

The log is `%LOCALAPPDATA%\FactoryGame\Saved\Logs\FactoryGame.log`; every SmartTrains line starts with
`LogSmartTrains:`. More detail: add `LogSmartTrains Verbose` (or `VeryVerbose`, which includes every rejected fit) under
`[Core.Log]` in `%LOCALAPPDATA%\FactoryGame\Saved\Config\Windows\Engine.ini`, or type `log LogSmartTrains Verbose` in
the console.

## Lines to look for

| Line | Meaning |
|---|---|
| `Subsystem ready` | Loaded; counts and whether Freight Car Control / DynamicTimeTable were found. |
| `PLATFORM ... -> ...` | A platform's role or items changed, and by whom. |
| `MANAGE` / `UNMANAGE` | A train was taken over or given back (saved stop count, autopilot). |
| `FACTORY <station>: uses ... /min, makes ... /min` | A factory scan for the loop plan: the station's most rates, what limits them. |
| `SPLIT n trains over m consumers: ...` | A new split of the trains: each consumer's trains and need. Only when its inputs change. |
| `LOOP <train>: serves <station> (was ...)` / `no loop: <why>` | A train's consumer changed. |
| `JOB n <train>: loop ...` / `switching patterns ...` | A train's job: its loop, or its Deliver or Sink stop first. |
| `WRITE <train> job n: [...]` | The timetable written, and whether through Freight Car Control. |
| `JOB n ...: docked at ..., stop i of n` | Arrival at a stop and what it moves there. |
| `JOB n ...: left ... after x s: ...` | What was actually moved. |
| `LOOP <train>: round n done` | A loop came round to its first stop again. |
| `JOB n ... DONE` | A Deliver or Sink job ended. |
| `TRIP A -> B took x s (estimate was y s ...)` | Travel time learning; big differences mean the speed estimate is off. |
| `PARK` | A train without a loop got its autopilot switched off where it stands, with the reason. |
| `WOULD JOB` | Observe-only mode: the job it would have given. |
| `FLEET ...` | Once a minute: managed trains moving loaded or empty, docked, standing, switching patterns, without a loop. |
| `MONITOR ...` | Every 5 minutes: the monitor report (consumers, trains needing a look). |
| `LOOPS model: ...` (Verbose) | The consumers and supplier items of the loop plan. |
| `Settings: ...` / `Settings changed: Key=value (was ...)` | All Mods menu settings at start, then every change. |
| `TRACE <platform> upstream/downstream: ...` (Verbose) | The migration tool's belt and pipe trace of a platform. |
| `EXTERNAL TIMETABLE CHANGE` | Someone else changed a managed train's timetable; the job was written back. |
| `DOCK MAP MISMATCH` | A train docked with different cars beside the platforms than expected. The new map is kept. |
| `SELFDRIVING <train>: <error> for N s` | A self-driving error that lasted 5 s, and `SDLE_NoError` when it clears. |
| `PLAYER <train>: ...` | A player got into or out of the driver's seat; driving by hand takes it off its loop until autopilot is on. |
| `MIGRATION` | Preview, apply and undo summaries; the CSV path. |
| `Test copy` / `Backup` | Save copies and the session name being put back. |

## Menus

- `UI hook ... added` at start-up, and `4 of 4 UI hooks registered`. A missing vanilla widget or panel name is logged
  as an error: the game's UI changed.
- `USTPlatformPanel shown in ...` / `Platform panel geometry: ...` when a platform menu opens. If the panel is drawn
  outside the window or cut off, the geometry line shows where it landed.
- `Platform menu: role on ... (edit N)`, `Locomotive menu: ...`, `Station menu: ...`, `Opening the migration window` / `Migration window opened|closed`, `Migration window: step ...`
  on the client. The server logs the change itself (`PLATFORM`, `MANAGE`, `STATION`, ...) with the player's name.
- `Remote call object not registered on the local player`: SmartTrains isn't on the server. Its menus stay empty.
- `Clipboard hooks installed` at start-up. `Game clipboard: copy from ...` / `paste onto ...` on the player's side
  when the game's copy/paste-settings is used on a platform, and `CLIPBOARD <player> copied|pasted ...` on the
  server. No `Game clipboard` line after the game's copy key on a platform means the game doesn't route that key
  through `AFGPlayerState::CopyFactoryClipboard` for platforms; the panel's Copy / Paste still work.
- **SML slot setup:** `UWidgetBlueprintHookSlot_Generic::SetupPanelSlot` dereferences a null `HorizontalBoxSlot` when
  the parent is a vertical box (SML 3.12). So hooks into vertical boxes pass no slot setup, and the panels pad
  themselves (`USTPanel::GetOuterPadding`).

## Known gaps

- A station that both supplies and takes the same item isn't supported.
- A loop uses the first dock map at each station: a push-pull train arriving the other way round puts other cars
  beside the platforms. The mismatch is logged and learned.
- Supplier choices follow the travel times as they are learned, so a train may change supplier (not consumer) when
  the nearest supplier changes.
