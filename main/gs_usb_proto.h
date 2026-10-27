// gs_usb wire protocol, as implemented by the Linux driver
// drivers/net/can/usb/gs_usb.c and candleLight_fw. All multi-byte fields are
// little endian (candleLight ignores HOST_FORMAT; so do we).

#pragma once

#include <stdint.h>

// Control requests (vendor, recipient = interface, wValue = channel)
enum gs_usb_breq {
    GS_USB_BREQ_HOST_FORMAT = 0,
    GS_USB_BREQ_BITTIMING,
    GS_USB_BREQ_MODE,
    GS_USB_BREQ_BERR,
    GS_USB_BREQ_BT_CONST,
    GS_USB_BREQ_DEVICE_CONFIG,
    GS_USB_BREQ_TIMESTAMP,
    GS_USB_BREQ_IDENTIFY,
    GS_USB_BREQ_GET_USER_ID,
    GS_USB_BREQ_SET_USER_ID,
    GS_USB_BREQ_DATA_BITTIMING,
    GS_USB_BREQ_BT_CONST_EXT,
    GS_USB_BREQ_SET_TERMINATION,
    GS_USB_BREQ_GET_TERMINATION,
    GS_USB_BREQ_GET_STATE,
};

enum gs_can_mode {
    GS_CAN_MODE_RESET = 0,
    GS_CAN_MODE_START = 1,
};

enum gs_can_state {
    GS_CAN_STATE_ERROR_ACTIVE = 0,
    GS_CAN_STATE_ERROR_WARNING,
    GS_CAN_STATE_ERROR_PASSIVE,
    GS_CAN_STATE_BUS_OFF,
    GS_CAN_STATE_STOPPED,
    GS_CAN_STATE_SLEEPING,
};

// gs_device_mode.flags
#define GS_CAN_MODE_LISTEN_ONLY     (1u << 0)
#define GS_CAN_MODE_LOOP_BACK       (1u << 1)
#define GS_CAN_MODE_TRIPLE_SAMPLE   (1u << 2)
#define GS_CAN_MODE_ONE_SHOT        (1u << 3)
#define GS_CAN_MODE_HW_TIMESTAMP    (1u << 4)
#define GS_CAN_MODE_BERR_REPORTING  (1u << 12)

// gs_device_bt_const.feature
#define GS_CAN_FEATURE_LISTEN_ONLY      (1u << 0)
#define GS_CAN_FEATURE_LOOP_BACK        (1u << 1)
#define GS_CAN_FEATURE_TRIPLE_SAMPLE    (1u << 2)
#define GS_CAN_FEATURE_ONE_SHOT         (1u << 3)
#define GS_CAN_FEATURE_HW_TIMESTAMP     (1u << 4)
#define GS_CAN_FEATURE_IDENTIFY         (1u << 5)
#define GS_CAN_FEATURE_BERR_REPORTING   (1u << 12)
#define GS_CAN_FEATURE_GET_STATE        (1u << 13)

// gs_host_frame.flags
#define GS_CAN_FLAG_OVERFLOW    (1u << 0)

#define GS_HOST_FRAME_ECHO_ID_RX    0xFFFFFFFFu

// The Linux driver keeps at most this many TX frames in flight per channel.
#define GS_MAX_TX_URBS  10

typedef struct __attribute__((packed)) {
    uint32_t byte_order;
} gs_host_config_t;

typedef struct __attribute__((packed)) {
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;
    uint8_t  icount;        // number of channels - 1
    uint32_t sw_version;
    uint32_t hw_version;
} gs_device_config_t;

typedef struct __attribute__((packed)) {
    uint32_t mode;
    uint32_t flags;
} gs_device_mode_t;

typedef struct __attribute__((packed)) {
    uint32_t state;
    uint32_t rxerr;
    uint32_t txerr;
} gs_device_state_t;

typedef struct __attribute__((packed)) {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
} gs_device_bittiming_t;

typedef struct __attribute__((packed)) {
    uint32_t mode;
} gs_identify_mode_t;

typedef struct __attribute__((packed)) {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
} gs_device_bt_const_t;

// Classic CAN frame without hardware timestamp: 12-byte header + 8 data bytes.
typedef struct __attribute__((packed)) {
    uint32_t echo_id;
    uint32_t can_id;    // SocketCAN layout: CAN_EFF_FLAG / CAN_RTR_FLAG / CAN_ERR_FLAG in top bits
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
} gs_host_frame_t;

_Static_assert(sizeof(gs_host_frame_t) == 20, "gs_host_frame_t must be 20 bytes");

// SocketCAN id flags (include/uapi/linux/can.h)
#define CAN_EFF_FLAG    0x80000000u
#define CAN_RTR_FLAG    0x40000000u
#define CAN_ERR_FLAG    0x20000000u
#define CAN_SFF_MASK    0x000007FFu
#define CAN_EFF_MASK    0x1FFFFFFFu

// SocketCAN error frames (include/uapi/linux/can/error.h)
#define CAN_ERR_DLC             8
#define CAN_ERR_LOSTARB         0x00000002u
#define CAN_ERR_CRTL            0x00000004u
#define CAN_ERR_PROT            0x00000008u
#define CAN_ERR_ACK             0x00000020u
#define CAN_ERR_BUSOFF          0x00000040u
#define CAN_ERR_BUSERROR        0x00000080u
#define CAN_ERR_RESTARTED       0x00000100u
#define CAN_ERR_CNT             0x00000200u

#define CAN_ERR_CRTL_RX_OVERFLOW    0x01
#define CAN_ERR_CRTL_RX_WARNING     0x04
#define CAN_ERR_CRTL_TX_WARNING     0x08
#define CAN_ERR_CRTL_RX_PASSIVE     0x10
#define CAN_ERR_CRTL_TX_PASSIVE     0x20
#define CAN_ERR_CRTL_ACTIVE         0x40

#define CAN_ERR_PROT_BIT        0x01
#define CAN_ERR_PROT_FORM       0x02
#define CAN_ERR_PROT_STUFF      0x04
