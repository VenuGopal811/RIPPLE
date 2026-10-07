#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// RIPPLE shared protocol core.
//
// THE ONE RULE THIS FILE EXISTS TO ENFORCE:
// No node ever sends a "confirmed" flag or references another node's ID.
// A node only ever broadcasts its own original witness report (sourceId is
// always its own identity, "A" or "B"). Confirmation is always computed
// locally, by counting distinct sourceIds seen for the same hazard. This
// file is the single place that logic lives, every node links against it
// unmodified, so the rule can never drift between boards.
// ---------------------------------------------------------------------------

#define RIPPLE_MAX_EVENTS 24
#define RIPPLE_TTL_MS 20000UL       // events older than this are forgotten
#define RIPPLE_MATCH_WINDOW_MS 15000UL  // corroboration time window

struct RippleEvent {
  char type[16];        // e.g. "obstruction"
  char position[4];     // where it was detected, "A" or "B"
  uint32_t originMs;    // millis() at the ORIGIN node when first witnessed
  char sourceId[4];     // "A" or "B" ONLY, the witness identity. Never changes on relay.
  bool valid;
};

enum RippleStatus {
  RIPPLE_NORMAL,
  RIPPLE_UNVERIFIED,   // exactly one distinct source seen
  RIPPLE_CONFIRMED     // two or more distinct sources seen
};

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
