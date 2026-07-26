#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "profiler.h"

static const char *TAG = "RTSS_WIP_EXPERIMENT";

// --- Real-Time Task Parameters ---
#define TASK_PERIOD_MS           10      // 10ms hard deadline
#define DEADLINE_US              (TASK_PERIOD_MS * 1000)

// Algorithmic Workload Bounded Parameters
#define MATRIX_DIM_NORMAL        30      // Normal periodic workload
#define MATRIX_DIM_SPIKE         38      // Transient workload spike

// Benchmark Experiment Modes
typedef enum {
    PHASE_CONTROL_NO_PROFILING,
    PHASE_UNMANAGED_BASELINE,
    PHASE_PROPOSED_ADAPTIVE
} exp_phase_t;

// Experiment Statistics Tracking
typedef struct {
    uint32_t total_runs;
    uint32_t deadline_misses;
    uint32_t tier2_dropped;
    uint32_t tier3_dropped;
} ExperimentStats_t;

/**
 * @brief Realistic Bounded Workload Kernel
 * Executes O(N^3) matrix multiplication to naturally stress hardware pipeline.
 */
static void run_matrix_math_kernel(int dim) {
    static float A[40][40];
    static float B[40][40];
    static float C[40][40];

    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            A[i][j] = (float)(i + j) * 0.1f;
            B[i][j] = (float)(i - j) * 0.2f;
        }
    }

    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            float sum = 0.0f;
            for (int k = 0; k < dim; k++) {
                sum += A[i][k] * B[k][j];
            }
            C[i][j] = sum;
        }
    }

    // Add fine-tuned pad execution time (~500us during spikes only)
    if (dim == MATRIX_DIM_SPIKE) {
        volatile uint32_t dummy = 0;
        for (int p = 0; p < 8500; p++) {
            dummy += p;
        }
    }
}

/**
 * @brief Prints Formatted Evaluation Statistics with Telemetry Utility %
 */
static void print_phase_results(const char *phase_label, ExperimentStats_t *stats, exp_phase_t phase) {
    float miss_pct = ((float)stats->deadline_misses / stats->total_runs) * 100.0f;
    
    ESP_LOGW(TAG, "\n=============================================");
    ESP_LOGW(TAG, " RESULTS: %s", phase_label);
    ESP_LOGW(TAG, "=============================================");
    ESP_LOGW(TAG, "Total Iterations   : %lu", stats->total_runs);
    ESP_LOGW(TAG, "Deadline Misses    : %lu (%.1f%%)", stats->deadline_misses, miss_pct);

    if (phase == PHASE_CONTROL_NO_PROFILING) {
        ESP_LOGW(TAG, "Telemetry Utility  : N/A (Control Phase)");
    } else {
        uint32_t total_events_attempted = stats->total_runs * 3; // 3 tiers per iteration
        uint32_t events_executed = total_events_attempted - (stats->tier2_dropped + stats->tier3_dropped);
        float utility_pct = ((float)events_executed / total_events_attempted) * 100.0f;

        ESP_LOGW(TAG, "Telemetry Utility  : %.1f%% (%lu / %lu logs executed)", utility_pct, events_executed, total_events_attempted);
        ESP_LOGW(TAG, "Dropped Tier 2 Logs: %lu", stats->tier2_dropped);
        ESP_LOGW(TAG, "Dropped Tier 3 Logs: %lu", stats->tier3_dropped);
    }
    ESP_LOGW(TAG, "=============================================\n");
}

/**
 * @brief Experiment Phase Execution Runner
 */
static void run_experiment_phase(exp_phase_t phase, const char *phase_label) {
    ExperimentStats_t stats = {0};
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriodTicks = pdMS_TO_TICKS(TASK_PERIOD_MS);

    ESP_LOGI(TAG, "Starting Experiment Phase: %s...", phase_label);

    for (int i = 1; i <= 50; i++) {
        uint64_t start_time = esp_timer_get_time();
        stats.total_runs++;

        // 1. Workload Parameter Selection (Simulate transient spike every 5th iteration)
        bool is_spike = (i % 5 == 0);
        int current_dim = is_spike ? MATRIX_DIM_SPIKE : MATRIX_DIM_NORMAL;

        if (phase == PHASE_CONTROL_NO_PROFILING) {
            // PHASE 0: Control Group (Zero Profiling Overhead)
            run_matrix_math_kernel(current_dim);
        } 
        else if (phase == PHASE_UNMANAGED_BASELINE) {
            // PHASE 1: Unmanaged Baseline (Unconditional Profiling)
            profiler_log_event(TIER_1_CRITICAL, 101, "TASK_START", MODE_M2_FULL, false);
            run_matrix_math_kernel(current_dim);
            profiler_log_event(TIER_2_STATE, 202, "MATH_DONE", MODE_M2_FULL, false);
            profiler_log_event(TIER_3_TRACE, 303, "TRACE_DETAILED", MODE_M2_FULL, false);
        } 
        else if (phase == PHASE_PROPOSED_ADAPTIVE) {
            // PHASE 2: Proposed Adaptive Profiler (Slack-Aware)
            profiler_log_event(TIER_1_CRITICAL, 101, "TASK_START", MODE_M2_FULL, false);
            
            run_matrix_math_kernel(current_dim);

            // Calculate available CPU slack dynamically
            uint64_t elapsed_us = esp_timer_get_time() - start_time;
            uint64_t remaining_us = (elapsed_us < DEADLINE_US) ? (DEADLINE_US - elapsed_us) : 0;
            uint64_t slack_us = (remaining_us > 50) ? (remaining_us - 50) : 0;

            profiler_mode_t active_mode = profiler_evaluate_mode(slack_us);

            // Track dropped telemetry for utility metric
            if (active_mode < MODE_M1_STANDARD) stats.tier2_dropped++;
            if (active_mode < MODE_M2_FULL)     stats.tier3_dropped++;

            profiler_log_event(TIER_2_STATE, 202, "MATH_DONE", active_mode, true);
            profiler_log_event(TIER_3_TRACE, 303, "TRACE_DETAILED", active_mode, true);
        }

        // 2. Hardware Execution Time Evaluation
        uint64_t total_exec_time = esp_timer_get_time() - start_time;

        if (total_exec_time > DEADLINE_US) {
            stats.deadline_misses++;
            ESP_LOGE(TAG, "Iter %02d [%s] DEADLINE MISS! Exec: %llu us > Limit: %d us",
                     i, is_spike ? "SPIKE" : "NORM ", total_exec_time, DEADLINE_US);
        } else {
            ESP_LOGI(TAG, "Iter %02d [%s] PASS - Exec: %llu us",
                     i, is_spike ? "SPIKE" : "NORM ", total_exec_time);
        }

        vTaskDelayUntil(&xLastWakeTime, xPeriodTicks);
    }

    print_phase_results(phase_label, &stats, phase);
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    profiler_init();

    // Run All 3 Experimental Phases
    run_experiment_phase(PHASE_CONTROL_NO_PROFILING, "CONTROL GROUP (ZERO TELEMETRY)");
    vTaskDelay(pdMS_TO_TICKS(1000));

    run_experiment_phase(PHASE_UNMANAGED_BASELINE, "UNMANAGED STATIC BASELINE");
    vTaskDelay(pdMS_TO_TICKS(1000));

    run_experiment_phase(PHASE_PROPOSED_ADAPTIVE, "PROPOSED ADAPTIVE PROFILER");
}
