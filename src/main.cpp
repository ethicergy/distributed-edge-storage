#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#ifndef NODE_ID
#error "NODE_ID must be defined"
#endif

static constexpr uint8_t NODE_A = 0;
static constexpr uint8_t NODE_B = 1;

static const uint8_t MAC_A[] = {
    0x14, 0x63, 0x93, 0xC5, 0xD0, 0x18
};

static const uint8_t MAC_B[] = {
    0x14, 0x63, 0x93, 0xC5, 0xC3, 0x1C
};

static constexpr uint8_t CHANNEL = 6;

volatile bool hello_received = false;

void on_receive(
    const uint8_t *mac_addr,
    const uint8_t *data,
    int data_len
) {
    if (data_len == 12 &&
        memcmp(data, "HELLO FROM A", 12) == 0) {

        hello_received = true;
    }

    if (data_len == 3 &&
        memcmp(data, "ACK", 3) == 0) {

        Serial.println("RECEIVED: ACK");
    }
}

void on_send(
    const uint8_t *mac_addr,
    esp_now_send_status_t status
) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.println("ESP-NOW TX: SUCCESS");
    } else {
        Serial.println("ESP-NOW TX: FAIL");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    esp_err_t channel_result =
        esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);

    Serial.println();
    Serial.println("================================");
    Serial.println("ESP-NOW TRANSPORT TEST");
    Serial.printf("NODE_ID: %d\n", NODE_ID);
    Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
    Serial.println("CHANNEL: 6");
    Serial.println("TX POWER: 8.5 dBm");
    Serial.println("================================");

    if (channel_result != ESP_OK) {
        Serial.printf(
            "ERROR: Channel setup failed: %s\n",
            esp_err_to_name(channel_result)
        );
        return;
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("ERROR: ESP-NOW init failed");
        return;
    }

    esp_now_register_recv_cb(on_receive);
    esp_now_register_send_cb(on_send);

    esp_now_peer_info_t peer_info = {};
    peer_info.channel = CHANNEL;
    peer_info.encrypt = false;
    peer_info.ifidx = WIFI_IF_STA;

#if NODE_ID == NODE_A
    memcpy(peer_info.peer_addr, MAC_B, 6);
#else
    memcpy(peer_info.peer_addr, MAC_A, 6);
#endif

    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        Serial.println("ERROR: Failed to add peer");
        return;
    }

    Serial.println("ESP-NOW READY");
}

void loop() {

#if NODE_ID == NODE_A

    static unsigned long last_send = 0;

    if (millis() - last_send >= 2000) {
        last_send = millis();

        const uint8_t message[] = "HELLO FROM A";

        esp_err_t result = esp_now_send(
            MAC_B,
            message,
            sizeof(message) - 1
        );

        if (result != ESP_OK) {
            Serial.printf(
                "SEND ERROR: %s\n",
                esp_err_to_name(result)
            );
        }
    }

#else

    if (hello_received) {
        hello_received = false;

        const uint8_t ack[] = "ACK";

        esp_err_t result = esp_now_send(
            MAC_A,
            ack,
            sizeof(ack) - 1
        );

        if (result != ESP_OK) {
            Serial.printf(
                "ACK SEND ERROR: %s\n",
                esp_err_to_name(result)
            );
        }
    }

#endif

    delay(10);
}