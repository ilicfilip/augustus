#ifndef AGENT_LOGISTICS_H
#define AGENT_LOGISTICS_H

#include "agent/json.h"

/** depot <id> [<resource> <source id> <destination id> [always|never|source_over N|dest_under N]] */
void agent_logistics_depot(json_writer *w, const char *args);

/** storage_report - warehouses/granaries (space, contents, staffing) and producers whose goods are stuck */
void agent_logistics_storage_report(json_writer *w);

/** supply <distributor id> [<resource>] - why a market (temple, tavern...) does or doesn't get each good */
void agent_logistics_supply(json_writer *w, const char *args);

#endif // AGENT_LOGISTICS_H
