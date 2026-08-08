/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_gmf_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HLS_IO_TASK_STACK          (8 * 1024)
#define HLS_IO_TASK_CORE           (0)
#define HLS_IO_TASK_PRIO           (8)
#define HLS_IO_RINGBUFFER_SIZE     (128 * 1024)
#define HLS_IO_BUFFER_SIZE         (8 * 1024)

/**
 * @brief  HLS (HTTP Live Streaming) configuration
 */
typedef struct {
    int                 dir;        /*!< Type of stream (reader only) */
    esp_gmf_io_cfg_t    io_cfg;     /*!< IO configuration for task and buffer */
    esp_err_t (*crt_bundle_attach)(void *conf);  /*!< TLS certificate bundle attach */
} hls_io_cfg_t;

#define HLS_IO_CFG_DEFAULT()       {                     \
    .dir          = ESP_GMF_IO_DIR_READER,               \
    .io_cfg       = {                                    \
        .thread = {                                      \
            .stack        = HLS_IO_TASK_STACK,           \
            .prio         = HLS_IO_TASK_PRIO,            \
            .core         = HLS_IO_TASK_CORE,            \
            .stack_in_ext = true,                        \
        },                                               \
        .buffer_cfg = {                                  \
            .io_size     = HLS_IO_BUFFER_SIZE,           \
            .buffer_size = HLS_IO_RINGBUFFER_SIZE,       \
        },                                               \
        .enable_speed_monitor = false,                   \
    },                                                   \
    .crt_bundle_attach = NULL,                           \
}

/**
 * @brief  Initialize the HLS IO with the specified configuration
 */
esp_gmf_err_t esp_gmf_io_hls_init(hls_io_cfg_t *config, esp_gmf_io_handle_t *io);

#ifdef __cplusplus
}
#endif
