#ifndef RIPPLE_CORE_H
#define RIPPLE_CORE_H

#include <Arduino.h>
#include <ArduinoJson.h>

#define RIPPLE_MAX_EVENTS 24
#define RIPPLE_DEFAULT_TTL_MS 20000ULL
#define RIPPLE_DEFAULT_MATCH_WINDOW_MS 5000ULL

// Corroboration status enum
enum class RippleStatus {
    NORMAL = 0,
    UNVERIFIED = 1,
    CONFIRMED = 2
};

// Event Data Schema matching DESIGN.md
struct RippleEvent {
    char type[16];        // Default: "obstruction"
    char position[8];     // Location, e.g. "A"
    uint64_t timestamp;   // Timestamp in milliseconds
    char sourceId[8];     // "A" or "B" only (witness identity)
    uint8_t hopCount;     // Incremented at each relay hop
    uint32_t eventId;     // Unique identifier hash for deduplication and TTL

    RippleEvent() : timestamp(0), hopCount(0), eventId(0) {
        strncpy(type, "obstruction", sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';
        strncpy(position, "A", sizeof(position) - 1);
        position[sizeof(position) - 1] = '\0';
        strncpy(sourceId, "", sizeof(sourceId) - 1);
        sourceId[sizeof(sourceId) - 1] = '\0';
    }

    // Helper to compute FNV-1a hash for unique event ID if eventId is 0
    uint32_t computeEventId() const {
        uint32_t hash = 2166136261UL;
        for (const char* p = sourceId; *p; ++p) {
            hash ^= (uint8_t)(*p);
            hash *= 16777619UL;
        }
        for (const char* p = position; *p; ++p) {
            hash ^= (uint8_t)(*p);
            hash *= 16777619UL;
        }
        for (const char* p = type; *p; ++p) {
            hash ^= (uint8_t)(*p);
            hash *= 16777619UL;
        }
        uint64_t ts = timestamp;
        for (int i = 0; i < 8; ++i) {
            hash ^= (uint8_t)(ts & 0xFF);
            hash *= 16777619UL;
            ts >>= 8;
        }
        return (hash == 0) ? 1 : hash;
    }

    uint32_t getOrComputeEventId() const {
        return (eventId != 0) ? eventId : computeEventId();
    }
};

// Corroboration Engine class
class CorroborationEngine {
public:
    CorroborationEngine(uint64_t matchWindowMs = RIPPLE_DEFAULT_MATCH_WINDOW_MS,
                        uint64_t ttlMs = RIPPLE_DEFAULT_TTL_MS);

    // Adds event to rolling buffer if not already seen (deduplication).
    // Returns true if event is new and successfully added, false if duplicate.
    bool addEvent(const RippleEvent& event, uint64_t nowMs = 0);

    // Prunes events older than ttlMs relative to currentTime.
    void pruneOldEvents(uint64_t currentTime);

    // Evaluates status based on unique sourceIds within match window relative to currentTime.
    RippleStatus evaluateStatus(uint64_t currentTime, uint64_t windowMs = 0) const;

    // Counts unique sourceId values ("A", "B", etc.) within matching time window.
    uint8_t getDistinctSourceCount(uint64_t currentTime, uint64_t windowMs = 0) const;

    // Check if an eventId or (sourceId, timestamp) has already been seen.
    bool hasSeen(uint32_t eventId) const;
    bool hasSeen(const char* sourceId, uint64_t timestamp) const;

    // Get number of currently active events stored.
    size_t getEventCount() const;

    // Clear all stored events.
    void clear();

private:
    struct Entry {
        RippleEvent event;
        uint64_t receivedMs;
        bool valid;
    };

    Entry m_buffer[RIPPLE_MAX_EVENTS];
    size_t m_count;
    uint64_t m_matchWindowMs;
    uint64_t m_ttlMs;

    int findSlot(uint32_t eventId) const;
};

// Helper Functions for JSON serialization (ESP-NOW payload & Web Serial line)
namespace RippleSerialization {
    // Serialize RippleEvent to JSON byte payload (for ESP-NOW transmission)
    // CRITICAL CONSTRAINT: Payload ONLY contains raw witness report fields (type, position, timestamp, sourceId, hopCount, eventId).
    // ABSOLUTELY NO "confirmed" field is transmitted over ESP-NOW.
    size_t serializeToJSONPayload(const RippleEvent& event, uint8_t* buffer, size_t maxLen);
    String serializeToJSONPayload(const RippleEvent& event);

    // Deserialize ESP-NOW JSON byte payload into RippleEvent struct
    bool deserializeFromJSONPayload(const uint8_t* payload, size_t length, RippleEvent& event);
    bool deserializeFromJSONPayload(const String& payloadStr, RippleEvent& event);

    // Serialize to JSON line string format for Web Serial output on Node D to laptop dashboard
    String serializeToJSONLine(const RippleEvent* lastEvent, RippleStatus status, const char* nodeId, uint8_t distinctSources);
}

#endif // RIPPLE_CORE_H

