#ifndef AGENT_ROME_H
#define AGENT_ROME_H

#include "agent/json.h"

/** rome - rank, salary, personal savings, favour, gift costs and Caesar's open requests */
void agent_rome_status(json_writer *w);

/** salary <rank 0-10> - the governor's salary; above the player's rank costs favour */
void agent_rome_salary(json_writer *w, const char *args);

/** gift <modest|generous|lavish> - gift to Caesar from personal savings (+3/+5/+10 favour) */
void agent_rome_gift(json_writer *w, const char *args);

/** dispatch <request id> - send the goods/money Caesar asked for */
void agent_rome_dispatch(json_writer *w, const char *args);

#endif // AGENT_ROME_H
