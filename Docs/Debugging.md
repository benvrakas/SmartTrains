# Debugging SmartTrains

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
| `JOB n <train>: ...` | A job was handed out, with its score parts. |
| `WRITE <train> job n: [...]` | The timetable written, and whether through Freight Car Control. |
| `JOB n ...: docked at ... for leg i/n, +x s against plan` | Arrival, and how late against the plan. |
| `JOB n ...: left ... after x s, loaded/unloaded ...` | What was actually moved. |
| `JOB n ... DONE` | Delivered amount against plan, total time. |
| `TRIP A -> B took x s (estimate was y s ...)` | Travel time learning; big differences mean the speed estimate is off. |
| `PARK` | A train's autopilot was switched off where it stands. |
| `WOULD JOB` | Observe-only mode: the job it would have given. |
| `EXTERNAL TIMETABLE CHANGE` | Someone else changed a managed train's timetable; the job was written back. |
| `DOCK MAP MISMATCH` | A train docked with different cars beside the platforms than expected. The new map is kept. |
| `NO FIT` (VeryVerbose) | Why a train can't do a supplier → consumer pair. |
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

## Known gaps (0.2.0)

- A trip moves one item: milk runs combine stops of the same item, not different items.
- A train that is parked counts as standing at the station where it last docked.
