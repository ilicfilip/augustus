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
| `city` | `scenario`, `date`, `paused`, `finance`, `population`, `labour` (`workers_short` = unfilled jobs), `food` (stored, needed_per_month, supply_months, types_eaten, percentage_produced), `ratings`, `win_criteria` (enabled criteria only) |
| `map [x y w h] [layer…]` | `map_width/height`, region, `entry`/`exit` points, `layers[]` each with `name`, `legend`, `rows` (one string per y, one char per tile). Layers: `terrain` (default; terrain + building letters), `desirability` (tens), `water` (fountain/reservoir range) |
| `buildings [type]` | all non-house buildings (or only `type`, which may also be a house level): `id`, `type`, `x`, `y`, `size`, `workers`/`workers_needed`, `road_access`, `fire_risk`/`damage_risk`, `stock` (granary: units, 100 = 1 load; warehouse: cartloads), flags |
| `houses [x y w h]` | per house: `level`, `x`, `y`, `size`, `population`, `room`, `desirability`, `happiness`, water (`fountain`, `well`), `foods`, `stock`, `access` (service counters, 0 = none), `evolve_id` + `evolve` (the house window's "why it won't evolve" sentence), `worst_desirability_building` |
| `build <type> <x> <y> [x2 y2]` | places like the UI does (same costs, rules, warnings). `type` = names from `buildings` (`housing` for housing plots); `x2 y2` drags roads, aqueducts, walls, highways, reservoirs, housing, plazas, gardens. Reply: `ok` (something changed), `cost`, `treasury`, `warnings` (the game's messages, e.g. "Must build on cleared land"), `at_target` (building there, or `tile` letter) |
| `clear <x> <y> [x2 y2]` | clear land; same reply. Clearing forts, monuments or bridges needs a confirmation the agent cancels (error) |
| `undo` | undo the last build/clear while the game allows (shortly after building): `refund`, `treasury` |
| `screenshot [city\|minimap\|display]` | saves a PNG (default: whole city); `directory` = where; newest PNG there is it |
| `step [days=1]` | `days_requested`, `ticks_run`, `date`; `stopped_by: "window_changed"` + `window` if a popup/screen opened mid-step |
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
- `json.c` — minimal JSON writer (no external dependencies)
