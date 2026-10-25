#include "transport.h"

#include <cstring>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>

// The ESP-NOW callback signatures below are those of ESP-IDF 4.4
// (Arduino-ESP32 core 2.0.x). IDF 5 changed them; platformio.ini pins
// the platform so an upgrade cannot silently break reception.
#if ESP_IDF_VERSION_MAJOR >= 5
#error "transport.cpp targets ESP-IDF 4.4 / Arduino-ESP32 2.0.x; update the ESP-NOW callbacks before upgrading"
#endif

namespace transport {

namespace {

constexpr std::uint8_t CHANNEL = 6;

constexpr std::uint8_t MAC_A[] = {
    0x14, 0x63, 0x93, 0xC5, 0xD0, 0x18
};

constexpr std::uint8_t MAC_B[] = {
    0x14, 0x63, 0x93, 0xC5, 0xC3, 0x1C
};

constexpr std::uint8_t MAC_C[] = {
    0x14, 0x63, 0x93, 0xC8, 0xA3, 0x08
};

constexpr std::uint8_t MAC_D[] = {
    0x14, 0x63, 0x93, 0xC5, 0xC1, 0x9C
};

constexpr std::uint8_t MAC_BROADCAST[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

bool initialized = false;
node::Id local_node_id = node::INVALID;

// Written by the Wi-Fi task (callbacks), read by the node task.
// Aligned 32-bit accesses are atomic on RV32.
volatile std::uint32_t rx_frames = 0;
volatile std::uint32_t rx_queue_overflows = 0;
volatile std::uint32_t rx_unknown_mac = 0;

std::uint32_t tx_ok[node::MAX - node::MIN + 1] = {};
std::uint32_t tx_failed[node::MAX - node::MIN + 1] = {};

QueueHandle_t rx_queue = nullptr;
QueueHandle_t tx_status_queue = nullptr;

static std::uint8_t rx_queue_storage[
    RX_QUEUE_DEPTH * sizeof(ReceivedFrame)
];

static StaticQueue_t rx_queue_control;

static std::uint8_t tx_status_queue_storage[
    RX_QUEUE_DEPTH * sizeof(SendStatus)
];

static StaticQueue_t tx_status_queue_control;

const std::uint8_t* mac_for_node(node::Id id) {
    switch (id) {
        case node::A:
            return MAC_A;

        case node::B:
            return MAC_B;

        case node::C:
            return MAC_C;

        case node::D:
            return MAC_D;

        default:
            return nullptr;
    }
}

node::Id node_from_mac(const std::uint8_t* mac) {
    if (mac == nullptr) {
        return node::INVALID;
    }

    if (std::memcmp(mac, MAC_A, MAC_ADDRESS_SIZE) == 0) {
        return node::A;
    }

    if (std::memcmp(mac, MAC_B, MAC_ADDRESS_SIZE) == 0) {
        return node::B;
    }

    if (std::memcmp(mac, MAC_C, MAC_ADDRESS_SIZE) == 0) {
        return node::C;
    }

    if (std::memcmp(mac, MAC_D, MAC_ADDRESS_SIZE) == 0) {
        return node::D;
    }

    return node::INVALID;
}

void on_receive(
    const std::uint8_t* mac_addr,
    const std::uint8_t* data,
    int data_len
) {
    if (rx_queue == nullptr ||
        mac_addr == nullptr ||
        data == nullptr ||
        data_len < 0 ||
        data_len > static_cast<int>(MAX_FRAME_SIZE)) {
        return;
    }

    const node::Id source = node_from_mac(mac_addr);

    if (source == node::INVALID) {
        ++rx_unknown_mac;
        return;
    }

    ReceivedFrame frame{};

    frame.source_node = source;
    frame.length = static_cast<std::uint8_t>(data_len);

    std::memcpy(
        frame.data,
        data,
        static_cast<std::size_t>(data_len)
    );

    std::memcpy(
        frame.source_mac,
        mac_addr,
        MAC_ADDRESS_SIZE
    );

    ++rx_frames;

    // Never block the ESP-NOW callback (rule R3).
    if (xQueueSend(rx_queue, &frame, 0) != pdTRUE) {
        ++rx_queue_overflows;
    }
}
void on_send(
    const std::uint8_t* mac_addr,
    esp_now_send_status_t status
) {
    if (tx_status_queue == nullptr ||
        mac_addr == nullptr) {
        return;
    }

    const node::Id destination = node_from_mac(mac_addr);

    if (destination == node::INVALID) {
        return;
    }

    SendStatus event{
        destination,
        status == ESP_NOW_SEND_SUCCESS
    };

    // Never block the callback.
    (void)xQueueSend(
        tx_status_queue,
        &event,
        0
    );
}

} // namespace

const std::uint8_t* configured_mac(node::Id id)
{
    return mac_for_node(id);
}

Result EspNowTransport::begin(
    node::Id local_node
) {
    if (local_node < node::MIN ||
        local_node > node::MAX) {
        return Result::INVALID_NODE;
    }

    local_node_id = local_node;

    rx_queue = xQueueCreateStatic(
        RX_QUEUE_DEPTH,
        sizeof(ReceivedFrame),
        rx_queue_storage,
        &rx_queue_control
    );

    tx_status_queue = xQueueCreateStatic(
        RX_QUEUE_DEPTH,
        sizeof(SendStatus),
        tx_status_queue_storage,
        &tx_status_queue_control
    );

    if (rx_queue == nullptr ||
        tx_status_queue == nullptr) {
        return Result::SEND_FAILED;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    const esp_err_t channel_result =
        esp_wifi_set_channel(
            CHANNEL,
            WIFI_SECOND_CHAN_NONE
        );

    if (channel_result != ESP_OK) {
        return Result::SEND_FAILED;
    }

    if (esp_now_init() != ESP_OK) {
        return Result::SEND_FAILED;
    }

    esp_now_register_recv_cb(on_receive);
    esp_now_register_send_cb(on_send);

        for (node::Id id = node::MIN;
         id <= node::MAX;
         ++id) {

        if (id == local_node_id) {
            continue;
        }

        const std::uint8_t* mac = mac_for_node(id);

        if (mac == nullptr) {
            return Result::INVALID_NODE;
        }

        esp_now_peer_info_t peer_info = {};

        std::memcpy(
            peer_info.peer_addr,
            mac,
            MAC_ADDRESS_SIZE
        );

        peer_info.channel = CHANNEL;
        peer_info.encrypt = false;
        peer_info.ifidx = WIFI_IF_STA;

        const esp_err_t result =
            esp_now_add_peer(&peer_info);

        if (result != ESP_OK) {
            return Result::SEND_FAILED;
        }
    }

    /*
     * Register the broadcast address as an ESP-NOW peer.
     * Broadcast transmission uses this MAC directly.
     */
    esp_now_peer_info_t broadcast_peer = {};

    std::memcpy(
        broadcast_peer.peer_addr,
        MAC_BROADCAST,
        MAC_ADDRESS_SIZE
    );

    broadcast_peer.channel = CHANNEL;
    broadcast_peer.encrypt = false;
    broadcast_peer.ifidx = WIFI_IF_STA;

    const esp_err_t broadcast_result =
        esp_now_add_peer(
            &broadcast_peer
        );

    if (broadcast_result != ESP_OK) {
        return Result::SEND_FAILED;
    }

    initialized = true;

    return Result::OK;
}

Result EspNowTransport::send_unicast(
    node::Id destination,
    const std::uint8_t* data,
    std::size_t length
) {
    if (!initialized) {
        return Result::NOT_INITIALIZED;
    }

    if (destination < node::MIN ||
        destination > node::MAX) {
        return Result::INVALID_NODE;
    }

    if (destination == local_node_id) {
        return Result::INVALID_NODE;
    }

    if (length > MAX_FRAME_SIZE) {
        return Result::FRAME_TOO_LARGE;
    }

    if (length > 0 && data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const std::uint8_t* mac = mac_for_node(destination);

    if (mac == nullptr) {
        return Result::PEER_NOT_FOUND;
    }

    const esp_err_t result =
        esp_now_send(
            mac,
            data,
            length
        );

    if (result != ESP_OK) {
        if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
            return Result::PEER_NOT_FOUND;
        }

        return Result::SEND_FAILED;
    }

    return Result::OK;
}


Result EspNowTransport::send_broadcast(
    const std::uint8_t* data,
    std::size_t length
) {
    if (!initialized) {
        return Result::NOT_INITIALIZED;
    }

    if (length > MAX_FRAME_SIZE) {
        return Result::FRAME_TOO_LARGE;
    }

    if (length > 0 && data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const esp_err_t result =
        esp_now_send(
            MAC_BROADCAST,
            data,
            length
        );

    if (result != ESP_OK) {
        return Result::SEND_FAILED;
    }

    return Result::OK;
}


bool EspNowTransport::receive(
    ReceivedFrame& frame
) {
    if (!initialized || rx_queue == nullptr) {
        return false;
    }

    return xQueueReceive(
        rx_queue,
        &frame,
        0
    ) == pdTRUE;
}
bool EspNowTransport::receive_send_status(
    SendStatus& status
) {
    if (!initialized || tx_status_queue == nullptr) {
        return false;
    }

    return xQueueReceive(
        tx_status_queue,
        &status,
        0
    ) == pdTRUE;
}


void EspNowTransport::drain_send_status()
{
    SendStatus status{};

    while (receive_send_status(status)) {
        if (status.destination < node::MIN || status.destination > node::MAX) {
            continue;
        }

        if (status.success) {
            ++tx_ok[status.destination - node::MIN];
        } else {
            ++tx_failed[status.destination - node::MIN];
        }
    }
}

Stats EspNowTransport::stats() const
{
    Stats stats{};

    stats.rx_frames = rx_frames;
    stats.rx_queue_overflows = rx_queue_overflows;
    stats.rx_unknown_mac = rx_unknown_mac;

    for (std::size_t i = 0; i < node::MAX - node::MIN + 1; ++i) {
        stats.tx_status_ok[i] = tx_ok[i];
        stats.tx_status_failed[i] = tx_failed[i];
    }

    return stats;
}

} // namespace transport
