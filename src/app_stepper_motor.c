#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include "app_stepper_motor.h"
#include "trs_server.h"

LOG_MODULE_REGISTER(app_stepper_motor);

#define PHASE_COUNT 4U
#define MIN_STEP_INTERVAL_MS 1U
#define CONTINUOUS_STEP_INTERVAL_MS 1U
#define STEPPER_WORK_STACK_SIZE 2048U
#define STEPPER_UI_MIN_MTU 35U
#define PHASE_OFF 0xFFU

static const uint8_t hs_high_index[8][2] = {
    {0, 2}, {PHASE_OFF, 2}, {1, 2}, {1, PHASE_OFF},
    {1, 3}, {PHASE_OFF, 3}, {0, 3}, {0, PHASE_OFF},
};
static const uint8_t hs_low_index[8][2] = {
    {1, 3}, {PHASE_OFF, 3}, {0, 3}, {0, PHASE_OFF},
    {0, 2}, {PHASE_OFF, 2}, {1, 2}, {1, PHASE_OFF},
};
static const uint8_t fs_high_index[4][2] = {
    {0, 2}, {1, 2}, {1, 3}, {0, 3},
};
static const uint8_t fs_low_index[4][2] = {
    {1, 3}, {0, 3}, {0, 2}, {1, 2},
};

static const struct gpio_dt_spec drive_high[PHASE_COUNT] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_a_high), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_b_high), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_c_high), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_d_high), gpios),
};
static const struct gpio_dt_spec drive_low[PHASE_COUNT] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_a_low), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_b_low), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_c_low), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(stepper_d_low), gpios),
};

static struct {
    uint16_t steps;
    uint16_t delay_ms;
    uint16_t active_steps;
    uint16_t active_delay_ms;
    uint16_t remaining;
    int64_t move_start_ms;
    uint8_t direction;
    uint8_t running;
    uint8_t phase;
    uint8_t continuous;
    uint8_t first_step;
    enum app_stepper_mode mode;
    struct bt_conn *conn;
} motor;
static struct k_work_delayable step_work;
static K_MUTEX_DEFINE(motor_lock);
static struct k_work_q motor_work_queue;
static K_THREAD_STACK_DEFINE(motor_work_stack, STEPPER_WORK_STACK_SIZE);
static struct k_work_delayable ui_update_work;
static struct bt_conn *ui_pending_conn;
static void ui_update_work_handler(struct k_work *work);

static int send_frame(struct bt_conn *conn, uint8_t id, const uint8_t *data, size_t len)
{
    uint8_t frame[32] = { (uint8_t)(len + 1U), id };

    if (len > sizeof(frame) - 2U) {
        return -EINVAL;
    }
    if (len != 0U) {
        memcpy(&frame[2], data, len);
    }
    return trs_server_send_vendor_command(conn, STEPPER_OPCODE, len + 2U, frame);
}

static int send_word(struct bt_conn *conn, uint8_t id, uint16_t value)
{
    uint8_t data[] = { (uint8_t)(value >> 8), (uint8_t)value };

    return send_frame(conn, id, data, sizeof(data));
}

static int all_off(void)
{
    int result = 0;

    for (size_t i = 0; i < PHASE_COUNT; i++) {
        int ret = gpio_pin_set_dt(&drive_high[i], 0);

        if (ret != 0) {
            result = ret;
        }
    }
    for (size_t i = 0; i < PHASE_COUNT; i++) {
        int ret = gpio_pin_set_dt(&drive_low[i], 0);

        if (ret != 0) {
            result = ret;
        }
    }
    return result;
}

static void stop_motor(bool notify)
{
    k_mutex_lock(&motor_lock, K_FOREVER);
    k_work_cancel_delayable(&step_work);
    motor.running = STEPPER_STOP;
    motor.remaining = 0U;
    motor.continuous = 0U;
    motor.first_step = 0U;
    if (all_off() != 0) {
        LOG_ERR("Failed to disable stepper bridges");
    }
    if (motor.conn != NULL) {
        if (notify) {
            uint8_t stopped = STEPPER_STOP;

            (void)send_frame(motor.conn, STEPPER_NTF_START_STOP, &stopped, 1U);
        }
        bt_conn_unref(motor.conn);
        motor.conn = NULL;
    }
    k_mutex_unlock(&motor_lock);
}

/* Change only the reversing winding; the other winding stays energized. */
static int apply_phase(uint8_t phase, uint8_t previous_phase, bool starting)
{
    const uint8_t (*high_index)[2] = motor.mode == STEPPER_MODE_HS ?
                                     hs_high_index : fs_high_index;
    const uint8_t (*low_index)[2] = motor.mode == STEPPER_MODE_HS ?
                                    hs_low_index : fs_low_index;
    int ret;

    for (size_t i = 0; i < 2U; i++) {
        if (!starting && high_index[phase][i] == high_index[previous_phase][i]) {
            continue;
        }
        ret = gpio_pin_set_dt(&drive_high[2U * i], 0);
        if (ret == 0) {
            ret = gpio_pin_set_dt(&drive_high[2U * i + 1U], 0);
        }
        if (ret == 0) {
            ret = gpio_pin_set_dt(&drive_low[2U * i], 0);
        }
        if (ret == 0) {
            ret = gpio_pin_set_dt(&drive_low[2U * i + 1U], 0);
        }
        if (ret != 0) {
            (void)all_off();
            return ret;
        }
        k_busy_wait(100);
        if (high_index[phase][i] == PHASE_OFF) {
            continue;
        }
        ret = gpio_pin_set_dt(&drive_low[low_index[phase][i]], 1);
        if (ret != 0) {
            (void)all_off();
            return ret;
        }
        ret = gpio_pin_set_dt(&drive_high[high_index[phase][i]], 1);
        if (ret != 0) {
            (void)all_off();
            return ret;
        }
    }
    return 0;
}

static void step_work_handler(struct k_work *work)
{
    k_mutex_lock(&motor_lock, K_FOREVER);
    if (motor.running != STEPPER_START) {
        k_mutex_unlock(&motor_lock);
        return;
    }
    if (motor.remaining == 0U && motor.continuous == 0U) {
        k_mutex_unlock(&motor_lock);
        stop_motor(true);
        return;
    }
    uint8_t previous_phase = motor.phase;

    uint8_t state_count = motor.mode == STEPPER_MODE_HS ? 8U : 4U;

    motor.phase = (motor.phase + (motor.direction == STEPPER_CW ? 1U :
                                  state_count - 1U)) % state_count;
    if (apply_phase(motor.phase, previous_phase, motor.first_step != 0U) != 0) {
        LOG_ERR("Stepper output error; stopping");
        k_mutex_unlock(&motor_lock);
        stop_motor(true);
        return;
    }
    motor.first_step = 0U;
    if (motor.continuous == 0U) {
        motor.remaining--;
    } else {
        k_work_reschedule_for_queue(&motor_work_queue, &step_work,
                                    K_MSEC(CONTINUOUS_STEP_INTERVAL_MS));
        k_mutex_unlock(&motor_lock);
        return;
    }
    uint32_t completed = (uint32_t)motor.active_steps - motor.remaining;
    int64_t next_due_ms = motor.move_start_ms +
                          (int64_t)completed * motor.active_delay_ms;
    int64_t wait_ms = next_due_ms - k_uptime_get();

    k_work_reschedule_for_queue(&motor_work_queue, &step_work,
                                K_MSEC(MAX(wait_ms, MIN_STEP_INTERVAL_MS)));
    k_mutex_unlock(&motor_lock);
}

int app_stepper_motor_init(void)
{
    memset(&motor, 0, sizeof(motor));
    motor.mode = STEPPER_MODE_FS;
    motor.steps = STEPPER_FS_STEPS_PER_REV;
    motor.delay_ms = STEPPER_DEFAULT_DELAY_MS;
    k_work_init_delayable(&step_work, step_work_handler);
    k_work_init_delayable(&ui_update_work, ui_update_work_handler);
    k_work_queue_start(&motor_work_queue, motor_work_stack,
                       K_THREAD_STACK_SIZEOF(motor_work_stack), 5, NULL);

    for (size_t i = 0; i < PHASE_COUNT; i++) {
        if (!gpio_is_ready_dt(&drive_high[i]) || !gpio_is_ready_dt(&drive_low[i])) {
            return -ENODEV;
        }
        int ret = gpio_pin_configure_dt(&drive_high[i], GPIO_OUTPUT_INACTIVE);

        if (ret != 0) {
            return ret;
        }
        ret = gpio_pin_configure_dt(&drive_low[i], GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            return ret;
        }
    }
    return all_off();
}

void app_stepper_motor_disconnect(void)
{
    k_work_cancel_delayable(&ui_update_work);
    k_mutex_lock(&motor_lock, K_FOREVER);
    /* BLE loss must not cancel an active open-loop move. */
    if (motor.conn != NULL) {
        bt_conn_unref(motor.conn);
        motor.conn = NULL;
    }
    if (ui_pending_conn != NULL) {
        bt_conn_unref(ui_pending_conn);
        ui_pending_conn = NULL;
    }
    k_mutex_unlock(&motor_lock);
}

bool app_stepper_motor_toggle_mode(void)
{
    if (motor.running == STEPPER_START) {
        stop_motor(false);
    }
    k_mutex_lock(&motor_lock, K_FOREVER);
    motor.mode = motor.mode == STEPPER_MODE_FS ? STEPPER_MODE_HS : STEPPER_MODE_FS;
    motor.steps = motor.mode == STEPPER_MODE_HS ? STEPPER_HS_STEPS_PER_REV :
                   STEPPER_FS_STEPS_PER_REV;
    motor.phase = 0U;
    k_mutex_unlock(&motor_lock);
    return motor.mode == STEPPER_MODE_HS;
}

enum app_stepper_mode app_stepper_motor_get_mode(void)
{
    return motor.mode;
}

uint16_t app_stepper_motor_get_steps_per_rev(void)
{
    return motor.mode == STEPPER_MODE_HS ? STEPPER_HS_STEPS_PER_REV :
           STEPPER_FS_STEPS_PER_REV;
}

const char *app_stepper_motor_get_ui_name(void)
{
    return motor.mode == STEPPER_MODE_HS ? "Stepper Motor - HS" :
           "Stepper Motor - FS";
}

int app_stepper_motor_send_ui_update(struct bt_conn *conn)
{
    const char *name = app_stepper_motor_get_ui_name();
    uint8_t data[29];
    size_t index = 0U;

    data[index++] = strlen(name);
    memcpy(&data[index], name, strlen(name));
    index += strlen(name);
    data[index++] = 0U;
    data[index++] = STEPPER_SUPPLY_VOLTAGE;
    uint16_t steps_per_rev = app_stepper_motor_get_steps_per_rev();
    data[index++] = (uint8_t)(steps_per_rev >> 8);
    data[index++] = (uint8_t)steps_per_rev;
    data[index++] = (uint8_t)(motor.steps >> 8);
    data[index++] = (uint8_t)motor.steps;
    data[index++] = (uint8_t)(motor.delay_ms >> 8);
    data[index++] = (uint8_t)motor.delay_ms;
    data[index++] = motor.direction;
    data[index++] = motor.running;
    return send_frame(conn, STEPPER_RSP_UI_UPDATE, data, index);
}

static void ui_update_work_handler(struct k_work *work)
{
    k_mutex_lock(&motor_lock, K_FOREVER);
    if (ui_pending_conn != NULL) {
        int ret = bt_gatt_get_mtu(ui_pending_conn) < STEPPER_UI_MIN_MTU ?
                  -EMSGSIZE : app_stepper_motor_send_ui_update(ui_pending_conn);

        if (ret != 0) {
            LOG_ERR("UI update requires ATT MTU >= %u: %d", STEPPER_UI_MIN_MTU, ret);
        }
        bt_conn_unref(ui_pending_conn);
        ui_pending_conn = NULL;
    }
    k_mutex_unlock(&motor_lock);
}

static void mtu_exchange_done(struct bt_conn *conn, uint8_t err,
                              struct bt_gatt_exchange_params *params)
{
    if (err != 0U) {
        LOG_ERR("MTU exchange failed: %u", err);
    }
    k_work_reschedule(&ui_update_work, K_NO_WAIT);
}

static struct bt_gatt_exchange_params mtu_exchange_params = {
    .func = mtu_exchange_done,
};

int app_stepper_motor_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                  const uint8_t *payload, uint16_t payload_len)
{
    uint8_t id;
    uint16_t value = 0U;
    bool begin_move = false;
    int ret;

    if (opcode != STEPPER_OPCODE || conn == NULL || payload == NULL ||
        payload_len < 2U || payload[0] != payload_len - 1U) {
        return -EINVAL;
    }
    id = payload[1];
    switch (id) {
    case STEPPER_CMD_UI_UPDATE:
        if (payload_len != 2U) {
            return -EINVAL;
        }
        ret = send_frame(conn, STEPPER_ACK_UI_UPDATE, NULL, 0U);
        if (ret != 0) {
            return ret;
        }
        if (bt_gatt_get_mtu(conn) >= STEPPER_UI_MIN_MTU) {
            return app_stepper_motor_send_ui_update(conn);
        }
        k_mutex_lock(&motor_lock, K_FOREVER);
        if (ui_pending_conn != NULL) {
            k_mutex_unlock(&motor_lock);
            return -EBUSY;
        }
        ui_pending_conn = bt_conn_ref(conn);
        k_mutex_unlock(&motor_lock);
        ret = bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
        if (ret != 0) {
            LOG_ERR("Cannot exchange MTU for UI update: %d", ret);
            k_mutex_lock(&motor_lock, K_FOREVER);
            bt_conn_unref(ui_pending_conn);
            ui_pending_conn = NULL;
            k_mutex_unlock(&motor_lock);
        }
        return ret;

    case STEPPER_CMD_STEPS:
    case STEPPER_CMD_DELAY:
        if (payload_len != 4U) {
            return -EINVAL;
        }
        value = ((uint16_t)payload[2] << 8) | payload[3];
        if ((id == STEPPER_CMD_STEPS && value == 0U) ||
            (id == STEPPER_CMD_DELAY && value != 0U &&
             value < MIN_STEP_INTERVAL_MS)) {
            return -EINVAL;
        }
        if (id == STEPPER_CMD_STEPS) {
            motor.steps = value;
        } else {
            motor.delay_ms = value;
        }
        ret = send_frame(conn, id == STEPPER_CMD_STEPS ? STEPPER_ACK_STEPS :
                         STEPPER_ACK_DELAY, NULL, 0U);
        if (ret != 0) {
            return ret;
        }
        ret = send_word(conn, id == STEPPER_CMD_STEPS ? STEPPER_RSP_STEPS :
                        STEPPER_RSP_DELAY, value);
        return ret == 0 ? send_word(conn, id == STEPPER_CMD_STEPS ?
                                    STEPPER_NTF_STEPS : STEPPER_NTF_DELAY, value) : ret;

    case STEPPER_CMD_DIRECTION:
        if (payload_len != 3U || payload[2] > STEPPER_CCW) {
            return -EINVAL;
        }
        bool stopped_for_direction = motor.running == STEPPER_START &&
                                     motor.direction != payload[2];

        if (stopped_for_direction) {
            stop_motor(false);
        }
        motor.direction = payload[2];
        ret = send_frame(conn, STEPPER_ACK_DIRECTION, NULL, 0U);
        if (ret != 0) {
            return ret;
        }
        ret = send_frame(conn, STEPPER_RSP_DIRECTION, &motor.direction, 1U);
        if (ret != 0) {
            return ret;
        }
        ret = send_frame(conn, STEPPER_NTF_DIRECTION, &motor.direction, 1U);
        if (ret != 0 || !stopped_for_direction) {
            return ret;
        }
        uint8_t stopped = STEPPER_STOP;

        return send_frame(conn, STEPPER_NTF_START_STOP, &stopped, 1U);

    case STEPPER_CMD_START_STOP:
        if (payload_len != 3U || payload[2] > STEPPER_START) {
            return -EINVAL;
        }
        if (payload[2] == STEPPER_START && motor.running == STEPPER_STOP) {
            begin_move = true;
        } else if (payload[2] == STEPPER_STOP) {
            stop_motor(false);
        }
        ret = send_frame(conn, STEPPER_ACK_START_STOP, NULL, 0U);
        if (ret != 0) {
            return ret;
        }
        ret = send_frame(conn, STEPPER_RSP_START_STOP, &payload[2], 1U);
        if (ret != 0) {
            return ret;
        }
        ret = send_frame(conn, STEPPER_NTF_START_STOP, &payload[2], 1U);
        if (ret != 0) {
            return ret;
        }
        if (begin_move) {
            motor.move_start_ms = k_uptime_get();
            motor.active_steps = motor.steps;
            motor.continuous = motor.delay_ms == 0U;
            motor.active_delay_ms = motor.continuous != 0U ?
                                    CONTINUOUS_STEP_INTERVAL_MS : motor.delay_ms;
            motor.remaining = motor.active_steps;
            motor.first_step = 1U;
            motor.running = STEPPER_START;
            motor.conn = bt_conn_ref(conn);
            k_work_reschedule_for_queue(&motor_work_queue, &step_work, K_NO_WAIT);
        }
        return 0;
    default:
        return -EINVAL;
    }
}