#include "heartbeat_task.h"

#include <cstddef>
#include <cstdint>

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace heartbeat {

namespace {

constexpr std::size_t HEARTBEAT_BUFFER_SIZE =
    transport::MAX_FRAME_SIZE;

} // namespace

HeartbeatTask::HeartbeatTask(
    transport::EspNowTransport& transport,
    hlc::Clock& clock,
    node::Id local_node_id,
    std::uint32_t interval_ms
)
    : transport_(transport),
      clock_(clock),
      local_node_id_(local_node_id),
      interval_ms_(interval_ms)
{
}

bool HeartbeatTask::start()
{
    if (started_) {
        return false;
    }

    if (interval_ms_ == 0) {
        return false;
    }

    if (!clock_.ready()) {
        return false;
    }

    const BaseType_t result =
        xTaskCreate(
            task_entry,
            "heartbeat",
            3072,
            this,
            1,
            &task_handle_
        );

    if (result != pdPASS) {
        task_handle_ = nullptr;
        return false;
    }

    started_ = true;

    return true;
}

void HeartbeatTask::task_entry(void* parameter)
{
    auto* task =
        static_cast<HeartbeatTask*>(parameter);

    task->run();

    vTaskDelete(nullptr);
}

void HeartbeatTask::run()
{
    std::uint8_t buffer[
        HEARTBEAT_BUFFER_SIZE
    ]{};

    for (;;) {

        const hlc::Hlc timestamp =
            clock_.now();

        if (timestamp != 0) {

            std::size_t bytes_written = 0;

            const protocol::Result build_result =
                heartbeat::build_packet(
                    local_node_id_,
                    timestamp,
                    buffer,
                    sizeof(buffer),
                    bytes_written
                );

            if (build_result == protocol::Result::OK) {

                const transport::Result send_result =
                    transport_.send_broadcast(
                        buffer,
                        bytes_written
                    );

                if (send_result == transport::Result::OK) {
                    Serial.println(
                        "HEARTBEAT TX: OK"
                    );
                } else {
                    Serial.print(
                        "HEARTBEAT TX: FAIL ("
                    );
                    Serial.print(
                        static_cast<std::uint8_t>(
                            send_result
                        )
                    );
                    Serial.println(")");
                }

            } else {

                Serial.print(
                    "HEARTBEAT BUILD: FAIL ("
                );
                Serial.print(
                    static_cast<std::uint8_t>(
                        build_result
                    )
                );
                Serial.println(")");
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(interval_ms_)
        );
    }
}

bool HeartbeatTask::started() const
{
    return started_;
}

} // namespace heartbeat