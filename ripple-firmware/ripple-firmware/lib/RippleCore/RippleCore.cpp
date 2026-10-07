#include "RippleCore.h"
#include <esp_now.h>
#include <WiFi.h>

static char s_nodeId[8];
static bool s_hasSensor;
static CorroborationEngine s_engine;
static RippleEvent s_pendingRelays[RIPPLE_MAX_EVENTS];
static uint32_t s_relayAtMs[RIPPLE_MAX_EVENTS];
static bool s_relayed[RIPPLE_MAX_EVENTS];
static size_t s_pendingCount = 0;

static RippleEvent s_lastEvent;
static bool s_hasLastEvent = false;

static bool s_newEventFlag = false;
static char s_reason[64] = "No events -> NORMAL";

static uint8_t BROADCAST_ADDR[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void recomputeReason() {
    uint64_t now = millis();
    uint8_t count = s_engine.getDistinctSourceCount(now);
    RippleStatus status = s_engine.evaluateStatus(now);

    if (status == RippleStatus::NORMAL) {
        snprintf(s_reason, sizeof(s_reason), "No events -> NORMAL");
    } else if (status == RippleStatus::UNVERIFIED) {
        snprintf(s_reason, sizeof(s_reason), "Distinct sources: %u -> UNVERIFIED", count);
    } else {
        snprintf(s_reason, sizeof(s_reason), "Distinct sources: %u -> CONFIRMED", count);
    }
}

static void scheduleRelay(const RippleEvent& incoming) {
    if (incoming.hopCount >= 10) return; // Max hop limit check

    if (s_pendingCount >= RIPPLE_MAX_EVENTS) {
        for (size_t i = 1; i < RIPPLE_MAX_EVENTS; ++i) {
            s_pendingRelays[i - 1] = s_pendingRelays[i];
            s_relayAtMs[i - 1] = s_relayAtMs[i];
            s_relayed[i - 1] = s_relayed[i];
        }
        s_pendingCount = RIPPLE_MAX_EVENTS - 1;
    }

    RippleEvent e = incoming;
    e.hopCount++; // Increment hop count on relay, sourceId & timestamp UNTOUCHED

    s_pendingRelays[s_pendingCount] = e;
    // Short random delay (5-15 ms) to avoid packet collision on relay
    s_relayAtMs[s_pendingCount] = millis() + random(5, 16);
    s_relayed[s_pendingCount] = false;
    s_pendingCount++;
}

static void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
    RippleEvent incoming;
    if (!RippleSerialization::deserializeFromJSONPayload(data, len, incoming)) {
        // Fallback: try direct struct copy if binary payload was transmitted
        if (len == (int)sizeof(RippleEvent)) {
            memcpy(&incoming, data, sizeof(RippleEvent));
        } else {
            return;
        }
    }

    if (incoming.sourceId[0] == '\0') return;

    if (s_engine.addEvent(incoming, millis())) {
        s_lastEvent = incoming;
        s_hasLastEvent = true;
        scheduleRelay(incoming);
        s_newEventFlag = true;
        recomputeReason();
    }
}

void rippleBegin(const char* nodeId, bool hasSensor) {
    strncpy(s_nodeId, nodeId, sizeof(s_nodeId) - 1);
    s_nodeId[sizeof(s_nodeId) - 1] = '\0';
    s_hasSensor = hasSensor;
    s_engine.clear();
    s_pendingCount = 0;
    s_hasLastEvent = false;

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
    if (!s_hasSensor) return; // Only sensor nodes originate witness reports

    RippleEvent e;
    strncpy(e.type, "obstruction", sizeof(e.type) - 1);
    strncpy(e.position, s_nodeId, sizeof(e.position) - 1);
    strncpy(e.sourceId, s_nodeId, sizeof(e.sourceId) - 1);
    e.timestamp = (uint64_t)millis();
    e.hopCount = 0;
    e.eventId = e.computeEventId();

    if (s_engine.addEvent(e, millis())) {
        s_lastEvent = e;
        s_hasLastEvent = true;
        s_newEventFlag = true;
        recomputeReason();
    }

    // Broadcast raw JSON payload via ESP-NOW
    String payload = RippleSerialization::serializeToJSONPayload(e);
    esp_now_send(BROADCAST_ADDR, (const uint8_t*)payload.c_str(), payload.length());
}

void rippleLoop() {
    uint64_t now = millis();
    s_engine.pruneOldEvents(now);
    recomputeReason();

    for (size_t i = 0; i < s_pendingCount; ++i) {
        if (s_relayed[i]) continue;
        if (now >= s_relayAtMs[i]) {
            String payload = RippleSerialization::serializeToJSONPayload(s_pendingRelays[i]);
            esp_now_send(BROADCAST_ADDR, (const uint8_t*)payload.c_str(), payload.length());
            s_relayed[i] = true;
        }
    }
}

RippleStatus rippleGetStatus() {
    return s_engine.evaluateStatus(millis());
}

uint8_t rippleGetDistinctSourceCount() {
    return s_engine.getDistinctSourceCount(millis());
}

const char* rippleGetReason() {
    return s_reason;
}

bool rippleHasNewEvent() {
    if (s_newEventFlag) {
        s_newEventFlag = false;
        return true;
    }
    return false;
}

CorroborationEngine& rippleGetEngine() {
    return s_engine;
}

const RippleEvent* rippleGetLastEvent() {
    return s_hasLastEvent ? &s_lastEvent : nullptr;
}
