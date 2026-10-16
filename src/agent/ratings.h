#ifndef AGENT_RATINGS_H
#define AGENT_RATINGS_H

#include "agent/json.h"

/** ratings - what each rating is made of and what would raise it (culture parts, prosperity factors, peace, favour) */
void agent_ratings(json_writer *w);

/** finance - income and expenses by category, this year so far and last year */
void agent_ratings_finance(json_writer *w);

#endif // AGENT_RATINGS_H
