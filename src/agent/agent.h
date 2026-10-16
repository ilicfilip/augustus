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

#endif // AGENT_AGENT_H
