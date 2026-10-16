#ifndef AGENT_LOGISTICS_H
#define AGENT_LOGISTICS_H

#include "agent/json.h"

/** depot <id> [<resource> <source id> <destination id> [always|never|source_over N|dest_under N]] */
void agent_logistics_depot(json_writer *w, const char *args);

/** storage_report - warehouses/granaries (space, contents, staffing) and producers whose goods are stuck */
void agent_logistics_storage_report(json_writer *w);

#endif // AGENT_LOGISTICS_H
