#ifndef AGENT_ACT_H
#define AGENT_ACT_H

#include "agent/json.h"

/** build <type> <x> <y> [x2 y2] - place a building, or drag roads/aqueducts/walls/housing from x,y to x2,y2 */
void agent_act_build(json_writer *w, const char *args);

/** clear <x> <y> [x2 y2] - clear land in a rectangle */
void agent_act_clear(json_writer *w, const char *args);

/** undo - undo the last construction, if the game still allows it */
void agent_act_undo(json_writer *w);

#endif // AGENT_ACT_H
