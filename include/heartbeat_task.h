#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "heartbeat.h"
#include "hlc.h"
#include "node_id.h"
#include "transport.h"

namespace heartbeat {

class HeartbeatTask {
public:
    HeartbeatTask(
        transport::EspNowTransport& transport,
        hlc::Clock& clock,
        node::Id local_node_id,
        std::uint32_t interval_ms
    );

    bool start();

    bool started() const;

private:
    static void task_entry(void* parameter);

    void run();

    transport::EspNowTransport& transport_;
    hlc::Clock& clock_;
    node::Id local_node_id_;
    std::uint32_t interval_ms_;

    TaskHandle_t task_handle_ = nullptr;
    bool started_ = false;
};

} // namespace heartbeat