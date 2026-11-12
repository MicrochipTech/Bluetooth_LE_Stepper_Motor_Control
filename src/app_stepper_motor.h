#ifndef APP_STEPPER_MOTOR_H
#define APP_STEPPER_MOTOR_H

#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

#define STEPPER_OPCODE               0x8E
#define STEPPER_SERVICE_DATA         0xFFE3
#define STEPPER_SUPPLY_VOLTAGE       6U
#define STEPPER_FS_STEPS_PER_REV     200U
#define STEPPER_HS_STEPS_PER_REV     400U
#define STEPPER_DEFAULT_DELAY_MS     5U

#define STEPPER_CMD_UI_UPDATE        0xA1
#define STEPPER_CMD_DIRECTION        0xA4
#define STEPPER_CMD_START_STOP       0xA5
#define STEPPER_CMD_STEPS            0xA6
#define STEPPER_CMD_DELAY            0xA7

#define STEPPER_RSP_UI_UPDATE        0xB1
#define STEPPER_RSP_DIRECTION        0xB4
#define STEPPER_RSP_START_STOP       0xB5
#define STEPPER_RSP_STEPS            0xB6
#define STEPPER_RSP_DELAY            0xB7

#define STEPPER_NTF_DIRECTION        0xC4
#define STEPPER_NTF_START_STOP       0xC5
#define STEPPER_NTF_STEPS            0xC6
#define STEPPER_NTF_DELAY            0xC7

#define STEPPER_ACK_UI_UPDATE        0xD1
#define STEPPER_ACK_DIRECTION        0xD4
#define STEPPER_ACK_START_STOP       0xD5
#define STEPPER_ACK_STEPS            0xD6
#define STEPPER_ACK_DELAY            0xD7

#define STEPPER_CW                   0U
#define STEPPER_CCW                  1U
#define STEPPER_STOP                 0U
#define STEPPER_START                1U

enum app_stepper_mode {
    STEPPER_MODE_FS = 0,
    STEPPER_MODE_HS = 1,
};

int app_stepper_motor_init(void);
void app_stepper_motor_disconnect(void);
bool app_stepper_motor_toggle_mode(void);
enum app_stepper_mode app_stepper_motor_get_mode(void);
uint16_t app_stepper_motor_get_steps_per_rev(void);
const char *app_stepper_motor_get_ui_name(void);
int app_stepper_motor_send_ui_update(struct bt_conn *conn);
int app_stepper_motor_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                  const uint8_t *payload, uint16_t payload_len);

#endif