/*
 * agent.h – Built-in snake agent interface.
 */

#ifndef SNAKE_AGENT_H
#define SNAKE_AGENT_H

#include "game.h"

/**
 * Naive agent: picks a random safe turn (one that doesn't cause
 * immediate death).  Falls back to TURN_STRAIGHT if trapped.
 */
Turn naive_agent_decide(const SnakeGame *g);

#endif /* SNAKE_AGENT_H */
