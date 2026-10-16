# Agent control server

Lets an external program (e.g. an MCP server driving an AI agent) observe and control the game.

- Start the game with `--agent-port PORT`; it listens on `127.0.0.1:PORT` only. Without the flag nothing changes.
- One client at a time. Commands run on the main thread between frames (`agent_poll()` in the SDL2 main loop).
- Protocol: one text command per line in, one JSON object per line out. Every reply has `command` and
  `ok`; failures add `error`.
- The game is **paused** while a client is connected; time only advances via `step`.
- 50 ticks = 1 day, 16 days = 1 month, 12 months = 1 year.
- SDL2 platform only for now (`src/platform/SDL2/augustus.c`); the SDL3 entry point is not hooked up.
  Windows builds get no-op stubs.

## Commands

| Command | Reply fields |
|---|---|
| `ping` | `version`, `window` {id, name}, `in_city`, `paused` |
| `city` | `scenario`, `date`, `paused`, `victory` (none/won/lost), `finance`, `population`, `labour` (`workers_short` = unfilled jobs), `food` (stored, needed_per_month, supply_months, types_eaten, percentage_produced), `ratings`, `win_criteria` (enabled criteria only), `labour_categories` (needed/allocated/priority) |
| `map [x y w h] [layer…]` | `map_width/height`, region, `entry`/`exit` points, `layers[]` each with `name`, `legend`, `rows` (one string per y, one char per tile). Layers: `terrain` (default; terrain + building letters), `desirability` (tens), `water` (fountain/reservoir range) |
| `buildable` | building types available in this mission: `type` (name accepted by `build`), `cost`, `size`, `workers` |
| `buildings [type]` | all non-house buildings (or only `type`, which may also be a house level): `id`, `type`, `x`, `y`, `size`, `workers`/`workers_needed`, `road_access`, `fire_risk`/`damage_risk`, `stock` (granary: units, 100 = 1 load; warehouse: cartloads), flags |
| `houses [x y w h]` | per house: `level`, `x`, `y`, `size`, `population`, `room`, `desirability`, `happiness`, water (`fountain`, `well`), `foods`, `stock`, `access` (service counters, 0 = none), `evolve_id` + `evolve` (the house window's "why it won't evolve" sentence), `worst_desirability_building` |
| `build <type> <x> <y> [x2 y2]` | places like the UI does (same costs, rules, warnings). `type` = names from `buildings` (`housing` for housing plots); `x2 y2` drags roads, aqueducts, walls, highways, reservoirs, housing, plazas, gardens. Reply: `ok` (something changed), `cost`, `treasury`, `warnings` (the game's messages, e.g. "Must build on cleared land"), `at_target` (building there, or `tile` letter) |
| `clear <x> <y> [x2 y2]` | clear land; same reply. Clearing forts, monuments or bridges needs a confirmation the agent cancels (error) |
| `undo` | undo the last build/clear while the game allows (shortly after building): `refund`, `treasury` |
| `set tax <0-25>` / `set wages <0-100>` / `set priority <category> <0-9>` | new values; priority replies with `labour_categories` (0 = no priority) |
| `storage <id> [<resource> <accept\|get\|refuse\|keep> [4-32]]` | warehouse/granary orders (quantity in cartloads, steps of 4; `keep` = "retaining", only cart depots take from it) → `orders` (non-refused resources: "state quantity"), `permissions` |
| `storage <id> permission <name> <on\|off>` | who may take goods: market, traders, dock, quartermaster, worker, barkeep, caravanserai, lighthouse, armoury, workcamp, natives, caesar |
| `market <id> [<resource> <on\|off>]` | which goods a market (or other distributor) fetches → `fetches` |
| `roadblock <id> [<walker> <on\|off>]` | which roaming walkers pass (maintenance, priest, market, entertainer, education, medicine, tax_collector, labor_seeker, missionary, watchman) → `lets_through`; new roadblocks block all |
| `message` | the open message popup: `type`, `urgent`, `year`, `month`, `param1/2` (shown beside the text by the game, e.g. request amounts), `title`, `subtitle`, `text` (plain text) |
| `save <agent_name>` | saves `agent_name.svx` in the savegame directory (name must start with `agent_`, so the player's saves are never overwritten) or to an absolute path → `file` |
| `screenshot [city\|minimap\|display]` | saves a PNG (default: whole city); `directory` = where; newest PNG there is it |
| `step [days=1]` | `days_requested`, `ticks_run`, `date`; `stopped_by: "window_changed"` + `window` if a popup/screen opened mid-step |
| `play <days> [speed]` | runs the game **in real time** through the normal game loop at `speed` % (10-100 step 10, 200, 300, 500; default 100 ≈ 0.8 s/day) so a person can watch; answers only when done (same fields as `step`) or when a popup opens (`stopped_by`). Commands sent meanwhile are queued. Pauses again afterwards |
| `load <savegame>` | `scenario`, `date` — loads from the savegame directory (or an absolute path), shows the city, pauses |
| `close` | closes a message dialog / message list / building info window and returns to the city |
| `pause` / `resume` | `paused` |
| `help` | `commands` |

`step` stops early whenever the city stops being the active window (e.g. a news message pops up).
It gives every simulated tick its own millisecond on the game clock (house evolution and some caches
skip work "already done this millisecond"), and the main loop adds `agent_time_offset()` so the game
clock stays monotonic afterwards.

Runs are **not fully reproducible**: Augustus seeds some gameplay randomness from the wall clock
(`random_from_stdlib` in `core/random.c`: entertainer venues, crime, strikes, requests, invasions).
Typical loop: `step N` → if `stopped_by`, inspect, `close`, step the remaining days.

`load` resets the view orientation to north so multi-tile placement is always anchored at the top-left tile.
Buildings placed this tick are listed (state "created") before the next tick makes them active.

Coordinates are map tiles (0..map_width-1, 0..map_height-1), the same in all commands. A building's
`x`,`y` is its top-left tile in that grid.

## Example

```
$ printf 'load Engineer.svx\nstep 30\ncity\n' | nc 127.0.0.1 7777
{"command":"load","ok":true,"scenario":"map3","date":{"year":-348,"month":5,"day":10}}
{"command":"step","ok":true,"days_requested":30,"ticks_run":1500,"date":{...}}
{"command":"city","ok":true,...}
```

## Files

- `agent.c` — socket handling and commands
- `observe.c` — `map`, `buildings`, `houses`, building type names
- `act.c` — `build`, `clear`, `undo`
- `settings.c` — `set`, `storage`, `market`, `roadblock`, `message`, `save`

Hooks outside `src/agent/`: `platform/SDL2/augustus.c` (poll, clock offset, shutdown after `game_exit`),
`platform/arguments.*` (`--agent-port`), `window/message_dialog.*` (`window_message_dialog_current()`
read-only accessor), `game/game.c` (no settings/config saving in agent mode), `game/file.c` (no mission-start
autosave in agent mode — it is named after the player's rank and would overwrite e.g. `Architect.svx`),
CMakeLists.

Agent mode never writes the player's files: settings (`c3.inf`, `augustus.ini`) aren't saved on exit (so
`--windowed` doesn't become the player's display setting), there is no mission-start autosave, and `save`
only accepts `agent_*` names. `build` moves the camera to what was built, for anyone watching.

Not handled yet: mission briefing (new mission start) and mission end screens — `step` stops with the
`mission_end` window and `city.victory` says won/lost, but the agent can't continue to the next mission.
- `json.c` — minimal JSON writer (no external dependencies)
