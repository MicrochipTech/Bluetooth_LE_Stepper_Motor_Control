/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include "trs_server.h"
#include "app_stepper_motor.h"

LOG_MODULE_REGISTER(main);

#define UART_TX_BUF_SIZE 244
#define RING_BUF_SIZE    512
#define LED_BLINK_INTERVAL_MS 500
#define LED_RX_PULSE_MS 250
#define MODE_BLINK_INTERVAL_MS 120
#define USER_LED_NODE DT_ALIAS(led3)
#define USER_BUTTON_NODE DT_ALIAS(sw0)

/* MBD legacy advertisement name and service data */
#define ADV_NAME "Zephyr_Stepper_Motor"
#define ADV_NAME_LEN (sizeof(ADV_NAME) - 1)

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)

/* incoming events and data from the bluetooth */
static K_FIFO_DEFINE(rx_queue);

#define APP_BUF_POOL_SIZE CONFIG_TRS_SERVER_BUF_POOL_SIZE
#define APP_BUF_DATA_SIZE CONFIG_TRS_SERVER_BUF_DATA_SIZE

NET_BUF_POOL_DEFINE(app_buff_pool, APP_BUF_POOL_SIZE, APP_BUF_DATA_SIZE, 0, NULL);
RING_BUF_DECLARE(rx_ring, RING_BUF_SIZE);

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(USER_LED_NODE, gpios);
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(USER_BUTTON_NODE, gpios);
static struct gpio_callback user_button_cb;

static struct bt_conn *ble_conn;
static struct k_work uart_rx_work;
static struct k_work ble_rx_work;
static struct k_work_delayable adv_retry_work;
static struct k_work_delayable led_blink_work;
static struct k_work_delayable led_rx_pulse_work;
static struct k_work mode_button_work;
static struct k_work_delayable mode_blink_work;
static uint8_t mode_blink_count;
static bool advertising_started;
static bool led_state;  /* true = ON, false = OFF */

static struct k_work vendor_work;
static uint8_t vendor_buf[32];
static int vendor_len_global;

/*
 * ── UART accumulation buffer ──
 * Collects bytes from ISR until a delimiter (CR/LF) is seen,
 * then hands off the complete line via a work item.
 */
#define ACCUM_BUF_SIZE 128
static uint8_t accum_buf[ACCUM_BUF_SIZE];
static size_t  accum_pos;
static struct k_work process_line_work;

/* Double-buffer: ISR fills accum_buf, work handler processes line_buf */
static uint8_t line_buf[ACCUM_BUF_SIZE];
static size_t  line_len;

static uint8_t svc_data[] = {
    0xDA, 0xFE,
    (STEPPER_SERVICE_DATA >> 8) & 0xFF,
    STEPPER_SERVICE_DATA & 0xFF
};

static struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_SVC_DATA16, svc_data, sizeof(svc_data)),
    BT_DATA(BT_DATA_NAME_COMPLETE, ADV_NAME, ADV_NAME_LEN),
};

/* ──────────────────────────────────────────────
 * Advertising
 * ────────────────────────────────────────────── */
static int start_advertising(void)
{
    int err;

    bt_le_adv_stop();
    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return err;
    }
    LOG_INF("Configuration mode: waiting connections...");
    
    /* Start LED blinking for advertising state */
    led_state = false;
    gpio_pin_set_dt(&user_led, 1);  /* 1 for GPIO_ACTIVE_LOW = LED OFF */
    k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    
    return 0;
}

static void adv_retry_work_handler(struct k_work *work)
{
    int err = start_advertising();

    if (err == -ENOMEM) {
        k_work_schedule(&adv_retry_work, K_MSEC(200));
    }
}

/* ──────────────────────────────────────────────
 * LED Blink handler (for advertising state)
 * ────────────────────────────────────────────── */
static void led_blink_work_handler(struct k_work *work)
{
    /* Only blink if still in advertising mode (not connected) */
    if (!ble_conn && advertising_started) {
        led_state = !led_state;
        gpio_pin_set_dt(&user_led, led_state ? 0 : 1);  /* Invert for GPIO_ACTIVE_LOW */
        k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    }
}

static void led_rx_pulse_work_handler(struct k_work *work)
{
    if (ble_conn) {
        led_state = true;
        gpio_pin_set_dt(&user_led, 1);
    }
}

static void user_led_rx_activity(void)
{
    if (ble_conn) {
        led_state = false;
        gpio_pin_set_dt(&user_led, 0);
        k_work_reschedule(&led_rx_pulse_work, K_MSEC(LED_RX_PULSE_MS));
    }
}

static void mode_blink_work_handler(struct k_work *work)
{
    if (mode_blink_count < 4U) {
        gpio_pin_set_dt(&user_led, (mode_blink_count & 1U) ? 1 : 0);
        mode_blink_count++;
        k_work_reschedule(&mode_blink_work, K_MSEC(MODE_BLINK_INTERVAL_MS));
        return;
    }

    gpio_pin_set_dt(&user_led, 1);
    if (!ble_conn && advertising_started) {
        k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    }
}

static void mode_button_work_handler(struct k_work *work)
{
    bool half_step = app_stepper_motor_toggle_mode();

    LOG_INF("Stepper mode changed to %s", half_step ? "HS" : "FS");
    mode_blink_count = 0U;
    k_work_cancel_delayable(&led_blink_work);
    k_work_reschedule(&mode_blink_work, K_NO_WAIT);

    if (ble_conn != NULL) {
        int ret = app_stepper_motor_send_ui_update(ble_conn);

        if (ret != 0) {
            LOG_ERR("Failed to send mode UI update: %d", ret);
        }
    }
}

static void user_button_handler(const struct device *port,
                                struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    k_work_submit(&mode_button_work);
}

/* ──────────────────────────────────────────────
 * Connection callbacks
 * ────────────────────────────────────────────── */
static void connected(struct bt_conn *connected, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
    } else {
        LOG_INF("Connected");
        if (!ble_conn) {
            ble_conn = bt_conn_ref(connected);
        }
        /* Cancel blinking and turn LED ON */
        k_work_cancel_delayable(&led_blink_work);
        k_work_cancel_delayable(&led_rx_pulse_work);
        led_state = true;
        gpio_pin_set_dt(&user_led, 1);  /* 1 = LED ON */
        LOG_INF("LED ON (connected)");
    }
}

static void disconnected(struct bt_conn *disconn, uint8_t reason)
{
    app_stepper_motor_disconnect();
    if (ble_conn) {
        bt_conn_unref(ble_conn);
        ble_conn = NULL;
    }
    LOG_INF("Disconnected, reason %u %s", reason, bt_hci_err_to_str(reason));
    
    /* Start blinking again for re-advertising */
    k_work_cancel_delayable(&led_rx_pulse_work);
    led_state = false;
    gpio_pin_set_dt(&user_led, 1);  /* 1 for GPIO_ACTIVE_LOW = LED OFF */
    k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    
    k_work_schedule(&adv_retry_work, K_MSEC(200));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ──────────────────────────────────────────────
 * Hex conversion helper
 * ────────────────────────────────────────────── */
static int hex_char_val(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}


/**
 * Check if the entire buffer (ignoring whitespace/CR/LF) is valid hex pairs.
 * Returns the number of decoded bytes, or 0 if not valid hex.
 */
static int try_decode_hex(const uint8_t *buf, size_t len, uint8_t *out, size_t out_max)
{
    int count = 0;
    int nibble = -1;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        /* skip whitespace and delimiters */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }

        int val = hex_char_val(c);

        if (val < 0) {
            /* non-hex character found → not a hex command */
            return 0;
        }

        if (nibble < 0) {
            nibble = val;
        } else {
            if ((size_t)count >= out_max) {
                return 0; /* overflow */
            }
            out[count++] = (uint8_t)((nibble << 4) | val);
            nibble = -1;
        }
    }

    /* If there's a leftover nibble, the input wasn't aligned hex pairs */
    if (nibble >= 0) {
        return 0;
    }

    return count;
}

/* ──────────────────────────────────────────────
 * Vendor command work handler (system workqueue)
 * ────────────────────────────────────────────── */
static void vendor_work_handler(struct k_work *work)
{
    if (ble_conn && vendor_len_global > 0) {
        LOG_INF("Vendor TX: opcode=0x%02X, payload_len=%d",
            vendor_buf[0], vendor_len_global - 1);
        LOG_HEXDUMP_INF(vendor_buf, vendor_len_global, "vendor payload");

        int payload_len = (vendor_len_global > 1) ? (vendor_len_global - 1) : 0;
        uint8_t *payload = (vendor_len_global > 1) ? &vendor_buf[1] : NULL;

        int ret = trs_server_send_vendor_command(ble_conn,
                             vendor_buf[0],
                             payload_len,
                             payload);
        if (ret != 0) {
            LOG_ERR("Vendor send failed: %d", ret);
        } else {
            LOG_INF("Vendor command sent successfully (opcode=0x%02X, %d bytes)",
                vendor_buf[0], payload_len);
        }
    }
}

/* ──────────────────────────────────────────────
 * Process a complete UART line (work handler)
 *
 * Decision logic:
 *   a. If the line is valid hex AND first byte == 0x8E → vendor command
 *   b. Otherwise → transparent data (send raw ASCII over data pipe)
 * ────────────────────────────────────────────── */
static void process_line_work_handler(struct k_work *work)
{
    uint8_t local_buf[ACCUM_BUF_SIZE];
    size_t  local_len;

    /* Snapshot under lock (line_buf written from ISR context via
     * accum_buf copy – but actually we copy in ISR, so grab quickly) */
    unsigned int key = irq_lock();
    memcpy(local_buf, line_buf, line_len);
    local_len = line_len;
    irq_unlock(key);

    if (local_len == 0) {
        return;
    }

    LOG_HEXDUMP_DBG(local_buf, local_len, "UART line");

    /* ── Try to decode as hex vendor command ── */
    uint8_t hex_bytes[64];
    int hex_len = try_decode_hex(local_buf, local_len, hex_bytes, sizeof(hex_bytes));

    if (hex_len > 0 && hex_bytes[0] == 0x8E) {
        /* This is a vendor command → send via control pipe */
        if ((size_t)hex_len > sizeof(vendor_buf)) {
            hex_len = sizeof(vendor_buf);
        }
        unsigned int k = irq_lock();
        vendor_len_global = hex_len;
        memcpy(vendor_buf, hex_bytes, hex_len);
        irq_unlock(k);

        k_work_submit(&vendor_work);
        LOG_INF("Vendor command queued (%d bytes)", hex_len);
        return;
    }

    /* ── Regular transparent data → ring buffer → BLE data pipe ── */
    if (!ble_conn) {
        LOG_WRN("No BLE connection, discarding data");
        return;
    }

    /* Strip trailing CR/LF if you want clean data, or keep them – your choice.
     * Here we send everything as-is (raw ASCII). */
    uint8_t *ptr;
    size_t claimed = ring_buf_put_claim(&rx_ring, &ptr, local_len);

    if (claimed > 0) {
        size_t to_store = MIN(claimed, local_len);

        memcpy(ptr, local_buf, to_store);
        ring_buf_put_finish(&rx_ring, to_store);

        if (local_len > to_store) {
            LOG_ERR("Ring buffer overflow, %zu bytes lost",
                local_len - to_store);
        }
        k_work_submit(&uart_rx_work);
    } else {
        ring_buf_put_finish(&rx_ring, 0);
        LOG_ERR("Ring buffer full, dropping %zu bytes", local_len);
    }
}

/* ──────────────────────────────────────────────
 * BLE → UART (data pipe)
 * ────────────────────────────────────────────── */
static void uart_rx_work_handler(struct k_work *work)
{
    uint16_t tx_mtu = 0U;
    size_t bytes_read;

    trs_server_get_tx_mtu(ble_conn, &tx_mtu);

    if (tx_mtu > 0U) {
        uint8_t tx_buff[UART_TX_BUF_SIZE];

        while ((bytes_read = ring_buf_get(&rx_ring,
                          tx_buff, tx_mtu)) > 0) {
            if (trs_server_send_data(ble_conn, bytes_read,
                         tx_buff) < 0) {
                LOG_ERR("Failed to send through BLE");
                ring_buf_reset(&rx_ring);
                break;
            }
        }
    } else {
        ring_buf_reset(&rx_ring);
    }
}

static void ble_rx_work_handler(struct k_work *work)
{
    uint16_t data_len;

    trs_server_get_data_len(ble_conn, &data_len);

    if (data_len > 0) {
        struct net_buf *rec_buf = net_buf_alloc_len(&app_buff_pool,
                                (size_t)data_len,
                                K_FOREVER);
        if (rec_buf == NULL) {
            LOG_ERR("No memory");
            return;
        }

        if (trs_server_get_data(ble_conn,
                    net_buf_add(rec_buf, data_len)) > 0) {
            unsigned int key = irq_lock();
            bool was_empty = k_fifo_is_empty(&rx_queue);

            k_fifo_put(&rx_queue, rec_buf);
            if (was_empty) {
                uart_irq_tx_enable(uart_dev);
            }
            irq_unlock(key);
        } else {
            net_buf_unref(rec_buf);
        }
    }
}

/* ──────────────────────────────────────────────
 * TRS server callback
 * ────────────────────────────────────────────── */
static void trs_server_cb_fn(struct trs_server_event *event)
{
    switch (event->event_id) {
    case TRS_SERVER_EVT_RECEIVE_DATA:
        user_led_rx_activity();
        k_work_submit(&ble_rx_work);
        break;

    case TRS_SERVER_EVT_CBFC_ENABLED:
        LOG_INF("CBFC Rx enabled");
        break;

    case TRS_SERVER_EVT_CBFC_CREDIT:
        LOG_INF("CBFC Tx credit updated");
        break;

    case TRS_SERVER_EVT_VENDOR_CMD: {
        uint8_t *vdata = event->event_field.vendor_cmd.payload;
        uint16_t vlen  = event->event_field.vendor_cmd.length;

        if (vlen > 0 && vdata) {
            user_led_rx_activity();

            if (vdata[0] == STEPPER_OPCODE && ble_conn) {
                LOG_INF("Vendor cmd received size=%u", vlen);
                LOG_HEXDUMP_INF(vdata, vlen, "RX Vendor:");

                int ret = app_stepper_motor_handle_cmd(ble_conn, vdata[0],
                                                    vdata + 1, vlen - 1);
                if (ret != 0) {
                    LOG_ERR("Failed to handle stepper motor command: %d", ret);
                }
            } else {
                LOG_INF("Data received size=%u", vlen);
                LOG_HEXDUMP_INF(vdata, vlen, "RX Data:");

                if (ble_conn) {
                    uint8_t ack_data[UART_TX_BUF_SIZE];
                    const char ack_prefix[] = "data received :";
                    size_t prefix_len = sizeof(ack_prefix) - 1;
                    size_t copy_len = MIN((size_t)vlen, sizeof(ack_data) - prefix_len);

                    memcpy(ack_data, ack_prefix, prefix_len);
                    memcpy(&ack_data[prefix_len], vdata, copy_len);

                    int ack_ret = trs_server_send_data(ble_conn,
                                                       prefix_len + copy_len,
                                                       ack_data);
                    if (ack_ret < 0) {
                        LOG_ERR("Failed to send data acknowledgement: %d", ack_ret);
                    }
                }
            }

            /* Echo to UART/Docklight */
            for (uint16_t i = 0; i < vlen; i++) {
                uart_poll_out(uart_dev, vdata[i]);
            }
        }
        break;
    }

    case TRS_SERVER_EVT_ERR_UNSPECIFIED:
        LOG_ERR("TRS Error: Unspecified");
        break;

    case TRS_SERVER_EVT_ERR_NO_MEM:
        LOG_ERR("TRS Error: No memory");
        break;

    default:
        break;
    }
}

static struct trs_server_cb trs_server_event_cb = {
    .trs_server_event = trs_server_cb_fn,
};

/* ──────────────────────────────────────────────
 * Auth
 * ────────────────────────────────────────────── */
static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .cancel = auth_cancel,
};

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }
    LOG_INF("Bluetooth initialized");
    start_advertising();
}

/* ──────────────────────────────────────────────
 * UART ISR – accumulate bytes, dispatch complete
 * lines via work item
 *
 * Delimiter: CR (\r) or LF (\n)
 *
 * For Docklight: make sure your sequences end with
 * CR or LF (or CR+LF). Configure Docklight send
 * sequences accordingly.
 * ────────────────────────────────────────────── */
static void rx_uart_isr(void)
{
    uint8_t tmp[32];
    int n = uart_fifo_read(uart_dev, tmp, sizeof(tmp));

    for (int i = 0; i < n; i++) {
        uint8_t c = tmp[i];

        if (c == '\r' || c == '\n') {
            if (accum_pos > 0) {
                /* Complete line ready – copy to line_buf
                 * and schedule processing */
                memcpy(line_buf, accum_buf, accum_pos);
                line_len = accum_pos;
                accum_pos = 0;
                k_work_submit(&process_line_work);
            }
            /* else: ignore empty lines / consecutive delimiters */
        } else {
            if (accum_pos < ACCUM_BUF_SIZE) {
                accum_buf[accum_pos++] = c;
            } else {
                /* Overflow – discard this line */
                LOG_ERR("Accum buffer overflow, discarding");
                accum_pos = 0;
            }
        }
    }
}

static void tx_uart_data(void)
{
    static struct net_buf *rec_buf;
    int len;

    if (!rec_buf) {
        rec_buf = k_fifo_get(&rx_queue, K_NO_WAIT);
        if (!rec_buf) {
            uart_irq_tx_disable(uart_dev);
            return;
        }
    }

    len = uart_fifo_fill(uart_dev, rec_buf->data, rec_buf->len);
    net_buf_pull(rec_buf, len);

    if (!rec_buf->len) {
        net_buf_unref(rec_buf);
        rec_buf = NULL;
    }
}

static void uart_isr(const struct device *unused, void *user_data)
{
    ARG_UNUSED(unused);
    ARG_UNUSED(user_data);

    while (uart_irq_update(uart_dev) && uart_irq_is_pending(uart_dev)) {
        if (uart_irq_tx_ready(uart_dev)) {
            tx_uart_data();
        }
        if (uart_irq_rx_ready(uart_dev)) {
            rx_uart_isr();
        }
    }
}

static int uart_init(void)
{
    if (!device_is_ready(uart_dev)) {
        return -ENODEV;
    }

    uart_irq_rx_disable(uart_dev);
    uart_irq_tx_disable(uart_dev);
    uart_irq_callback_set(uart_dev, uart_isr);
    uart_irq_rx_enable(uart_dev);

    return 0;
}

/* ──────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────── */
int main(void)
{
    int err;

    k_work_init(&ble_rx_work, ble_rx_work_handler);
    k_work_init(&uart_rx_work, uart_rx_work_handler);
    k_work_init(&vendor_work, vendor_work_handler);
    k_work_init(&process_line_work, process_line_work_handler);
    k_work_init_delayable(&adv_retry_work, adv_retry_work_handler);
    k_work_init_delayable(&led_blink_work, led_blink_work_handler);
    k_work_init_delayable(&led_rx_pulse_work, led_rx_pulse_work_handler);
    k_work_init(&mode_button_work, mode_button_work_handler);
    k_work_init_delayable(&mode_blink_work, mode_blink_work_handler);
    advertising_started = true;

    LOG_INF("Initializing Bluetooth...");

    err = uart_init();
    if (err) {
        return 0;
    }

    /* Initialize LED GPIO */
    if (!gpio_is_ready_dt(&user_led)) {
        LOG_ERR("LED GPIO device not ready");
        return 0;
    }
    err = gpio_pin_configure_dt(&user_led, GPIO_OUTPUT);
    if (err < 0) {
        LOG_ERR("Failed to configure LED GPIO: %d", err);
        return 0;
    }
    gpio_pin_set_dt(&user_led, 1);  /* Start with LED OFF */
    led_state = false;
    LOG_INF("LED initialized");

    if (!gpio_is_ready_dt(&user_button)) {
        LOG_ERR("User button GPIO device not ready");
        return 0;
    }
    err = gpio_pin_configure_dt(&user_button, GPIO_INPUT);
    if (err != 0) {
        LOG_ERR("Failed to configure user button: %d", err);
        return 0;
    }
    gpio_init_callback(&user_button_cb, user_button_handler, BIT(user_button.pin));
    err = gpio_add_callback(user_button.port, &user_button_cb);
    if (err != 0) {
        LOG_ERR("Failed to add user button callback: %d", err);
        return 0;
    }
    err = gpio_pin_interrupt_configure_dt(&user_button, GPIO_INT_EDGE_TO_ACTIVE);
    if (err != 0) {
        LOG_ERR("Failed to configure user button interrupt: %d", err);
        return 0;
    }
    LOG_INF("User button toggles FS/HS step mode");

    bt_conn_auth_cb_register(&auth_cb_display);

    err = trs_server_cb_register(&trs_server_event_cb);
    if (err) {
        return 0;
    }

    /* Initialize stepper motor BLE module */
    err = app_stepper_motor_init();
    if (err) {
        LOG_ERR("Failed to initialize stepper motor BLE module: %d", err);
        return 0;
    }

    err = bt_enable(bt_ready);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
    }

    return 0;
}