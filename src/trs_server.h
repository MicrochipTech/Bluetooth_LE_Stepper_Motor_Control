/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TRANSPARENT_SERVICE_SERVER_H_
#define TRANSPARENT_SERVICE_SERVER_H_

/**
 * @brief Transparent Service Server
 */
#include <stdint.h>
#include <zephyr/sys/slist.h>

#ifdef __cplusplus
extern "C" {
#endif

enum trs_server_event_id {
    TRS_SERVER_EVT_RECEIVE_DATA= 1,
    TRS_SERVER_EVT_CBFC_ENABLED,
    TRS_SERVER_EVT_CBFC_CREDIT,
    TRS_SERVER_EVT_VENDOR_CMD,
    TRS_SERVER_EVT_ERR_UNSPECIFIED,
    TRS_SERVER_EVT_ERR_NO_MEM,
};

struct trs_server_vendor_cmd{
    uint16_t        length;
    uint8_t        *payload;
};

struct trs_server_event_field{
    struct bt_conn *conn;
    struct trs_server_vendor_cmd vendor_cmd;
};

struct  trs_server_event
{
    enum trs_server_event_id event_id;                 
    struct trs_server_event_field event_field;               
};

/** @brief Transparent service callback structure */
struct trs_server_cb {
    /**
     * @brief Transparent server callback
     *
     * @param conn Connection object.
     * @param event Pointer to the event structure containing details about the event.
     *
     * @return 0 on successful handling of trs server event
     * @return -ENOTSUP if not supported. It can be used to pass handling to other
     *         listeners in case of multiple listeners
     * @return other negative error codes will result in immediate error response
     */
    void (*trs_server_event)(struct  trs_server_event *event);


    /** Internal member to form a list of callbacks */
    sys_snode_t _node;
};

/**
 * @brief Transparent service server callback register
 *
 * @param cb pointer to required callback
 *
 * @return 0 on success
 * @return negative error codes on failure
 */
int trs_server_cb_register(struct trs_server_cb *cb);

/** @brief Transparent service server callback unregister
 *
 * @param cb Pointer to callbacks structure
 *
 * @return 0 on success
 * @return -EINVAL in case @p cb is NULL
 * @return -ENOENT in case the @p cb was not found in registered callbacks
 */
int trs_server_cb_unregister(struct trs_server_cb *cb);

/** @brief Retrieves tx mtu size
 *
 * @param conn Connection object.
 * @param event Pointer to where the mtu will be stored.
 *
 * @return none
 */
void trs_server_get_tx_mtu(struct bt_conn *conn , uint16_t *tx_mtu);

/** @brief Retrieves the length of data queued
 *
 * @param conn Connection object.
 * @param event Pointer to where the data length will be stored.
 *
 * @return none
 */
void trs_server_get_data_len(struct bt_conn *conn, uint16_t *data_len);

/** @brief Retrieves queued data
 *
 * @param conn Connection object.
 * @param data Pointer to the buffer where the data will be stored.
 *
 * @return 0 on success
 * @return -ENOTCONN in case not a valid connection
 * @return -ENODATA in case there is no data to be retrieved
 */
int trs_server_get_data(struct bt_conn *conn, uint8_t *data);

/** @brief Sends a vendor-specific command over BLE.
 *
 * @param conn Connection object.
 * @param command_id The identifier of the vendor-specific command.
 * @param command_length The length of the command payload.
 * @param command_payload Pointer to the command payload data.
 *
 * @return 0 on success
 * @return -ENOTCONN in case not a valid connection
 * @return -EINVAL Invalid parameters; incorrect commandID, commandLength.
 * @return -ENOTSUP in case the CCCD of TCP is not enabled.
 */
int trs_server_send_vendor_command(struct bt_conn *conn, uint8_t command_id, 
                                   uint16_t command_length, uint8_t *command_payload);

/** @brief Sends transparent data over BLE.
 *
 * @param conn Connection object.
 * @param len The length of the data to be sent
 * @param data Pointer to the data to be sent.
 *
 * @return 0 on success
 * @return -ENOTCONN in case not a valid connection.
 * @return -ENOTSUP in case the CCCD of TCP is not enabled.
 * @return -ENOBUFS in case no available credits.
 * @return -EINVAL Invalid parameters; incorrect commandID, commandLength.
 */
int trs_server_send_data(struct bt_conn *conn, uint16_t len, uint8_t *data);


#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* TRANSPARENT_SERVICE_SERVER_H_ */