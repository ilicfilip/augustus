#ifndef AGENT_TRADE_H
#define AGENT_TRADE_H

#include "agent/json.h"

/**
 * trade - overview of resources (stock, settings, prices) and trade partners (routes, quotas)
 * trade <resource> <import|export|stockpile|mothball> <on|off>
 * trade <resource> <import_up_to|export_over> <0-100>
 */
void agent_trade(json_writer *w, const char *args);

/** open_route <city id> - open a trade route like the empire screen does */
void agent_trade_open_route(json_writer *w, const char *args);

#endif // AGENT_TRADE_H
