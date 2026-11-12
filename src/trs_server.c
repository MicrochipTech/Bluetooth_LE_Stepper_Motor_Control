/** @file
 *  @brief GATT Transparent Service Server
 */

/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/init.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/check.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>

#include "trs.h"
#include "trs_server.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(trs_server);

/*Maximum number of concurrent connections supported by the device. */
#define TRS_SERVER_MAX_CONN_NBR                     CONFIG_BT_MAX_CONN

#define TRS_SERVER_ATT_DEFAULT_MTU                  (23U)

/*credit value for BLE Transparent server*/
#define TRS_SERVER_CREDIT                           CONFIG_TRS_SERVER_CREDIT
/*Maximum number of credits that can be returned to the peer device.*/
#define TRS_SERVER_MAX_RETURN_CREDIT                CONFIG_TRS_SERVER_CREDIT-3

/*Enable flag for CBFC (Credit Based Flow Control) transmission.*/
#define TRS_SERVER_CBFC_TX_ENABLED                  (1U)
/* Enable flag for CBFC reception.*/
#define TRS_SERVER_CBFC_RX_ENABLED                  (1U << 1U)

/*Opcode indicating the server has enabled CBFC.*/
#define TRS_SERVER_OPCODE_CBFC_ENABLED              CONFIG_TRS_SERVER_OPCODE_CBFC_ENABLED
/*Opcode for giving credit in CBFC.*/
#define TRS_SERVER_OPCODE_CBFC_GIVE_CREDIT          CONFIG_TRS_SERVER_OPCODE_CBFC_GIVE_CREDIT
/*Opcode indicating a successful operation in CBFC.*/
#define TRS_SERVER_OPCODE_CBFC_SUCCESS              (0U)

/*Minimum value for vendor-specific opcodes.*/
#define TRS_SERVER_VENDOR_OPCODE_MIN                CONFIG_TRS_SERVER_VENDOR_OPCODE_MIN

#define TRS_SERVER_BUF_POOL_SIZE                    CONFIG_TRS_SERVER_BUF_POOL_SIZE
#define TRS_SERVER_BUF_DATA_SIZE                    CONFIG_TRS_SERVER_BUF_DATA_SIZE

NET_BUF_POOL_DEFINE(trs_buff_pool, TRS_SERVER_BUF_POOL_SIZE, TRS_SERVER_BUF_DATA_SIZE, 0, NULL);

struct trs_packet_list{
    uint8_t   flags;
    struct net_buf *buf;
};

struct trs_server_queue{
    uint8_t     used_num;
    uint8_t     write_index; 
    uint8_t     read_index; 
    struct trs_packet_list packet_list[TRS_SERVER_CREDIT];
};

struct trs_conn_list
{
    struct bt_conn  *conn;
    uint16_t    att_mtu;
    uint8_t     cbfc_enable;
    uint8_t     peer_credit;
    uint8_t     local_credit; 
    struct trs_server_queue recv_pkt_queue;
};


/*An array to keep track of the connection list for BLE Transparent Service.*/
static struct trs_conn_list  conn_list[TRS_SERVER_MAX_CONN_NBR];
static sys_slist_t trs_server_cbs = SYS_SLIST_STATIC_INIT(&trs_cbs);

static bool check_queued_task(void)
{
    uint8_t i;
    struct bt_conn_info info;
    
    for(i=0; i < TRS_SERVER_MAX_CONN_NBR; i++){
        if (bt_conn_get_info(conn_list[i].conn, &info) == 0){
            if ((conn_list[i].conn != NULL) && 
                (info.state == BT_CONN_STATE_CONNECTED) &&
                (conn_list[i].peer_credit >= TRS_SERVER_MAX_RETURN_CREDIT)){
                        return true;
                }
            }
    }

    return false;
}

static void event_send(struct trs_server_event *event)
{
    struct trs_server_cb *listener;
    SYS_SLIST_FOR_EACH_CONTAINER(&trs_server_cbs, listener, _node){
        if (listener->trs_server_event){
            listener->trs_server_event(event);
        }
    }
}

static void server_return_credit(int conn_index)
{
    uint8_t notify_data[5U] = {0};
    uint16_t notify_len = 0U;

    if (conn_list[conn_index].peer_credit == 0U)
    {
        LOG_ERR("No credits available");
        return;
    };

    notify_data[notify_len] = TRS_SERVER_OPCODE_CBFC_SUCCESS;
    notify_len ++;
    notify_data[notify_len] = TRS_SERVER_OPCODE_CBFC_ENABLED;
    notify_len ++;
    notify_data[notify_len] = (uint8_t)(conn_list[conn_index].att_mtu>>8);
    notify_len ++;
    notify_data[notify_len] = (uint8_t)(conn_list[conn_index].att_mtu);
    notify_len ++;
    notify_data[notify_len] = conn_list[conn_index].peer_credit;
    notify_len ++;

    if (!bt_trs_cp_notify(conn_list[conn_index].conn, &notify_data[0], notify_len))
    {
        conn_list[conn_index].peer_credit = 0U;
    }
}

static int get_conn_list_index(struct bt_conn *conn)
{
    for(int i=0; i<TRS_SERVER_MAX_CONN_NBR; i++)
    {
        if ((conn_list[i].conn != NULL) && 
            (conn_list[i].conn == conn))
        {
            return i;
        }
    }
    return -ENOTCONN;
}

static int get_free_conn_list_index(void)
{
    for(int i=0; i<TRS_SERVER_MAX_CONN_NBR; i++)
    {
        if (conn_list[i].conn == NULL)
        {
            return i;
        }
    }
    return -ENOTCONN;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    if (conn_err) {
        LOG_ERR("Connection failed, err 0x%02x %s", conn_err, bt_hci_err_to_str(conn_err));
    } else {
        int conn_index = get_free_conn_list_index();
        if(conn_index >= 0){
            conn_list[conn_index].att_mtu = TRS_SERVER_ATT_DEFAULT_MTU;
            conn_list[conn_index].conn = bt_conn_ref(conn);
        }
        else{
            struct trs_server_event event;
            event.event_id = TRS_SERVER_EVT_ERR_UNSPECIFIED;
            event.event_field.conn = conn;
            event_send(&event);
        }
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    int conn_index = get_conn_list_index(conn);
    if(conn_index >= 0){
        while(conn_list[conn_index].recv_pkt_queue.used_num > 0){
            if (conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].buf != NULL)
            {
                net_buf_unref(conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].buf);
                conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].buf = NULL;
            }

            conn_list[conn_index].recv_pkt_queue.read_index++;
            if (conn_list[conn_index].recv_pkt_queue.read_index >= TRS_SERVER_CREDIT)
            {
                conn_list[conn_index].recv_pkt_queue.read_index = 0;
            }

            conn_list[conn_index].recv_pkt_queue.used_num --;
        }
        bt_conn_unref(conn_list[conn_index].conn);
        conn_list[conn_index].conn = NULL;
    }
}

static struct bt_conn_cb conn_cbs = {
    .connected = connected,
    .disconnected = disconnected,
};

void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    int conn_index = get_conn_list_index(conn);
    if(conn_index >= 0){
        conn_list[conn_index].att_mtu = tx;
    }
}

static struct bt_gatt_cb gatt_callbacks = {
    .att_mtu_updated = mtu_updated,
};

static int trs_rx_rec(struct bt_conn *conn, uint8_t* data, uint16_t length, uint8_t flags)
{
    int conn_index = get_conn_list_index(conn);
    if(conn_index < 0){
        LOG_ERR("No connection found");
        return conn_index;
    }
    if (conn_list[conn_index].recv_pkt_queue.used_num < TRS_SERVER_CREDIT)
    {
        struct trs_server_event event;
        struct net_buf *buf = net_buf_alloc_len(&trs_buff_pool, (size_t)length, K_NO_WAIT);
        if (buf == NULL) {
            LOG_ERR("No memory for TRS Server Rx packet");
            event.event_id = TRS_SERVER_EVT_ERR_NO_MEM;
            event.event_field.conn = conn;
            event_send(&event);
            return -ENOBUFS;
        }
        memcpy(net_buf_add(buf, (size_t)length), data, (size_t)length);
        conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.write_index].flags = flags;
        conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.write_index].buf = buf;
        conn_list[conn_index].recv_pkt_queue.write_index++;
        if (conn_list[conn_index].recv_pkt_queue.write_index >= TRS_SERVER_CREDIT){
            conn_list[conn_index].recv_pkt_queue.write_index = 0;
        }
        conn_list[conn_index].recv_pkt_queue.used_num++;

        event.event_id = TRS_SERVER_EVT_RECEIVE_DATA;
        event.event_field.conn = conn;
        event_send(&event);
    }

    return 0;
}

static int trs_cp_rec(struct bt_conn *conn, uint8_t* data, uint16_t length, uint8_t flags)
{
    struct trs_server_event event;
    int conn_index = get_conn_list_index(conn);
    if(conn_index < 0){
        LOG_ERR("No connection found");
        return conn_index;
    }
    (void)memset((uint8_t *) &event, 0, sizeof(event));
    switch(data[0])
    {
        case TRS_SERVER_OPCODE_CBFC_ENABLED:
        {
            conn_list[conn_index].cbfc_enable |= TRS_SERVER_CBFC_RX_ENABLED;
            conn_list[conn_index].peer_credit = TRS_SERVER_CREDIT;
            server_return_credit(conn_index);
            event.event_id=TRS_SERVER_EVT_CBFC_ENABLED;
            event.event_field.conn = conn;
            event_send(&event);
        }
        break;
        case TRS_SERVER_OPCODE_CBFC_GIVE_CREDIT:
        {
            conn_list[conn_index].cbfc_enable |= TRS_SERVER_CBFC_TX_ENABLED;
            conn_list[conn_index].local_credit += data[1];
            event.event_id=TRS_SERVER_EVT_CBFC_CREDIT;
            event.event_field.conn = conn;
            event_send(&event);
        }
        break;
        default:
        {
            if (data[0] >= TRS_SERVER_VENDOR_OPCODE_MIN)
            {
                event.event_id=TRS_SERVER_EVT_VENDOR_CMD;
                event.event_field.conn = conn;
                event.event_field.vendor_cmd.length = length;
                event.event_field.vendor_cmd.payload = data;
                event_send(&event);
            }
        }
        break;
    }

    return 0;
}

static struct bt_trs_cb trs_cb = {
    .rx_write = trs_rx_rec,
    .cp_write = trs_cp_rec,
};

static int trs_server_init(void)
{
    bt_conn_cb_register(&conn_cbs);
    bt_gatt_cb_register(&gatt_callbacks);
    bt_trs_cb_register(&trs_cb);
    return 0;
}

void trs_server_get_tx_mtu(struct bt_conn *conn , uint16_t *tx_mtu)
{
    int conn_index = get_conn_list_index(conn);
    *tx_mtu = 0U;
    if (conn_index >= 0){
        *tx_mtu = conn_list[conn_index].att_mtu;
    }
}

void trs_server_get_data_len(struct bt_conn *conn, uint16_t *data_len)
{
    int conn_index = get_conn_list_index(conn);
    *data_len = 0U;
    
    if (conn_list[conn_index].recv_pkt_queue.used_num > 0U && conn_index >= 0)
    {
        *data_len = conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].buf->len;
    }
}

int trs_server_get_data(struct bt_conn *conn, uint8_t *data)
{
    int ret = -ENODATA;
    uint8_t writeType = 0U;
    int conn_index = get_conn_list_index(conn);
    if(conn_index < 0){
        return conn_index;
    }
    if (conn_list[conn_index].recv_pkt_queue.used_num > 0U)
    {
        struct net_buf *buf = conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].buf;
        if (buf->data != NULL && buf->len > 0U)
        {
            ret = (int)(buf->len);
            (void)memcpy(data, buf->data, buf->len);
            net_buf_unref(buf);
            buf = NULL;
        }
        
        writeType = conn_list[conn_index].recv_pkt_queue.packet_list[conn_list[conn_index].recv_pkt_queue.read_index].flags;

        conn_list[conn_index].recv_pkt_queue.read_index++;
        if (conn_list[conn_index].recv_pkt_queue.read_index >= TRS_SERVER_CREDIT)
        {
            conn_list[conn_index].recv_pkt_queue.read_index = 0;
        }

        conn_list[conn_index].recv_pkt_queue.used_num --;
        
        if ((conn_list[conn_index].cbfc_enable & TRS_SERVER_CBFC_RX_ENABLED) && (writeType & BT_GATT_WRITE_FLAG_CMD))
        {
            conn_list[conn_index].peer_credit++;
            if (conn_list[conn_index].peer_credit >= TRS_SERVER_MAX_RETURN_CREDIT)
            {
                server_return_credit(conn_index);
            }
        }
    }
    
    return ret;
}

int trs_server_send_vendor_command(struct bt_conn *conn, uint8_t command_id, uint16_t command_length, uint8_t *command_payload)
{
    int result = 0;
    uint8_t send_buf[244] = {0};

    int conn_index = get_conn_list_index(conn);
    if(conn_index < 0){
        LOG_ERR("No connection found");
        return conn_index;
    }

    if(bt_trs_cp_ccc_config_status(conn) == false){
        LOG_ERR("Control Point notification not enabled");
        return -ENOTSUP;
    }

    if (command_id < TRS_SERVER_VENDOR_OPCODE_MIN){
        LOG_ERR("Not a valid command ID");
        return -EINVAL;
    }

    if (command_length > (conn_list[conn_index].att_mtu-4U)){
        LOG_ERR("Data length is more than expected");
        return -EINVAL;
    }

    if(command_length > 0U && command_payload == NULL){
        LOG_ERR("Command payload is NULL");
        return -ENODATA;
    }


    send_buf[0] = command_id;
    if(command_length > 0U){
        (void)memcpy(&send_buf[1], command_payload, command_length);
    }
    result = bt_trs_cp_notify(conn_list[conn_index].conn, &send_buf[0], (uint16_t)(command_length+1U));
    
    return result;
}

int trs_server_send_data(struct bt_conn *conn, uint16_t len, uint8_t *data)
{
    int result = 0;
    uint8_t send_buf[244] = {0};

    int conn_index = get_conn_list_index(conn);
    if(conn_index < 0){
        LOG_ERR("No connection found");
        return conn_index;
    }

    if(bt_trs_tx_ccc_config_status(conn) == false){
        LOG_ERR("Tx notification not enabled");
        return -ENOTSUP;
    }

    if(((conn_list[conn_index].cbfc_enable&TRS_SERVER_CBFC_TX_ENABLED)!=0U) 
        && (conn_list[conn_index].local_credit == 0U)){
        LOG_ERR("No credits available");
        return -ENOBUFS;
    }

    if (check_queued_task()){
        LOG_ERR("More packets in the queue to process");
        return -ENOBUFS;
    }

    if(len > (conn_list[conn_index].att_mtu-4U)){
        LOG_ERR("Data length is more than expected");
        return -EINVAL;
    }

    (void)memcpy(&send_buf[0], data, len);

    result = bt_trs_tx_notify(conn_list[conn_index].conn, &send_buf[0], len);
    if(!result){
        if ((conn_list[conn_index].cbfc_enable&TRS_SERVER_CBFC_TX_ENABLED) != 0U){
            conn_list[conn_index].local_credit--;
        }
    }

    return result;
}

int trs_server_cb_register(struct trs_server_cb *cb)
{
    CHECKIF(cb == NULL) {
        return -EINVAL;
    }

    sys_slist_append(&trs_server_cbs, &cb->_node);

    return 0;
}

int trs_server_cb_unregister(struct trs_server_cb *cb)
{
    CHECKIF(cb == NULL){
        return -EINVAL;
    }

    if (!sys_slist_find_and_remove(&trs_server_cbs, &cb->_node)) {
        return -ENOENT;
    }

    return 0;
}

SYS_INIT(trs_server_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);