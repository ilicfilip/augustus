#ifndef AGENT_LAYOUT_H
#define AGENT_LAYOUT_H

#include "agent/json.h"

/** walker_preview <type> <x> <y> - where a service building's walkers would go (existing or hypothetical) */
void agent_layout_walker_preview(json_writer *w, const char *args);

/** layout_review [x y w h] - houses each service misses, junction leaks, roadblocks blocking their own services */
void agent_layout_review(json_writer *w, const char *args);

#endif // AGENT_LAYOUT_H
