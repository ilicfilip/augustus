#ifndef AGENT_CAMPAIGN_H
#define AGENT_CAMPAIGN_H

#include "agent/json.h"

/** victory <accept|continue_2|continue_5> - answer the "mission won" dialog */
void agent_campaign_victory(json_writer *w, const char *args);

/** next_mission [choice] - after a won mission: advance the campaign, pick a map if offered, start it */
void agent_campaign_next_mission(json_writer *w, const char *args);

/** test_win - force a win (only when AUGUSTUS_AGENT_TEST_MODE=1, for automated tests) */
void agent_campaign_test_win(json_writer *w);

#endif // AGENT_CAMPAIGN_H
