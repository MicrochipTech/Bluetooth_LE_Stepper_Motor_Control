/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TRANSPARENT_SERVICE_H_
#define TRANSPARENT_SERVICE_H_

/**
 * @brief Transparent Service (TRS)
 */
#include <stdint.h>
#include <zephyr/sys/slist.h>
#include <zephyr/bluetooth/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 128-bit UUIDs for Service and Characteristics */
/**
 *  @brief Transparent Service UUID value
 */
#define BT_UUID_TRS_VAL 0x55,0xE4,0x05,0xD2,0xAF,0x9F,0xA9,0x8F,\
                        0xE5,0x4A,0x7D,0xFE,0x43,0x53,0x53,0x49
/**
 *  @brief TRS Characteristic Tx UUID value
 */
#define BT_UUID_TX_CHAR_VAL 0x16,0x96,0x24,0x47,0xC6,0x23,0x61,0xBA,\
                            0xD9,0x4B,0x4D,0x1E,0x43,0x53,0x53,0x49
/**
 *  @brief TRS Characteristic Rx UUID value
 */
#define BT_UUID_RX_CHAR_VAL 0xB3,0x9B,0x72,0x34,0xBE,0xEC,0xD4,0xA8,\
                            0xF4,0x43,0x41,0x88,0x43,0x53,0x53,0x49
/**
 *  @brief TRS Characteristic Control Point UUID value
 */
#define BT_UUID_CP_CHAR_VAL 0x7E,0x3B,0x07,0xFF,0x1C,0x51,0x49,0x2F,\
                            0xB3,0x39,0x8A,0x4C,0x43,0x53,0x53,0x49

/** @brief Transparent service callback structure */
struct bt_trs_cb {
    /**
     * @brief Transparent Rx write callback
     *
     * @param conn Connection object.
     * @param data Pointer to Attribute data.
     * @param length Attribute value length.
     * @param flags GATT attribute write flags
     *
     * @return 0 on successful handling of Rx Write
     * @return -ENOTSUP if not supported. It can be used to pass handling to other
     *         listeners in case of multiple listeners
     * @return other negative error codes will result in immediate error response
     */
    int (*rx_write)(struct bt_conn *conn, uint8_t* data, uint16_t length, uint8_t flags);

    /**
     * @brief Transparent control point write callback
     *
     * @param conn Connection object.
     * @param data Pointer to Attribute data.
     * @param length Attribute value length.
     * @param flags GATT attribute write flags
     *
     * @return 0 on successful handling of CP Write
     * @return -ENOTSUP if not supported. It can be used to pass handling to other
     *         listeners in case of multiple listeners
     * @return other negative error codes will result in immediate error response
     */
    int (*cp_write)(struct bt_conn *conn, uint8_t* data, uint16_t length, uint8_t flags);

    /**
     * @brief Transparent tx write callback
     *
     * @param conn Connection object.
     * @param data Pointer to Attribute data.
     * @param length Attribute value length.
     * @param flags GATT attribute write flags
     *
     * @return 0 on successful handling of Tx Write
     * @return -ENOTSUP if not supported. It can be used to pass handling to other
     *         listeners in case of multiple listeners
     * @return other negative error codes will result in immediate error response
     */
    int (*tx_write)(struct bt_conn *conn, uint8_t* data, uint16_t length, uint8_t flags);

    /** @brief Tx charcateristic notifications changed
     *
     * @param enabled Flag that is true if notifications were enabled, false
     *                if they were disabled.
     */
    void (*tx_ntf_changed)(bool enabled);

    /** @brief cp charcateristic notifications changed
     *
     * @param enabled Flag that is true if notifications were enabled, false
     *                if they were disabled.
     */
    void (*cp_ntf_changed)(bool enabled);

    /** Internal member to form a list of callbacks */
    sys_snode_t _node;
};

/** @brief Notify transmit data.
 *
 * This will send a GATT notification to specific connection.
 *
 *  @param conn Connection object.
 *  @param data Pointer to Attribute data.
 *  @param len  Attribute value length.
 *
 *  @return Zero in case of success and error code in case of error.
 */
int bt_trs_tx_notify(struct bt_conn *conn, uint8_t* data, uint16_t len);

/** @brief Notify controle point data.
 *
 * This will send a GATT notification to specific connection.
 *
 *  @param conn Connection object.
 *  @param data Pointer to Attribute data.
 *  @param len  Attribute value length.
 *
 *  @return Zero in case of success and error code in case of error.
 */
int bt_trs_cp_notify(struct bt_conn *conn, uint8_t* data, uint16_t len);

/** @brief Get the Tx characteristic client configuration status.
 *
 * This will get the Tx characteristic client configuration status of 
 * a specific connection.
 *
 *  @param conn Connection object.
 *
 *  @return true if the attribute object has been subscribed.
 */
bool bt_trs_tx_ccc_config_status(struct bt_conn *conn);

/** @brief Get the CP characteristic client configuration status.
 *
 * This will get the CP characteristic client configuration status of 
 * a specific connection.
 *
 *  @param conn Connection object.
 *
 *  @return true if the attribute object has been subscribed.
 */
bool bt_trs_cp_ccc_config_status(struct bt_conn *conn);

/** @brief Transparent service callback register
 *
 * This function will register callbacks that will be called in
 * certain events related to Transparent Service.
 *
 * @param cb Pointer to callbacks structure. Must point to memory that remains valid
 * until unregistered.
 *
 * @return 0 on success
 * @return -EINVAL in case @p cb is NULL
 */
int bt_trs_cb_register(struct bt_trs_cb *cb);

/** @brief Transparent service callback unregister
 *
 * This function will unregister callback from Transparent service.
 *
 * @param cb Pointer to callbacks structure
 *
 * @return 0 on success
 * @return -EINVAL in case @p cb is NULL
 * @return -ENOENT in case the @p cb was not found in registered callbacks
 */
int bt_trs_cb_unregister(struct bt_trs_cb *cb);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* TRANSPARENT_SERVICE_H_ */