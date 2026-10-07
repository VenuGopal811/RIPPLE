#include <Arduino.h>
#include "RippleCore.h"

#ifndef NODE_ID
#error "NODE_ID must be set via platformio.ini build_flags, see environments"
#endif

// ---------------------------------------------------------------------------
// Hardware Pin Map (matching DESIGN.md)
// ---------------------------------------------------------------------------
#define PIN_BUTTON         4    // Manual button (active LOW, INPUT_PULLUP)
#define PIN_TRIG           5    // Ultrasonic HC-SR04 TRIG pin
#define PIN_ECHO           18   // Ultrasonic HC-SR04 ECHO pin (via 5V->3.3V divider)

#define PIN_CAUTION_LED   22   // Node D Caution LED (UNVERIFIED / 1 distinct source)
#define PIN_CONFIRMED_LED 23   // Node D Confirmed LED (CONFIRMED / >=2 distinct sources)
#define PIN_BUZZER_1      18   // Node D Buzzer 1
#define PIN_BUZZER_2      19   // Node D Buzzer 2

#ifdef ROLE_SENSOR
static unsigned long lastUltrasoundCheckMs = 0;
static unsigned long lastTriggerSentMs = 0;
static bool lastButtonState = HIGH;
static unsigned long lastButtonDebounceMs = 0;
static bool obstructionActive = false;

// Reads HC-SR04 distance in cm (returns -1.0 on timeout)
float readUltrasonicDistanceCm() {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    // 25,000 µs pulseIn timeout (~4.2 meters max) to avoid blocking main loop
    long durationUs = pulseIn(PIN_ECHO, HIGH, 25000UL);
    if (durationUs == 0) return -1.0f;

    return (durationUs * 0.0343f) / 2.0f;
}
#endif

#ifdef ROLE_GATEWAY
#include <ArduinoJson.h>
static void publishStateAsJson(const RippleEvent* lastTrigger) {
    // Web Serial output for Node D laptop bridge
    RippleStatus status = rippleGetStatus();
    uint8_t count = rippleGetDistinctSourceCount();
    const RippleEvent* ev = lastTrigger ? lastTrigger : rippleGetLastEvent();
    String line = RippleSerialization::serializeToJSONLine(ev, status, NODE_ID, count);
    Serial.print(line);
}
#endif

void setup() {
    Serial.begin(115200);
    delay(200);

#if defined(ROLE_SENSOR)
    pinMode(PIN_TRIG, OUTPUT);
    digitalWrite(PIN_TRIG, LOW);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    rippleBegin(NODE_ID, /* hasSensor = */ true);
#else
    rippleBegin(NODE_ID, /* hasSensor = */ false);
#endif

#if defined(ROLE_GATEWAY)
    pinMode(PIN_CAUTION_LED, OUTPUT);
    pinMode(PIN_CONFIRMED_LED, OUTPUT);
    pinMode(PIN_BUZZER_1, OUTPUT);
    pinMode(PIN_BUZZER_2, OUTPUT);
    digitalWrite(PIN_CAUTION_LED, LOW);
    digitalWrite(PIN_CONFIRMED_LED, LOW);
    digitalWrite(PIN_BUZZER_1, LOW);
    digitalWrite(PIN_BUZZER_2, LOW);
#endif

    Serial.print("[RIPPLE] Node ");
    Serial.print(NODE_ID);
    Serial.println(" initialized & ready.");
}

void loop() {
    rippleLoop();

#if defined(ROLE_SENSOR)
    unsigned long now = millis();

    // 1. Non-blocking HC-SR04 Ultrasonic Distance Sampling (every 60 ms)
    if (now - lastUltrasoundCheckMs >= 60) {
        lastUltrasoundCheckMs = now;
        float distCm = readUltrasonicDistanceCm();

        if (distCm > 0.0f && distCm <= 15.0f) {
            if (!obstructionActive && (now - lastTriggerSentMs >= 1000)) {
                obstructionActive = true;
                lastTriggerSentMs = now;
                rippleFireLocalWitness();
                Serial.print("[ULTRASONIC HAZARD DETECTED] distance=");
                Serial.print(distCm);
                Serial.print(" cm, source=");
                Serial.println(NODE_ID);
            }
        } else if (distCm > 18.0f) {
            obstructionActive = false; // Reset threshold hysteresis
        }
    }

    // 2. Manual Button Override (active LOW with debounce)
    bool buttonNow = digitalRead(PIN_BUTTON);
    if (buttonNow == LOW && lastButtonState == HIGH && (now - lastButtonDebounceMs >= 250)) {
        lastButtonDebounceMs = now;
        lastTriggerSentMs = now;
        rippleFireLocalWitness();
        Serial.print("[BUTTON HAZARD TRIGGERED] source=");
        Serial.println(NODE_ID);
    }
    lastButtonState = buttonNow;
#endif

    // Handle incoming events and local corroboration updates
    if (rippleHasNewEvent()) {
        RippleStatus status = rippleGetStatus();
        uint8_t count = rippleGetDistinctSourceCount();

        Serial.print("[STATE UPDATE] Node ");
        Serial.print(NODE_ID);
        Serial.print(" -> Status: ");
        Serial.print(status == RippleStatus::CONFIRMED ? "CONFIRMED" :
                     status == RippleStatus::UNVERIFIED ? "UNVERIFIED" : "NORMAL");
        Serial.print(" (Distinct Sources: ");
        Serial.print(count);
        Serial.print(") | ");
        Serial.println(rippleGetReason());

#if defined(ROLE_SENSOR)
        // Node B specific reaction logging
        if (strcmp(NODE_ID, "B") == 0) {
            if (status == RippleStatus::UNVERIFIED) {
                Serial.println("[NODE B REACTION] CAUTION - Received Node A alert alone.");
            } else if (status == RippleStatus::CONFIRMED) {
                Serial.println("[NODE B REACTION] CONFIRMED - Local witness + Node A corroborated!");
            }
        }
#endif

#if defined(ROLE_RELAY) && !defined(ROLE_GATEWAY)
        // Node C reaction (relay node)
        if (status == RippleStatus::CONFIRMED) {
            Serial.println("[ACTION Node C] CONFIRMED hazard -> Minor course adjustment.");
        } else if (status == RippleStatus::UNVERIFIED) {
            Serial.println("[ACTION Node C] UNVERIFIED hazard -> Relaying only.");
        }
#endif

#if defined(ROLE_GATEWAY)
        // Node D reaction: drives Caution LED (GPIO 22), Confirmed LED (GPIO 23), Buzzers (GPIO 18/19)
        if (status == RippleStatus::CONFIRMED) {
            digitalWrite(PIN_CAUTION_LED, LOW);
            digitalWrite(PIN_CONFIRMED_LED, HIGH);
            digitalWrite(PIN_BUZZER_1, HIGH);
            digitalWrite(PIN_BUZZER_2, HIGH);
            Serial.println("[ACTION Node D] CONFIRMED hazard -> Full reroute warning! Confirmed LED & dual buzzers ACTIVE.");
        } else if (status == RippleStatus::UNVERIFIED) {
            digitalWrite(PIN_CAUTION_LED, HIGH);
            digitalWrite(PIN_CONFIRMED_LED, LOW);
            digitalWrite(PIN_BUZZER_1, HIGH);
            digitalWrite(PIN_BUZZER_2, LOW);
            Serial.println("[ACTION Node D] UNVERIFIED hazard -> Caution LED & Buzzer 1 ACTIVE.");
        } else {
            digitalWrite(PIN_CAUTION_LED, LOW);
            digitalWrite(PIN_CONFIRMED_LED, LOW);
            digitalWrite(PIN_BUZZER_1, LOW);
            digitalWrite(PIN_BUZZER_2, LOW);
            Serial.println("[ACTION Node D] NORMAL -> All actuators OFF.");
        }
        publishStateAsJson(nullptr);
#endif
    }
}
