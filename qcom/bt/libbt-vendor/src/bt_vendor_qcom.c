/*
 * Copyright 2012 The Android Open Source Project
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/******************************************************************************
 *
 *  Filename:      bt_vendor_qcom.c
 *
 *  Description:   vendor specific library implementation
 *
 ******************************************************************************/

#define LOG_TAG "bt_vendor"

#include <utils/Log.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <termios.h>
#include "bt_vendor_qcom.h"
#include "hci_uart.h"
#include "hci_smd.h"
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <linux/un.h>
#include "bt_vendor_persist.h"

#define WAIT_TIMEOUT 200000

/******************************************************************************
**  Externs
******************************************************************************/
extern int hw_config(int nState);

extern int is_hw_ready();
extern int rome_soc_init(int fd, char *bdaddr);
/******************************************************************************
**  Variables
******************************************************************************/
int pFd[2] = {0,};
int ant_fd;
bt_vendor_callbacks_t *bt_vendor_cbacks = NULL;
uint8_t vnd_local_bd_addr[6]={0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static int btSocType = BT_SOC_DEFAULT;
static int rfkill_id = -1;
static char *rfkill_state = NULL;


static const tUSERIAL_CFG userial_init_cfg =
{
    (USERIAL_DATABITS_8 | USERIAL_PARITY_NONE | USERIAL_STOPBITS_1),
    USERIAL_BAUD_115200
};

#if (HW_NEED_END_WITH_HCI_RESET == TRUE)
void hw_epilog_process(void);
#endif

#ifdef WIFI_BT_STATUS_SYNC
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include "cutils/properties.h"

static const char WIFI_PROP_NAME[]    = "wlan.driver.status";
static const char SERVICE_PROP_NAME[]    = "bluetooth.hsic_ctrl";
static const char BT_STATUS_NAME[]    = "bluetooth.enabled";
static const char WIFI_SERVICE_PROP[] = "wlan.hsic_ctrl";

#define WIFI_BT_STATUS_LOCK    "/data/connectivity/wifi_bt_lock"
int isInit=0;
#endif /* WIFI_BT_STATUS_SYNC */

/******************************************************************************
**  Local type definitions
******************************************************************************/


/******************************************************************************
**  Functions
******************************************************************************/
#ifdef WIFI_BT_STATUS_SYNC
int bt_semaphore_create(void)
{
    int fd;

    fd = open(WIFI_BT_STATUS_LOCK, O_RDONLY);

    if (fd < 0)
        ALOGE("can't create file\n");

    return fd;
}

int bt_semaphore_get(int fd)
{
    int ret;

    if (fd < 0)
        return -1;

    ret = flock(fd, LOCK_EX);
    if (ret != 0) {
        ALOGE("can't hold lock: %s\n", strerror(errno));
        return -1;
    }

    return ret;
}

int bt_semaphore_release(int fd)
{
    int ret;

    if (fd < 0)
        return -1;

    ret = flock(fd, LOCK_UN);
    if (ret != 0) {
        ALOGE("can't release lock: %s\n", strerror(errno));
        return -1;
    }

    return ret;
}

int bt_semaphore_destroy(int fd)
{
    if (fd < 0)
        return -1;

    return close (fd);
}

int bt_wait_for_service_done(void)
{
    char service_status[PROPERTY_VALUE_MAX];
    int count = 30;

    ALOGE("%s: check\n", __func__);

    /* wait for service done */
    while (count-- > 0) {
        property_get(WIFI_SERVICE_PROP, service_status, NULL);

        if (strcmp(service_status, "") != 0) {
            usleep(200000);
        }
	else {
            break;
	}
    }

    return 0;
}

#endif /* WIFI_BT_STATUS_SYNC */

/** Get Bluetooth SoC type from system setting */
static int get_bt_soc_type()
{
    int ret = 0;
    char bt_soc_type[PROPERTY_VALUE_MAX];

    ALOGI("bt-vendor : get_bt_soc_type");

    ret = property_get("qcom.bluetooth.soc", bt_soc_type, NULL);
    if (ret != 0) {
        ALOGI("qcom.bluetooth.soc set to %s\n", bt_soc_type);
        if (!strncasecmp(bt_soc_type, "rome", sizeof("rome"))) {
            return BT_SOC_ROME;
        }
        else if (!strncasecmp(bt_soc_type, "ath3k", sizeof("ath3k"))) {
            return BT_SOC_AR3K;
        }
        else {
            ALOGI("qcom.bluetooth.soc not set, so using default.\n");
            return BT_SOC_DEFAULT;
        }
    }
    else {
        ALOGE("%s: Failed to get soc type", __FUNCTION__);
        ret = BT_SOC_DEFAULT;
    }

    return ret;
}

bool can_perform_action(char action) {
    bool can_perform = false;
    char ref_count[PROPERTY_VALUE_MAX];
    int value, ret;

    property_get("wc_transport.ref_count", ref_count, "0");

    value = atoi(ref_count);
    ALOGV("%s: ref_count: %s\n",__func__,  ref_count);

    if(action == '1') {
        ALOGV("%s: on : value is: %d", __func__, value);
        value++;
        if (value == 1)
           can_perform = true;
        else if (value > 2) return false;
    } else  {
        ALOGV("%s: off : value is: %d", __func__, value);
        value--;
        if (value == 0)
           can_perform = true;
        else if (value < 0) return false;
    }

    snprintf(ref_count, 3, "%d", value);
    ALOGV("%s: updated ref_count is: %s", __func__, ref_count);

    ret  = property_set("wc_transport.ref_count", ref_count);
    if (ret < 0) {
        ALOGE("%s: Error while updating property: %d\n", __func__, ret);
        return false;
    }
    ALOGV("%s returning %d", __func__, can_perform);
    return can_perform;
}

void stop_hci_filter() {
       char value[PROPERTY_VALUE_MAX] = {'\0'};
       ALOGV("%s: Entry ", __func__);

       property_get("wc_transport.start_hci", value, "false");

       if (strcmp(value, "false") == 0) {
           ALOGV("%s: hci_filter has been stopped already", __func__);
           return;
       }

       property_set("wc_transport.start_hci", "false");
       property_set("wc_transport.hci_filter_status", "0");
       ALOGV("%s: Exit ", __func__);
}

void start_hci_filter() {
       ALOGV("%s: Entry ", __func__);
       int i, init_success = 0;
       char value[PROPERTY_VALUE_MAX] = {'\0'};


       property_get("wc_transport.start_hci", value, false);

       if (strcmp(value, "true") == 0) {
           ALOGV("%s: hci_filter has been started already", __func__);
           return;
       }

       property_set("wc_transport.hci_filter_status", "0");

       property_set("wc_transport.start_hci", "true");
       //sched_yield();
       for(i=0; i<45; i++) {
          property_get("wc_transport.hci_filter_status", value, "0");
          if (strcmp(value, "1") == 0) {
               init_success = 1;
               break;
           } else {
               usleep(WAIT_TIMEOUT);
           }
        }
        ALOGV("start_hcifilter status:%d after %f seconds \n", init_success, 0.2*i);

        ALOGV("%s: Exit ", __func__);
}

/** Bluetooth Controller power up or shutdown */
static int bt_powerup(int en )
{
    char rfkill_type[64];
    char type[16];
    int fd, size, i, ret;

    char disable[PROPERTY_VALUE_MAX];
    char state;
    char on = (en)?'1':'0';

#ifdef WIFI_BT_STATUS_SYNC
    char wifi_status[PROPERTY_VALUE_MAX];
    int lock_fd;
#endif /*WIFI_BT_STATUS_SYNC*/

    ALOGI("bt_powerup: %c", on);

    /* Check if rfkill has been disabled */
    ret = property_get("ro.rfkilldisabled", disable, "0");
    if (!ret ){
        ALOGE("Couldn't get ro.rfkilldisabled (%d)", ret);
        return -1;
    }
    /* In case rfkill disabled, then no control power*/
    if (strcmp(disable, "1") == 0) {
        ALOGI("ro.rfkilldisabled : %s", disable);
        return -1;
    }

#ifdef WIFI_BT_STATUS_SYNC
    lock_fd = bt_semaphore_create();
    bt_semaphore_get(lock_fd);
    bt_wait_for_service_done();
#endif

    /* Assign rfkill_id and find bluetooth rfkill state path*/
    for(i=0;(rfkill_id == -1) && (rfkill_state == NULL);i++)
    {
        snprintf(rfkill_type, sizeof(rfkill_type), "/sys/class/rfkill/rfkill%d/type", i);
        if ((fd = open(rfkill_type, O_RDONLY)) < 0)
        {
            ALOGE("open(%s) failed: %s (%d)\n", rfkill_type, strerror(errno), errno);

#ifdef WIFI_BT_STATUS_SYNC
            bt_semaphore_release(lock_fd);
            bt_semaphore_destroy(lock_fd);
#endif
            return -1;
        }

        size = read(fd, &type, sizeof(type));
        close(fd);

        if ((size >= 9) && !memcmp(type, "bluetooth", 9))
        {
            asprintf(&rfkill_state, "/sys/class/rfkill/rfkill%d/state", rfkill_id = i);
            break;
        }
    }

    /* Get rfkill State to control */
    if ((fd = open(rfkill_state, O_RDWR)) < 0)
    {
        ALOGE("open(%s) for write failed: %s (%d)",rfkill_state, strerror(errno), errno);
#ifdef WIFI_BT_STATUS_SYNC
        bt_semaphore_release(lock_fd);
        bt_semaphore_destroy(lock_fd);
#endif

        return -1;
    }

    if(can_perform_action(on) == false) {
        ALOGE("%s:can't perform action as it is being used by other clients", __func__);
#ifdef WIFI_BT_STATUS_SYNC
        bt_semaphore_release(lock_fd);
        bt_semaphore_destroy(lock_fd);
#endif
        goto done;
    }

    ALOGE("Write %c to rfkill\n", on);

    /* Write value to control rfkill */
    if ((size = write(fd, &on, 1)) < 0) {
        ALOGE("write(%s) failed: %s (%d)",rfkill_state, strerror(errno),errno);
#ifdef WIFI_BT_STATUS_SYNC
        bt_semaphore_release(lock_fd);
        bt_semaphore_destroy(lock_fd);
#endif
	return -1;
    }

    if(on == '0'){
        ALOGE("Stopping HCI filter as part of CTRL:OFF");
        stop_hci_filter();
        property_set("wc_transport.soc_initialized", "0");
    }

#ifdef WIFI_BT_STATUS_SYNC
    /* query wifi status */
    property_get(WIFI_PROP_NAME, wifi_status, "");

    ALOGE("bt get wifi status: %s, isInit: %d\n",  wifi_status, isInit);

    /* If wlan driver is not loaded, and bt is changed from off => on */
    if (strncmp(wifi_status, "unloaded", strlen("unloaded")) == 0 || strlen(wifi_status) == 0) {
        if (on == '1') {
            ALOGI("%s: BT_VND_PWR_ON\n", __func__);
            if(property_set(SERVICE_PROP_NAME, "load_wlan") < 0) {
                ALOGE("%s Property setting failed", SERVICE_PROP_NAME);
                close(fd);
                bt_semaphore_release(lock_fd);
                bt_semaphore_destroy(lock_fd);
                return -1;
            }
        }
        else if (isInit == 0 && on == '0') {
            ALOGI("%s: BT_VND_PWR_OFF\n", __func__);
            if(property_set(SERVICE_PROP_NAME, "unbind_hsic") < 0) {
                ALOGE("%s Property setting failed", SERVICE_PROP_NAME);
                close(fd);
                bt_semaphore_release(lock_fd);
                bt_semaphore_destroy(lock_fd);
                return -1;
            }
       }
    }

    if (isInit == 0 && on == '0')
        property_set(BT_STATUS_NAME, "false");
    else if (on == '1')
        property_set(BT_STATUS_NAME, "true");

    bt_semaphore_release(lock_fd);
    bt_semaphore_destroy(lock_fd);
#endif /* WIFI_BT_STATUS_SYNC */

done:
    if (fd >= 0)
        close(fd);

    return 0;
}

/*****************************************************************************
**
**   BLUETOOTH VENDOR INTERFACE LIBRARY FUNCTIONS
**
*****************************************************************************/

static int init(const bt_vendor_callbacks_t* p_cb, unsigned char *local_bdaddr)
{
    int i;

    ALOGI("bt-vendor : init");

    if (p_cb == NULL)
    {
        ALOGE("init failed with no user callbacks!");
        return -1;
    }

    if ((btSocType = get_bt_soc_type()) < 0) {
        ALOGE("%s: Failed to detect BT SOC Type", __FUNCTION__);
        return -1;
    }

    switch(btSocType)
    {
        case BT_SOC_ROME:
        case BT_SOC_AR3K:
            ALOGI("bt-vendor : Initializing UART transport layer");
            userial_vendor_init();
            break;
        case BT_SOC_DEFAULT:
            break;
        default:
            ALOGE("Unknown btSocType: 0x%x", btSocType);
            break;
    }

    /* store reference to user callbacks */
    bt_vendor_cbacks = (bt_vendor_callbacks_t *) p_cb;

    /* Copy BD Address as little-endian byte order */
    if(local_bdaddr)
        for(i=0;i<6;i++)
            vnd_local_bd_addr[i] = *(local_bdaddr + (5-i));

    ALOGI("%s: Local BD Address : %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", __FUNCTION__,
                                                vnd_local_bd_addr[0],
                                                vnd_local_bd_addr[1],
                                                vnd_local_bd_addr[2],
                                                vnd_local_bd_addr[3],
                                                vnd_local_bd_addr[4],
                                                vnd_local_bd_addr[5]);

#ifdef WIFI_BT_STATUS_SYNC
    isInit = 1;
#endif /* WIFI_BT_STATUS_SYNC */

    return 0;
}



int connect_to_local_socket(char* name) {
       socklen_t len; int sk = -1;

       ALOGE("%s: ACCEPT ", __func__);
       sk  = socket(AF_LOCAL, SOCK_STREAM, 0);
       if (sk < 0) {
           ALOGE("Socket creation failure");
           return -1;
       }

        if(socket_local_client_connect(sk, name,
            ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM) < 0)
        {
             ALOGE("failed to connect (%s)", strerror(errno));
             close(sk);
             sk = -1;
        } else {
                ALOGE("%s: Connection succeeded\n", __func__);
        }
        return sk;
}

bool is_soc_initialized() {
    bool init = false;
    char init_value[PROPERTY_VALUE_MAX];
    int ret;

    ALOGI("bt-vendor : is_soc_initialized");

    ret = property_get("wc_transport.soc_initialized", init_value, NULL);
    if (ret != 0) {
        ALOGI("wc_transport.soc_initialized set to %s\n", init_value);
        if (!strncasecmp(init_value, "1", sizeof("1"))) {
            init = true;
        }
    }
    else {
        ALOGE("%s: Failed to get wc_transport.soc_initialized", __FUNCTION__);
    }

    return init;
}


/** Requested operations */
static int op(bt_vendor_opcode_t opcode, void *param)
{
    int retval = 0;
    int nCnt = 0;
    int nState = -1;
    bool is_ant_req = false;

    ALOGV("bt-vendor : op for %d", opcode);

    switch(opcode)
    {
        case BT_VND_OP_POWER_CTRL:
            {
                nState = *(int *) param;
                ALOGI("bt-vendor : BT_VND_OP_POWER_CTRL: %s",
                        (nState == BT_VND_PWR_ON)? "On" : "Off" );

                switch(btSocType)
                {
                    case BT_SOC_DEFAULT:
                        retval = hw_config(nState);
                        if(nState == BT_VND_PWR_ON
                           && retval == 0
                           && is_hw_ready() == TRUE){
                            retval = 0;
                        }
                        else {
                            retval = -1;
                        }
                        break;
                    case BT_SOC_ROME:
                    case BT_SOC_AR3K:
                        /* BT Chipset Power Control through Device Tree Node */
                        retval = bt_powerup(nState);
                    default:
                        break;
                }
            }
            break;

        case BT_VND_OP_FW_CFG:
            {
                // call hciattach to initalize the stack
                if(bt_vendor_cbacks){
                   ALOGI("Bluetooth Firmware and transport layer are initialized");
                   bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
                }
                else{
                    ALOGE("Error : bt_vendor_cback is NULL,callback not possible");
                    ALOGE("Error : hci, smd initialization Error");
                    retval = -1;
                }
            }
            break;

        case BT_VND_OP_SCO_CFG:
            {
                bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS); //dummy
            }
            break;

        case BT_VND_OP_ANT_USERIAL_OPEN:
                ALOGI("bt-vendor : BT_VND_OP_ANT_USERIAL_OPEN");
                is_ant_req = true;
                //fall through
        case BT_VND_OP_USERIAL_OPEN:
            {
                int (*fd_array)[] = (int (*)[]) param;
                int idx, fd;
                ALOGI("bt-vendor : BT_VND_OP_USERIAL_OPEN");
                switch(btSocType)
                {
                    case BT_SOC_DEFAULT:
                        {
                            if(bt_hci_init_transport(pFd) != -1){
                                int (*fd_array)[] = (int (*) []) param;

                                    (*fd_array)[CH_CMD] = pFd[0];
                                    (*fd_array)[CH_EVT] = pFd[0];
                                    (*fd_array)[CH_ACL_OUT] = pFd[1];
                                    (*fd_array)[CH_ACL_IN] = pFd[1];
                            }
                            else {
                                retval = -1;
                                break;
                            }
                            retval = 2;
                        }
                        break;
                    case BT_SOC_AR3K:
                        {
                            fd = userial_vendor_open((tUSERIAL_CFG *) &userial_init_cfg);
                            if (fd != -1) {
                                for (idx=0; idx < CH_MAX; idx++)
                                    (*fd_array)[idx] = fd;
                                     retval = 1;
                            }
                            else {
                                retval = -1;
                                break;
                            }

                            /* Vendor Specific Process should happened during userial_open process
                                After userial_open, rx read thread is running immediately,
                                so it will affect VS event read process.
                            */
                            if(ath3k_init(fd,3000000,115200,NULL,&vnd_userial.termios)<0)
                                retval = -1;
                        }
                        break;
                    case BT_SOC_ROME:
                        {
                            if (!is_soc_initialized()) {
                                fd = userial_vendor_open((tUSERIAL_CFG *) &userial_init_cfg);
                                if (fd < 0) {
                                    ALOGE("userial_vendor_open returns err");
                                    retval = -1;
                                } else {
                                    ALOGV("rome_soc_init is started");
                                    property_set("wc_transport.soc_initialized", "0");
                                    /* Always read BD address from NV file */
                                    if(!bt_vendor_nv_read(1, vnd_local_bd_addr))
                                    {
                                       /* Since the BD address is configured in boot time We should not be here */
                                       ALOGI("Failed to read BD address. Use the one from bluedroid stack/ftm");
                                    }
                                    if(rome_soc_init(fd,vnd_local_bd_addr)<0) {
                                        retval = -1;
                                    } else {
                                        ALOGV("rome_soc_init is completed");
                                        property_set("wc_transport.soc_initialized", "1");
                                        /*Close the UART port*/
                                        close(fd);
                                    }
                                }
                            }

                            if (retval != -1) {
                                 start_hci_filter();
                                 if (is_ant_req) {
                                     ALOGV("connect to ant channel");
                                     ant_fd = fd = connect_to_local_socket("ant_sock");
                                 } else {
                                     ALOGV("connect to bt channel");
                                     vnd_userial.fd = fd = connect_to_local_socket("bt_sock");
                                 }

                                 if (fd != -1) {
                                     ALOGV("%s: received the socket fd: %d is_ant_req: %d\n",
                                                                 __func__, fd, is_ant_req);
                                     for (idx=0; idx < CH_MAX; idx++)
                                          (*fd_array)[idx] = fd;
                                          retval = 1;
                                     }
                                 else {
                                     retval = -1;
                                 }
                             }
                        }
                        break;
                    default:
                        ALOGE("Unknown btSocType: 0x%x", btSocType);
                        break;
                }
            }
            break;

        case BT_VND_OP_ANT_USERIAL_CLOSE:
            {
                ALOGI("bt-vendor : BT_VND_OP_ANT_USERIAL_CLOSE");
                if (ant_fd != -1) {
                    ALOGE("closing ant_fd");
                    close(ant_fd);
                    ant_fd = -1;
                }
            }
            break;
        case BT_VND_OP_USERIAL_CLOSE:
            {
                ALOGI("bt-vendor : BT_VND_OP_USERIAL_CLOSE btSocType: %d", btSocType);
                switch(btSocType)
                {
                    case BT_SOC_DEFAULT:
                         bt_hci_deinit_transport(pFd);
                         break;

                     case BT_SOC_ROME:
                     case BT_SOC_AR3K:
                        userial_vendor_close();
                        break;
                    default:
                        ALOGE("Unknown btSocType: 0x%x", btSocType);
                        break;
                }
            }
            break;

        case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
            if (btSocType ==  BT_SOC_AR3K) {
                uint32_t *timeout_ms = (uint32_t *) param;
                *timeout_ms = 1000;
            }
            break;

        case BT_VND_OP_LPM_SET_MODE:
            if(btSocType ==  BT_SOC_AR3K) {
                uint8_t *mode = (uint8_t *) param;

                if (*mode) {
                    lpm_set_ar3k(UPIO_LPM_MODE, UPIO_ASSERT, 0);
                }
                else {
                    lpm_set_ar3k(UPIO_LPM_MODE, UPIO_DEASSERT, 0);
                }

                bt_vendor_cbacks->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
            }
            else {
                bt_vendor_cbacks->lpm_cb(BT_VND_OP_RESULT_SUCCESS); //dummy
            }
            break;

        case BT_VND_OP_LPM_WAKE_SET_STATE:
            {
                switch(btSocType)
                {
                    case BT_SOC_ROME:
                        {
                            uint8_t *state = (uint8_t *) param;
                            uint8_t wake_assert = (*state == BT_VND_LPM_WAKE_ASSERT) ? \
                                BT_VND_LPM_WAKE_ASSERT : BT_VND_LPM_WAKE_DEASSERT;

                            if (wake_assert == 0)
                                ALOGI("ASSERT: Waking up BT-Device");
                            else if (wake_assert == 1)
                                ALOGI("DEASSERT: Allowing BT-Device to Sleep");

#ifdef QCOM_BT_SIBS_ENABLE
                            if(bt_vendor_cbacks){
                                ALOGI("Invoking HCI H4 callback function");
                               bt_vendor_cbacks->lpm_set_state_cb(wake_assert);
                            }
#endif
                        }
                        break;
                    case BT_SOC_AR3K:
                        {
                            uint8_t *state = (uint8_t *) param;
                            uint8_t wake_assert = (*state == BT_VND_LPM_WAKE_ASSERT) ? \
                                                        UPIO_ASSERT : UPIO_DEASSERT;
                            lpm_set_ar3k(UPIO_BT_WAKE, wake_assert, 0);
                        }
                    case BT_SOC_DEFAULT:
                        break;
                    default:
                        ALOGE("Unknown btSocType: 0x%x", btSocType);
                        break;
                    }
            }
            break;
        case BT_VND_OP_EPILOG:
            {
#if (HW_NEED_END_WITH_HCI_RESET == FALSE)
                if (bt_vendor_cbacks)
                {
                    bt_vendor_cbacks->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
                }
#else
                hw_epilog_process();
#endif
            }
            break;
    }

    return retval;
}

static void ssr_cleanup(void) {
    int pwr_state=BT_VND_PWR_OFF;

    ALOGI("ssr_cleanup");

    if ((btSocType = get_bt_soc_type()) < 0) {
        ALOGE("%s: Failed to detect BT SOC Type", __FUNCTION__);
        return -1;
    }

    if (btSocType == BT_SOC_ROME) {

        /*Close both ANT and BT channels*/
        op(BT_VND_OP_ANT_USERIAL_CLOSE, NULL);
        op(BT_VND_OP_USERIAL_CLOSE, NULL);
        /*CTRL OFF twice to make sure hw
         * turns off*/
        op(BT_VND_OP_POWER_CTRL, &pwr_state);

    }

    /*Generally switching of chip should be enough*/
    op(BT_VND_OP_POWER_CTRL, &pwr_state);

    bt_vendor_cbacks = NULL;
}


/** Closes the interface */
static void cleanup( void )
{
    ALOGI("cleanup");
    bt_vendor_cbacks = NULL;

#ifdef WIFI_BT_STATUS_SYNC
    isInit = 0;
#endif /* WIFI_BT_STATUS_SYNC */
}

// Entry point of DLib
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    init,
    op,
    cleanup,
    ssr_cleanup
};
