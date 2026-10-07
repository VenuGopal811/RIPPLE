#include "RippleCore.h"
#include <esp_now.h>
#include <WiFi.h>

static char s_nodeId[4];
static bool s_hasSensor;
static RippleEvent s_events[RIPPLE_MAX_EVENTS];
static uint8_t s_eventCount = 0;
static bool s_relayed[RIPPLE_MAX_EVENTS];
static uint32_t s_relayAtMs[RIPPLE_MAX_EVENTS];
static bool s_newEventFlag = false;
static char s_reason[64] = "No events -> NORMAL";

static uint8_t BROADCAST_ADDR[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// --- internal helpers -------------------------------------------------

static int findEventIndex(const RippleEvent& e) {
  for (uint8_t i = 0; i < s_eventCount; i++) {
    if (s_events[i].valid &&
        strcmp(s_events[i].sourceId, e.sourceId) == 0 &&
        s_events[i].originMs == e.originMs) {
      return i;
    }
  }
  return -1;
}

static void pruneExpired() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_eventCount; i++) {
    if (s_events[i].valid && (now - s_events[i].originMs) > RIPPLE_TTL_MS) {
      s_events[i].valid = false;
    }
  }
}

static void recomputeReason() {
  // Count distinct sourceIds among currently valid events within the
  // corroboration window. This is the ONLY place "confirmed" is decided,
  // and it only ever looks at sourceId identity, never at relay hop count.
  bool sawA = false, sawB = false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_eventCount; i++) {
    if (!s_events[i].valid) continue;
    if ((now - s_events[i].originMs) > RIPPLE_MATCH_WINDOW_MS) continue;
    if (strcmp(s_events[i].sourceId, "A") == 0) sawA = true;
    if (strcmp(s_events[i].sourceId, "B") == 0) sawB = true;
  }

  uint8_t count = (sawA ? 1 : 0) + (sawB ? 1 : 0);
  if (count == 0) {
    snprintf(s_reason, sizeof(s_reason), "No events -> NORMAL");
  } else if (count == 1) {
    snprintf(s_reason, sizeof(s_reason), "Sources seen: %s only -> UNVERIFIED", sawA ? "A" : "B");
  } else {
    snprintf(s_reason, sizeof(s_reason), "Sources seen: A, B -> CONFIRMED");
  }
}

static void storeAndMaybeRelay(const RippleEvent& incoming) {
  if (findEventIndex(incoming) >= 0) return; // dedup: already have this exact witness report

  if (s_eventCount >= RIPPLE_MAX_EVENTS) {
    // simple ring overwrite of the oldest slot, fine for a 4-node demo
    for (uint8_t i = 1; i < s_eventCount; i++) s_events[i - 1] = s_events[i];
    s_eventCount--;
  }

  s_events[s_eventCount] = incoming;
  s_relayed[s_eventCount] = false;
  // small randomized delay avoids every node rebroadcasting in the same
  // instant, and gives the visualization a visible, deliberate ripple
  s_relayAtMs[s_eventCount] = millis() + 400 + random(0, 300);
  s_eventCount++;

  s_newEventFlag = true;
  recomputeReason();
}

static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(RippleEvent)) return;
  RippleEvent incoming;
  memcpy(&incoming, data, sizeof(RippleEvent));
  if (!incoming.valid) return;
  storeAndMaybeRelay(incoming);
}

// --- public API ---------------------------------------------------------

void rippleBegin(const char* nodeId, bool hasSensor) {
  strncpy(s_nodeId, nodeId, sizeof(s_nodeId) - 1);
  s_hasSensor = hasSensor;

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[RippleCore] ESP-NOW init failed");
    return;
  }
  esp_now_recv_cb_t recvCb = reinterpret_cast<esp_now_recv_cb_t>(onEspNowRecv);
  esp_now_register_recv_cb(recvCb);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void rippleFireLocalWitness() {
  if (!s_hasSensor) return; // only A and B may originate a witness report
  RippleEvent e;
  strncpy(e.type, "obstruction", sizeof(e.type));
  strncpy(e.position, s_nodeId, sizeof(e.position));
  strncpy(e.sourceId, s_nodeId, sizeof(e.sourceId));
  e.originMs = millis();
  e.valid = true;
  storeAndMaybeRelay(e);
  // broadcast immediately, this node's own origin report goes out right away,
  // relay of OTHER nodes' reports still uses the randomized delay above
  esp_now_send(BROADCAST_ADDR, (uint8_t*)&e, sizeof(e));
}

void rippleLoop() {
  pruneExpired();
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_eventCount; i++) {
    if (!s_events[i].valid || s_relayed[i]) continue;
    if (now >= s_relayAtMs[i]) {
      esp_now_send(BROADCAST_ADDR, (uint8_t*)&s_events[i], sizeof(RippleEvent));
      s_relayed[i] = true;
    }
  }
}

RippleStatus rippleGetStatus() {
  bool sawA = false, sawB = false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_eventCount; i++) {
    if (!s_events[i].valid) continue;
    if ((now - s_events[i].originMs) > RIPPLE_MATCH_WINDOW_MS) continue;
    if (strcmp(s_events[i].sourceId, "A") == 0) sawA = true;
    if (strcmp(s_events[i].sourceId, "B") == 0) sawB = true;
  }
  if (sawA && sawB) return RIPPLE_CONFIRMED;
  if (sawA || sawB) return RIPPLE_UNVERIFIED;
  return RIPPLE_NORMAL;
}

uint8_t rippleGetDistinctSourceCount() {
  RippleStatus s = rippleGetStatus();
  if (s == RIPPLE_CONFIRMED) return 2;
  if (s == RIPPLE_UNVERIFIED) return 1;
  return 0;
}

const char* rippleGetReason() { return s_reason; }

bool rippleHasNewEvent() {
  if (s_newEventFlag) { s_newEventFlag = false; return true; }
  return false;
}
