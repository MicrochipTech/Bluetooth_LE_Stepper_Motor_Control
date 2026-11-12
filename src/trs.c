/** @file
 *  @brief GATT Transparent Service
 */

/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/sys/__assert.h>
#include <stdbool.h>
#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/sys/check.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include "trs.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(trs);

/**
 * @brief GATT ATTR Error that should be returned in case
 * HRS Control point request is not supported.
 */
#define BT_TRS_ATT_ERR_APPLICATION_ERROR 0x80

static sys_slist_t trs_cbs = SYS_SLIST_STATIC_INIT(&trs_cbs);

static const struct bt_uuid_128 trs_uuid = BT_UUID_INIT_128(BT_UUID_TRS_VAL );
static const struct bt_uuid_128 tx_char_uuid = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static const struct bt_uuid_128 rx_char_uuid = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);
static const struct bt_uuid_128 cp_char_uuid = BT_UUID_INIT_128(BT_UUID_CP_CHAR_VAL);

static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
   int err = -ENOTSUP;
   struct bt_trs_cb *listener;

   if ((flags & BT_GATT_WRITE_FLAG_PREPARE)) {
      /* Prepare Write Request received. Reject it since this Characteristic
      * Won't support Prepare Write.
      */
      return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
   }

   SYS_SLIST_FOR_EACH_CONTAINER(&trs_cbs, listener, _node) {
      if (listener->rx_write) {
         err = listener->rx_write(conn, (uint8_t *)buf, len, flags);
         /* If we get an error other than ENOTSUP then immediately
            * break the loop and return a generic gatt error, assuming this
            * listener supports this request code, but failed to serve it
            */
         if ((err != 0) && (err != -ENOTSUP)) {
            LOG_ERR("TRS Rx Written Cb Ret %d", err);
            return BT_GATT_ERR(BT_TRS_ATT_ERR_APPLICATION_ERROR);
         }
      }
   }
   if (err) {
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
   } else {
      return len;
   }
}


static ssize_t cp_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
   int err = -ENOTSUP;
   struct bt_trs_cb *listener;

     if ((flags & BT_GATT_WRITE_FLAG_PREPARE)) {
      /* Prepare Write Request received. Reject it since this Characteristic
      * Won't support Prepare Write.
      */
      return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
   }

   SYS_SLIST_FOR_EACH_CONTAINER(&trs_cbs, listener, _node) {
      if (listener->cp_write) {
         err = listener->cp_write(conn, (uint8_t *)buf, len, flags);
         /* If we get an error other than ENOTSUP then immediately
            * break the loop and return a generic gatt error, assuming this
            * listener supports this request code, but failed to serve it
            */
         if ((err != 0) && (err != -ENOTSUP)) {
            LOG_ERR("TRS CP Written Cb Ret %d", err);
            return BT_GATT_ERR(BT_TRS_ATT_ERR_APPLICATION_ERROR);
         }
      }
   }
   if (err) {
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
   } else {
      return len;
   }
}

static ssize_t tx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
   int err = -ENOTSUP;
   struct bt_trs_cb *listener;

   if ((flags & BT_GATT_WRITE_FLAG_PREPARE)) {
      /* Prepare Write Request received. Reject it since this Characteristic
      * Won't support Prepare Write.
      */
      return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
   }

   SYS_SLIST_FOR_EACH_CONTAINER(&trs_cbs, listener, _node) {
      if (listener->cp_write) {
         err = listener->tx_write(conn, (uint8_t *)buf, len, flags);
         /* If we get an error other than ENOTSUP then immediately
            * break the loop and return a generic gatt error, assuming this
            * listener supports this request code, but failed to serve it
            */
         if ((err != 0) && (err != -ENOTSUP)) {
            LOG_ERR("TRS Tx Written Cb Ret %d", err);
            return BT_GATT_ERR(BT_TRS_ATT_ERR_APPLICATION_ERROR);
         }
      }
   }
   if (err) {
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
   } else {
      return len;
   }
}

static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
   ARG_UNUSED(attr);

   struct bt_trs_cb *listener;

   bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);

   LOG_INF("Tx notifications %s", notif_enabled ? "enabled" : "disabled");

   SYS_SLIST_FOR_EACH_CONTAINER(&trs_cbs, listener, _node) {
      if (listener->tx_ntf_changed) {
         listener->tx_ntf_changed(notif_enabled);
      }
   }
}

static void cp_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
   ARG_UNUSED(attr);

   struct bt_trs_cb *listener;

   bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);

   LOG_INF("CP notifications %s", notif_enabled ? "enabled" : "disabled");

   SYS_SLIST_FOR_EACH_CONTAINER(&trs_cbs, listener, _node) {
      if (listener->cp_ntf_changed) {
         listener->cp_ntf_changed(notif_enabled);
      }
   }
}



BT_GATT_SERVICE_DEFINE(trs,
    BT_GATT_PRIMARY_SERVICE(&trs_uuid),
    BT_GATT_CHARACTERISTIC(&tx_char_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                        BT_GATT_PERM_WRITE, NULL, tx_write, NULL),
    BT_GATT_CCC(tx_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&rx_char_uuid.uuid,BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                         BT_GATT_PERM_WRITE, NULL, rx_write, NULL),
    BT_GATT_CHARACTERISTIC(&cp_char_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_WRITE, NULL, cp_write, NULL),
    BT_GATT_CCC(cp_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);


int bt_trs_tx_notify(struct bt_conn *conn, uint8_t* data, uint16_t len)
{
   int rc;

   if (data == NULL) {
      return -EINVAL;
   }

   rc = bt_gatt_notify(conn, &trs.attrs[2], data, len);

   return rc == -ENOTCONN ? 0 : rc;
}
int bt_trs_cp_notify(struct bt_conn *conn, uint8_t* data, uint16_t len)
{
   int rc;

   if (data == NULL) {
      return -EINVAL;
   }

   rc = bt_gatt_notify(conn, &trs.attrs[7], data, len);

   return rc == -ENOTCONN ? 0 : rc;
}

bool bt_trs_tx_ccc_config_status(struct bt_conn *conn)
{
   bool is_subscribed = bt_gatt_is_subscribed(conn, &trs.attrs[2], BT_GATT_CCC_NOTIFY);

   return is_subscribed;
}

bool bt_trs_cp_ccc_config_status(struct bt_conn *conn)
{
   bool is_subscribed = bt_gatt_is_subscribed(conn, &trs.attrs[7], BT_GATT_CCC_NOTIFY);

   return is_subscribed;
}

int bt_trs_cb_register(struct bt_trs_cb *cb)
{
   CHECKIF(cb == NULL) {
      return -EINVAL;
   }

   sys_slist_append(&trs_cbs, &cb->_node);

   return 0;
}

int bt_trs_cb_unregister(struct bt_trs_cb *cb)
{
   CHECKIF(cb == NULL) {
      return -EINVAL;
   }

   if (!sys_slist_find_and_remove(&trs_cbs, &cb->_node)) {
      return -ENOENT;
   }

   return 0;
}

