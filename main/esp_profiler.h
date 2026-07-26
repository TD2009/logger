#ifndef ESP_PROFILER_H
#define ESP_PROFILER_H

#include <stdint.h>

/**
 * @brief Get current hardware timestamp in microseconds.
 */
uint64_t esp_profiler_get_time_us(void);

/**
 * @brief Perform active spin-wait loop simulating task execution.
 */
void esp_profiler_busy_wait_us(uint32_t us);

#endif // ESP_PROFILER_H
