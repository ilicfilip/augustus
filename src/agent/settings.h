#ifndef AGENT_SETTINGS_H
#define AGENT_SETTINGS_H

#include "agent/json.h"

/** set tax <0-25> | set wages <0-100> | set priority <category> <0-9> */
void agent_settings_set(json_writer *w, const char *args);

/** Labour categories with workers needed/allocated and priority, for the `city` reply */
void agent_settings_write_labour_categories(json_writer *w);

/** storage <id> <resource> <accept|get|refuse|keep> [quantity] | storage <id> permission <name> <on|off> */
void agent_settings_storage(json_writer *w, const char *args);

/** market <id> <resource> <on|off> - which goods a market (or other distributor) fetches */
void agent_settings_market(json_writer *w, const char *args);

/** roadblock <id> <walker> <on|off> - which roaming walkers may pass a roadblock or gate */
void agent_settings_roadblock(json_writer *w, const char *args);

/** message - title and text of the open message popup */
void agent_settings_message(json_writer *w);

/** save <name> - save the game; names must start with "agent_" (or be an absolute path) */
void agent_settings_save(json_writer *w, const char *args);

#endif // AGENT_SETTINGS_H
