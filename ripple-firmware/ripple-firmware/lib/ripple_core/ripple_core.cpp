#include "ripple_core.h"

// --- CorroborationEngine Implementation ---

CorroborationEngine::CorroborationEngine(uint64_t matchWindowMs, uint64_t ttlMs)
    : m_count(0), m_matchWindowMs(matchWindowMs), m_ttlMs(ttlMs) {
    clear();
}

void CorroborationEngine::clear() {
    for (size_t i = 0; i < RIPPLE_MAX_EVENTS; ++i) {
        m_buffer[i].valid = false;
        m_buffer[i].receivedMs = 0;
    }
    m_count = 0;
}

int CorroborationEngine::findSlot(uint32_t eventId) const {
    for (size_t i = 0; i < m_count; ++i) {
        if (m_buffer[i].valid && m_buffer[i].event.getOrComputeEventId() == eventId) {
            return (int)i;
        }
    }
    return -1;
}

bool CorroborationEngine::hasSeen(uint32_t eventId) const {
    return findSlot(eventId) >= 0;
}

bool CorroborationEngine::hasSeen(const char* sourceId, uint64_t timestamp) const {
    RippleEvent dummy;
    strncpy(dummy.sourceId, sourceId, sizeof(dummy.sourceId) - 1);
    dummy.sourceId[sizeof(dummy.sourceId) - 1] = '\0';
    dummy.timestamp = timestamp;
    return hasSeen(dummy.computeEventId());
}

bool CorroborationEngine::addEvent(const RippleEvent& event, uint64_t nowMs) {
    if (nowMs == 0) nowMs = (uint64_t)millis();

    pruneOldEvents(nowMs);

    RippleEvent e = event;
    uint32_t id = e.getOrComputeEventId();
    e.eventId = id;

    // Deduplication check
    if (hasSeen(id)) {
        return false;
    }

    // Rolling buffer overwrite if full
    if (m_count >= RIPPLE_MAX_EVENTS) {
        for (size_t i = 1; i < RIPPLE_MAX_EVENTS; ++i) {
            m_buffer[i - 1] = m_buffer[i];
        }
        m_count = RIPPLE_MAX_EVENTS - 1;
    }

    m_buffer[m_count].event = e;
    m_buffer[m_count].receivedMs = nowMs;
    m_buffer[m_count].valid = true;
    m_count++;

    return true;
}

void CorroborationEngine::pruneOldEvents(uint64_t currentTime) {
    if (currentTime == 0) currentTime = (uint64_t)millis();

    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < m_count; ++readIdx) {
        if (!m_buffer[readIdx].valid) continue;

        // Check age against TTL
        uint64_t ageTs = (currentTime >= m_buffer[readIdx].event.timestamp) ? 
                         (currentTime - m_buffer[readIdx].event.timestamp) : 0;
        uint64_t ageRecv = (currentTime >= m_buffer[readIdx].receivedMs) ? 
                          (currentTime - m_buffer[readIdx].receivedMs) : 0;

        if (ageTs <= m_ttlMs && ageRecv <= m_ttlMs) {
            if (writeIdx != readIdx) {
                m_buffer[writeIdx] = m_buffer[readIdx];
            }
            writeIdx++;
        }
    }
    m_count = writeIdx;
    for (size_t i = m_count; i < RIPPLE_MAX_EVENTS; ++i) {
        m_buffer[i].valid = false;
    }
}

uint8_t CorroborationEngine::getDistinctSourceCount(uint64_t currentTime, uint64_t windowMs) const {
    if (currentTime == 0) currentTime = (uint64_t)millis();
    if (windowMs == 0) windowMs = m_matchWindowMs;

    char seenSources[RIPPLE_MAX_EVENTS][8];
    size_t uniqueCount = 0;

    for (size_t i = 0; i < m_count; ++i) {
        if (!m_buffer[i].valid) continue;

        uint64_t ageTs = (currentTime >= m_buffer[i].event.timestamp) ? 
                         (currentTime - m_buffer[i].event.timestamp) : 0;
        uint64_t ageRecv = (currentTime >= m_buffer[i].receivedMs) ? 
                          (currentTime - m_buffer[i].receivedMs) : 0;

        // Match within corroboration window
        if (ageTs <= windowMs || ageRecv <= windowMs) {
            const char* src = m_buffer[i].event.sourceId;
            if (src[0] == '\0') continue;

            bool alreadyCounted = false;
            for (size_t u = 0; u < uniqueCount; ++u) {
                if (strcmp(seenSources[u], src) == 0) {
                    alreadyCounted = true;
                    break;
                }
            }
            if (!alreadyCounted && uniqueCount < RIPPLE_MAX_EVENTS) {
                strncpy(seenSources[uniqueCount], src, sizeof(seenSources[uniqueCount]) - 1);
                seenSources[uniqueCount][sizeof(seenSources[uniqueCount]) - 1] = '\0';
                uniqueCount++;
            }
        }
    }

    return (uint8_t)uniqueCount;
}

RippleStatus CorroborationEngine::evaluateStatus(uint64_t currentTime, uint64_t windowMs) const {
    uint8_t distinctCount = getDistinctSourceCount(currentTime, windowMs);
    if (distinctCount >= 2) {
        return RippleStatus::CONFIRMED;
    } else if (distinctCount == 1) {
        return RippleStatus::UNVERIFIED;
    } else {
        return RippleStatus::NORMAL;
    }
}

size_t CorroborationEngine::getEventCount() const {
    return m_count;
}

// --- RippleSerialization Implementation ---

namespace RippleSerialization {

size_t serializeToJSONPayload(const RippleEvent& event, uint8_t* buffer, size_t maxLen) {
    JsonDocument doc;
    doc["type"] = event.type;
    doc["position"] = event.position;
    doc["timestamp"] = event.timestamp;
    doc["sourceId"] = event.sourceId;
    doc["hopCount"] = event.hopCount;
    doc["eventId"] = event.getOrComputeEventId();

    return serializeJson(doc, (char*)buffer, maxLen);
}

String serializeToJSONPayload(const RippleEvent& event) {
    JsonDocument doc;
    doc["type"] = event.type;
    doc["position"] = event.position;
    doc["timestamp"] = event.timestamp;
    doc["sourceId"] = event.sourceId;
    doc["hopCount"] = event.hopCount;
    doc["eventId"] = event.getOrComputeEventId();

    String out;
    serializeJson(doc, out);
    return out;
}

bool deserializeFromJSONPayload(const uint8_t* payload, size_t length, RippleEvent& event) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) return false;

    const char* typeStr = doc["type"] | "obstruction";
    const char* posStr = doc["position"] | "A";
    const char* srcStr = doc["sourceId"] | "";

    strncpy(event.type, typeStr, sizeof(event.type) - 1);
    event.type[sizeof(event.type) - 1] = '\0';

    strncpy(event.position, posStr, sizeof(event.position) - 1);
    event.position[sizeof(event.position) - 1] = '\0';

    event.timestamp = doc["timestamp"] | 0ULL;

    strncpy(event.sourceId, srcStr, sizeof(event.sourceId) - 1);
    event.sourceId[sizeof(event.sourceId) - 1] = '\0';

    event.hopCount = doc["hopCount"] | 0;
    event.eventId = doc["eventId"] | event.computeEventId();

    return true;
}

bool deserializeFromJSONPayload(const String& payloadStr, RippleEvent& event) {
    return deserializeFromJSONPayload((const uint8_t*)payloadStr.c_str(), payloadStr.length(), event);
}

String serializeToJSONLine(const RippleEvent* lastEvent, RippleStatus status, const char* nodeId, uint8_t distinctSources) {
    JsonDocument doc;
    doc["type"] = lastEvent ? lastEvent->type : "obstruction";
    doc["position"] = nodeId ? nodeId : "D";
    doc["timestamp"] = lastEvent ? lastEvent->timestamp : (uint64_t)millis();
    doc["sourceId"] = lastEvent ? lastEvent->sourceId : "";
    doc["status"] = (status == RippleStatus::CONFIRMED) ? "CONFIRMED" :
                    (status == RippleStatus::UNVERIFIED) ? "UNVERIFIED" : "NORMAL";
    doc["distinctSources"] = distinctSources;

    String out;
    serializeJson(doc, out);
    out += "\n";
    return out;
}

} // namespace RippleSerialization

