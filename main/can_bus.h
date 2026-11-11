// CAN side of the gateway: owns the TWAI controller.
//
// Frames to the host are produced in gs_host_frame_t layout (SocketCAN id
// flags), which keeps this module independent of the USB transport.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gs_usb_proto.h"

// Bit timing in controller time quanta. tseg1 includes the propagation segment.
typedef struct {
    uint32_t brp;
    uint32_t tseg1;
    uint32_t tseg2;
    uint32_t sjw;
} can_timing_t;

typedef struct {
    bool listen_only;
    bool loopback;
    bool one_shot;
    bool triple_sample;
    bool berr_reporting;
} can_mode_t;

typedef struct {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t rx_overflows;
    uint32_t bus_errors;
    uint32_t bus_off_count;
} can_stats_t;

// Controller limits reported to the host in BT_CONST (from the ESP-IDF
// esp32s3 twai_ll.h: BRP 2..16384 even, TSEG1 1..16, TSEG2 1..8, SJW 1..4;
// TWAI clock is APB = 80 MHz).
#define CAN_FCLK_HZ     80000000u
#define CAN_TSEG1_MIN   1u
#define CAN_TSEG1_MAX   16u
#define CAN_TSEG2_MIN   1u
#define CAN_TSEG2_MAX   8u
#define CAN_SJW_MAX     4u
#define CAN_BRP_MIN     2u
#define CAN_BRP_MAX     16384u
#define CAN_BRP_INC     2u

esp_err_t can_bus_init(void);

// Received frames, error frames. Lossy: overflow is flagged on the next frame.
QueueHandle_t can_bus_rx_queue(void);

// TX echoes. Sized for every in-flight frame, so it never drops: a lost echo
// would stall the host's TX queue.
QueueHandle_t can_bus_echo_queue(void);

bool can_bus_timing_valid(const can_timing_t *timing);

esp_err_t can_bus_start(const can_timing_t *timing, const can_mode_t *mode);
void can_bus_stop(void);
bool can_bus_running(void);
// Bit rate (bit/s) and listen-only flag of the running channel; 0 / false when stopped.
uint32_t can_bus_bitrate(void);
bool can_bus_listen_only(void);

// Queue a host frame for transmission. The frame's echo is delivered on the
// echo queue once the controller finishes with it (sent or failed).
void can_bus_send(const gs_host_frame_t *frame);

void can_bus_get_state(gs_device_state_t *out);
void can_bus_get_stats(can_stats_t *out);
