#ifndef AGENT_OBSERVE_H
#define AGENT_OBSERVE_H

#include "agent/json.h"

/** map [x y w h] [layer...] - map region as text rows, one character per tile */
void agent_observe_map(json_writer *w, const char *args);

/** buildings [type] - all non-house buildings, or only the given type (e.g. "prefecture") */
void agent_observe_buildings(json_writer *w, const char *args);

/** houses [x y w h] - houses with what they have and what they need to evolve */
void agent_observe_houses(json_writer *w, const char *args);

/** Machine-readable name of a building type, e.g. "engineers_post" (static buffer) */
const char *agent_building_type_name(int type);

/** Building type for a name accepted by `build`, or BUILDING_NONE (0) */
int agent_building_type_from_name(const char *name);

/** The `terrain` map layer character for a tile */
char agent_tile_char(int grid_offset);

#endif // AGENT_OBSERVE_H
