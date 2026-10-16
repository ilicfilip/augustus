#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

/**
 * Agent control interface: a localhost TCP server that lets an external program (e.g. an MCP server)
 * observe and control the game. One text command per line in, one JSON object per line out.
 * Disabled unless started with --agent-port.
 */

/**
 * Starts listening on 127.0.0.1:port
 * @return 1 on success, 0 on failure
 */
int agent_init(int port);

/**
 * @return 1 if the agent server is running
 */
int agent_is_enabled(void);

/**
 * Accepts connections and executes pending commands. Call once per frame from the main thread.
 */
void agent_poll(void);

void agent_shutdown(void);

/**
 * Milliseconds the agent has moved the game clock ahead of real time. `step` gives every simulated tick
 * its own clock value (some simulation code caches or skips work "per millisecond"), so the main loop
 * must add this to real time to keep the game clock monotonic.
 */
unsigned long long agent_time_offset(void);

#endif // AGENT_AGENT_H
