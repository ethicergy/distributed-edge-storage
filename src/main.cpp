#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("Distributed Edge Storage");
    Serial.println("PlatformIO build test");
}

void loop() {
    delay(1000);
}
