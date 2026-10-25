#include "i2c_bus.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace i2c_bus {

namespace {

StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex = nullptr;

SemaphoreHandle_t bus_mutex()
{
    // First use is from setup(), before any other task exists.
    if (mutex == nullptr) {
        mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    }

    return mutex;
}

} // namespace

Lock::Lock()
{
    xSemaphoreTake(bus_mutex(), portMAX_DELAY);
}

Lock::~Lock()
{
    xSemaphoreGive(bus_mutex());
}

} // namespace i2c_bus
