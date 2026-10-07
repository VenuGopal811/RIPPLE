#pragma once
#include <Arduino.h>
#include "ripple_core.h"

// Legacy / C-style aliases for compatibility
#define RIPPLE_NORMAL RippleStatus::NORMAL
#define RIPPLE_UNVERIFIED RippleStatus::UNVERIFIED
#define RIPPLE_CONFIRMED RippleStatus::CONFIRMED

// Call once in setup().
void rippleBegin(const char* nodeId, bool hasSensor);

// Call every loop(). Handles TTL cleanup, pending relay sends, and
// re-broadcasting events this node hasn't relayed yet.
void rippleLoop();

// Sensor/button nodes (A, B) call this when their own sensor fires.
// Does nothing on a relay-only node.
void rippleFireLocalWitness();

// Current corroboration state for the active hazard, and how many
// distinct sourceIds support it (1 = unverified, 2 = confirmed).
RippleStatus rippleGetStatus();
uint8_t rippleGetDistinctSourceCount();

// Human-readable reason string, for Serial logging and the gateway's JSON
// bridge, e.g. "Sources seen: A only -> UNVERIFIED".
const char* rippleGetReason();

// True once per new event, for one loop() iteration, so main.cpp can react
// (buzzer, LED, status print) without polling every event itself.
bool rippleHasNewEvent();

// Access the global CorroborationEngine instance
CorroborationEngine& rippleGetEngine();

// Access the most recently received/processed event
const RippleEvent* rippleGetLastEvent();
