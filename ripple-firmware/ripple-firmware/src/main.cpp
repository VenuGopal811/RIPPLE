#include <Arduino.h>
#include "RippleCore.h"

#ifndef NODE_ID
#error "NODE_ID must be set via platformio.ini build_flags, see environments"
#endif

// ---------------------------------------------------------------------------
// Pin map (see DESIGN.md "Hardware wiring summary")
// ---------------------------------------------------------------------------
#define PIN_SENSOR_OUT 4
#define PIN_BUTTON     5
#define PIN_BUZZER     18
#define PIN_LED        19

#ifdef ROLE_SENSOR
static bool lastSensorState = HIGH;
#endif

#ifdef ROLE_GATEWAY
#include <ArduinoJson.h>
static void publishStateAsJson(const RippleEvent* lastTrigger) {
  // One JSON line per state change, this is what the browser dashboard and
  // the 2D visualization's receiveLiveEvent() consume over Web Serial.
  JsonDocument doc;
  doc["type"] = "obstruction";
  doc["position"] = NODE_ID;
  doc["timestamp"] = (uint32_t)millis();
  doc["sourceId"] = lastTrigger ? lastTrigger->sourceId : "";
  doc["status"] = rippleGetStatus() == RIPPLE_CONFIRMED ? "CONFIRMED"
                 : rippleGetStatus() == RIPPLE_UNVERIFIED ? "UNVERIFIED" : "NORMAL";
  doc["reason"] = rippleGetReason();
  serializeJson(doc, Serial);
  Serial.println();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);

#if defined(ROLE_SENSOR)
  pinMode(PIN_SENSOR_OUT, INPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  rippleBegin(NODE_ID, /* hasSensor = */ true);
#else
  rippleBegin(NODE_ID, /* hasSensor = */ false);
#endif

#if defined(ROLE_GATEWAY)
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);
#endif

  Serial.print("[RIPPLE] Node ");
  Serial.print(NODE_ID);
  Serial.println(" ready.");
}

void loop() {
  rippleLoop();

#if defined(ROLE_SENSOR)
  // Poll sensor and backup button. Either one firing counts as this node's
  // own witness report, exactly one report per press/trigger.
  bool sensorNow = digitalRead(PIN_SENSOR_OUT);
  bool buttonNow = digitalRead(PIN_BUTTON);
  if ((sensorNow == LOW && lastSensorState == HIGH) || buttonNow == LOW) {
    rippleFireLocalWitness();
    Serial.print("[ALERT SENT] source=");
    Serial.println(NODE_ID);
    delay(300); // simple debounce
  }
  lastSensorState = sensorNow;
#endif

  if (rippleHasNewEvent()) {
    RippleStatus status = rippleGetStatus();
    Serial.print("[STATE] ");
    Serial.print(NODE_ID);
    Serial.print(" -> ");
    Serial.println(rippleGetReason());

#if defined(ROLE_RELAY) && !defined(ROLE_SENSOR)
    // C and D: react based on distinct-source count only, per PRD node table.
    if (status == RIPPLE_CONFIRMED) {
      #if defined(ROLE_GATEWAY)
        digitalWrite(PIN_BUZZER, HIGH);
        digitalWrite(PIN_LED, HIGH);
        Serial.println("[ACTION] Full reroute, buzzer + LED active");
      #else
        Serial.println("[ACTION] Minor course adjustment");
      #endif
    } else {
      Serial.println("[ACTION] Relaying");
      #if defined(ROLE_GATEWAY)
        digitalWrite(PIN_BUZZER, LOW);
        digitalWrite(PIN_LED, LOW);
      #endif
    }
#endif

#if defined(ROLE_GATEWAY)
    publishStateAsJson(nullptr);
#endif
  }
}
