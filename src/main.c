/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <dk_buttons_and_leds.h>

#include <stdio.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <nrf_modem_at.h>
#include <zephyr/net/socket.h>

#define UDP_IP_HEADER_SIZE 28

static int client_fd;
static struct sockaddr_storage host_addr;
static struct k_work_delayable server_transmission_work;
static struct k_work_delayable lte_set_connection_work;
static struct k_work_delayable psm_negotiation_work;
static struct k_work_delayable rai_req_work;

static volatile enum state_type { 
        LTE_STATE_ON,
        LTE_STATE_OFFLINE,
        LTE_STATE_BUSY 
} LTE_Connection_Current_State;

static volatile enum state_type LTE_Connection_Target_State;
static volatile bool PSM_Enable = true;
static volatile bool RAI_Enable = false;
static volatile bool buttons_prev[4] = {0};

/** Need to enable release 14 RAI feature at offline mode before run this command **/
int rai_req(bool enable)
{ 
        int err;

        enum lte_lc_system_mode mode;

        err = lte_lc_system_mode_get(&mode, NULL);
        if (err) {
                return -EFAULT;
        }

        switch (mode) {
        case LTE_LC_SYSTEM_MODE_LTEM:
        case LTE_LC_SYSTEM_MODE_LTEM_GPS:
        case LTE_LC_SYSTEM_MODE_NBIOT:
        case LTE_LC_SYSTEM_MODE_NBIOT_GPS:
        case LTE_LC_SYSTEM_MODE_LTEM_NBIOT:
        case LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS:
                break;
        default:
                printk("Unknown system mode");
                printk("Cannot request RAI for unknown system mode");
                return -EOPNOTSUPP;
        }

        if (enable) {
                err = nrf_modem_at_printf("AT%%RAI=1");
        } else {
                err = nrf_modem_at_printf("AT%%RAI=0");
        }

        if (err) {
                printk("nrf_modem_at_printf failed, reported error: %d", err);
                return -EFAULT;
        }

        return 0;
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
        uint32_t button = button_state & has_changed;

        if (button & DK_BTN1_MSK) {
                if (LTE_Connection_Current_State == LTE_STATE_ON) {
                        printk("Send UDP package!\n");
                        k_work_reschedule(&server_transmission_work, K_NO_WAIT);
                }
        }

        if (button & DK_BTN2_MSK) {
                if (LTE_Connection_Current_State == LTE_STATE_ON) {
                        LTE_Connection_Target_State = LTE_STATE_OFFLINE;
                        k_work_reschedule(&lte_set_connection_work, K_NO_WAIT);
                }
                else if (LTE_Connection_Current_State == LTE_STATE_OFFLINE) {
                        LTE_Connection_Target_State = LTE_STATE_ON;
                        k_work_reschedule(&lte_set_connection_work, K_NO_WAIT);
                }
        }

#if defined(CONFIG_UDP_PSM_ENABLE)
        if (button & DK_BTN3_MSK) {
                PSM_Enable = !PSM_Enable;
                k_work_reschedule(&psm_negotiation_work, K_NO_WAIT);
        }       
#else
        printk("PSM is not enabled in prj.conf!");
#endif

#if defined(CONFIG_UDP_PSM_ENABLE)
        if (button & DK_BTN4_MSK) {
                RAI_Enable = !RAI_Enable;
                k_work_reschedule(&rai_req_work, K_NO_WAIT);
        }       
#else
        printk("RAI is not enabled in prj.conf!");
#endif
}

static void server_disconnect(void)
{
        (void)close(client_fd);
}

static int server_init(void)
{
        struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

        server4->sin_family = AF_INET;
        server4->sin_port = htons(CONFIG_UDP_SERVER_PORT);

        inet_pton(AF_INET, CONFIG_UDP_SERVER_ADDRESS_STATIC,
                  &server4->sin_addr);

        return 0;
}

static int server_connect(void)
{
        int err;

        client_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client_fd < 0) {
                printk("Failed to create UDP socket: %d\n", errno);
                err = -errno;
                goto error;
        }

        err = connect(client_fd, (struct sockaddr *)&host_addr,
                      sizeof(struct sockaddr_in));
        if (err < 0) {
                printk("Connect failed : %d\n", err);
                goto error;
        }

        return 0;

error:
        server_disconnect();

        return err;
}

#if defined(CONFIG_NRF_MODEM_LIB)
static void lte_handler(const struct lte_lc_evt *const evt)
{
        switch (evt->type) {
        case LTE_LC_EVT_NW_REG_STATUS:
                printk("Network registration status: %d\n",
                       evt->nw_reg_status);

                if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
                    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
                        if (evt->nw_reg_status == 0) {
                                LTE_Connection_Current_State = LTE_STATE_OFFLINE;
                                printk("LTE OFFLINE!\n");
                                break;
                        }
                        break;
                }

                printk("Network registration status: %s\n",
                        evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? 
                                "Connected - home network" : "Connected - roaming\n");
                LTE_Connection_Current_State = LTE_STATE_ON;
                break;
        case LTE_LC_EVT_PSM_UPDATE:
                printk("PSM parameter update: TAU: %d, Active time: %d\n",
                       evt->psm_cfg.tau, evt->psm_cfg.active_time);
                break;
        case LTE_LC_EVT_EDRX_UPDATE:
        {
                char log_buf[60];
                ssize_t len;

                len = snprintf(log_buf, sizeof(log_buf),
                               "eDRX parameter update: eDRX: %f, PTW: %f\n",
                               evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
                if (len > 0) {
                        printk("%s\n", log_buf);
                }
                break;
        }
        case LTE_LC_EVT_RRC_UPDATE:
                printk("RRC mode: %s\n",
                       evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle\n");
                break;
        case LTE_LC_EVT_CELL_UPDATE:
                printk("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
                       evt->cell.id, evt->cell.tac);
                break;
        default:
                break;
        }
}

static int configure_low_power(void)
{
        int err;

#if defined(CONFIG_UDP_PSM_ENABLE)
        /** Power Saving Mode */
        err = lte_lc_psm_req(PSM_Enable);
        if (err) {
                printk("lte_lc_psm_req, error: %d\n", err);
        }
#else
        err = lte_lc_psm_req(false);
        if (err) {
                printk("lte_lc_psm_req, error: %d\n", err);
        }
#endif

#if defined(CONFIG_UDP_EDRX_ENABLE)
        /** enhanced Discontinuous Reception */
        err = lte_lc_edrx_req(true);
        if (err) {
                printk("lte_lc_edrx_req, error: %d\n", err);
        }
#else
        err = lte_lc_edrx_req(false);
        if (err) {
                printk("lte_lc_edrx_req, error: %d\n", err);
        }
#endif

#if defined(CONFIG_UDP_RAI_ENABLE) && defined(CONFIG_BOARD_NRF9160DK_NRF9160_NS)
        /* %REL14FEAT is only supported (and required) on nRF9160 */
        /** Enable release 14 RAI feature **/
        err = nrf_modem_at_printf("AT%%REL14FEAT=0,1,0,0,0");
        if (err) {
                printk("Release 14 RAI feature AT-command failed, err %d", err);
        }
#endif

        return err;
}

static int modem_init(void)
{
        int err;

        err = nrf_modem_lib_init();
        if (err) {
                printk("Failed to init the modem library, error: %d\n", err);
                return err;
        }

        if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
                /* Do nothing, modem is already configured and LTE connected. */
        } else {
                err = lte_lc_init();
                if (err) {
                        printk("Modem initialization failed, error: %d\n", err);
                        return err;
                }
        }

        return 0;
}

static void modem_connect(void)
{
        int err;

        if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
                /* Do nothing, modem is already configured and LTE connected. */
        } else {
                err = lte_lc_connect_async(lte_handler);
                if (err) {
                        printk("Connecting to LTE network failed, error: %d\n", err);
                        return;
                }
        }
}
#endif

static void server_transmission_work_fn(struct k_work *work)
{
        int err;
        char buffer[CONFIG_UDP_DATA_UPLOAD_SIZE_BYTES] = {"\0"};

        server_connect();

        printk("Transmitting UDP/IP payload of %d bytes to the IP address %s, port number %d\n",
               CONFIG_UDP_DATA_UPLOAD_SIZE_BYTES + UDP_IP_HEADER_SIZE,
               CONFIG_UDP_SERVER_ADDRESS_STATIC,
               CONFIG_UDP_SERVER_PORT);

        if (RAI_Enable) {
                printk("Setting socket option to RAI_LAST to send last package!\n");
                err = setsockopt(client_fd, SOL_SOCKET, SO_RAI_LAST, NULL, 0);
                if (err < 0) {
                        printk("Set socket option RAI_LAST failed : %d\n", err);
                }
        }

        err = send(client_fd, buffer, sizeof(buffer), 0);
        if (err < 0) {
                printk("Failed to transmit UDP packet, %d\n", err);
                return;
        }
        if (RAI_Enable) {
                printk("Setting socket option to RAI_NO_DATA!\n");
                err = setsockopt(client_fd, SOL_SOCKET, SO_RAI_NO_DATA, NULL, 0);
                if (err < 0) {
                        printk("Set socket option RAI_NO_DATA failed : %d\n", err);
                }
        }

        server_disconnect();
        k_work_schedule(&server_transmission_work,
                K_SECONDS(CONFIG_UDP_DATA_UPLOAD_FREQUENCY_SECONDS));
}

static void lte_set_connection_work_fn(struct k_work *work)
{
        int err;
        if (LTE_STATE_BUSY == LTE_Connection_Target_State) {
                LTE_Connection_Current_State = LTE_STATE_BUSY;
        } else {
                LTE_Connection_Current_State = LTE_STATE_BUSY;
                if (LTE_STATE_OFFLINE == LTE_Connection_Target_State) {
                        lte_lc_offline();
                } else if (LTE_STATE_ON == LTE_Connection_Target_State) {
                        lte_lc_offline();



#if defined(CONFIG_UDP_RAI_ENABLE)
                        err = rai_req(RAI_Enable);
                        if (err) {
                                printk("rai_req, error: %d\n", err);
                        }
#endif

                        lte_lc_normal();
                }
        }
}

static void psm_negotiation_work_fn(struct k_work *work)
{
        int err;
        printk("PSM mode setting is changed, renegotiate PSM!\n");
        printk("PSM_ENABLE: %d\n", PSM_Enable);
        err = lte_lc_psm_req(PSM_Enable);
        if (err) {
                printk("lte_lc_psm_req, error: %d\n", err);
        }
}

static void rai_req_work_fn(struct k_work *work)
{
        int err;
        printk("RAI setting changed\n");
        err = rai_req(RAI_Enable);
        if (err) {
                printk("rai_req, error: %d\n", err);
        }
}

static void work_init(void)
{
        k_work_init_delayable(&server_transmission_work, server_transmission_work_fn);
        k_work_init_delayable(&lte_set_connection_work, lte_set_connection_work_fn);
        k_work_init_delayable(&psm_negotiation_work, psm_negotiation_work_fn);
        k_work_init_delayable(&rai_req_work, rai_req_work_fn);
}

int main(void)
{
        int err;
        printk("UDP sample has started\n");


        err = dk_buttons_init(button_handler);
        if (err) {
                printk("Failed to init buttons: %d\n", err);
                return 1;
        }

        LTE_Connection_Current_State = LTE_STATE_BUSY;
        LTE_Connection_Target_State = LTE_STATE_ON;

#if defined(CONFIG_NRF_MODEM_LIB)
        /* Initialize the modem before calling configure_low_power(). This is
         * because the enabling of RAI is dependent on the
         * configured network mode which is set during modem initialization.
         */
        err = modem_init();
        if (err) {
                printk("Failed to initialize the modem. Aborting\n");
                return 1;
        }

        err = configure_low_power();
        if (err) {
                printk("Unable to set low power configuration, error: %d\n",
                       err);
        }

        modem_connect();
#endif

        while (LTE_STATE_BUSY == LTE_Connection_Current_State) {
                printk("lte_set_connection BUSY!\n");
                k_sleep(K_SECONDS(3));
        }

        err = server_init();
        if (err) {
                printk("Not able to initialize UDP server connection\n");
                return 1;
        }

        work_init();
        k_work_schedule(&server_transmission_work, K_NO_WAIT);
        return 0;
}