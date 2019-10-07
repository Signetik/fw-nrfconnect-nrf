/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <zephyr/types.h>
#include <errno.h>
#include <net/socket.h>
#include <string.h>
#include <stdio.h>
#include <device.h>
#include <lte_lc.h>
#include <at_cmd.h>
#include <at_cmd_parser/at_cmd_parser.h>
#include <at_cmd_parser/at_params.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(lte_lc, CONFIG_LTE_LINK_CONTROL_LOG_LEVEL);

#define LC_MAX_READ_LENGTH		128
#define AT_CMD_SIZE(x)			(sizeof(x) - 1)
#define AT_CEREG_5			"AT+CEREG=5"
#define AT_CEREG_READ			"AT+CEREG?"
#define AT_CEREG_PARAMS_COUNT		10
#define AT_CEREG_ACTIVE_TIME_INDEX	8
#define AT_CEREG_TAU_INDEX		9
#define AT_CEREG_RESPONSE_MAX_LEN	80

/* Lookup table for T3324 timer used for PSM active time. Unit is seconds.
 * Ref: GPRS Timer 2 IE in 3GPP TS 24.008 Table 10.5.163/3GPP TS 24.008.
 */
static const u32_t t3324_lookup[8] = {2, 60, 600, 60, 60, 60, 60, 0};

/* Lookup table for T3412 timer used for periodic TAU. Unit is seconds.
 * Ref: GPRS Timer 3 IE in 3GPP TS 24.008 Table 10.5.163a/3GPP TS 24.008.
 */
static const u32_t t3412_lookup[8] = {600, 3600, 36000, 2, 30, 60,
				      1152000, 0};

#if defined(CONFIG_BSD_LIBRARY_TRACE_ENABLED)
/* Enable modem trace */
static const char mdm_trace[] = "AT%XMODEMTRACE=1,2";
#endif
/* Subscribes to notifications with level 5 */
static const char cereg_5_subscribe[] = AT_CEREG_5;

#if defined(CONFIG_LTE_LOCK_BANDS)
/* Lock LTE bands 3, 4, 13 and 20 (volatile setting) */
static const char lock_bands[] = "AT%XBANDLOCK=2,\""CONFIG_LTE_LOCK_BAND_MASK
				 "\"";
#endif
#if defined(CONFIG_LTE_LOCK_PLMN)
/* Lock PLMN */
static const char lock_plmn[] = "AT+COPS=1,2,\""
				 CONFIG_LTE_LOCK_PLMN_STRING"\"";
#endif
/* Request eDRX settings to be used */
static const char edrx_req[] = "AT+CEDRXS=1,"CONFIG_LTE_EDRX_REQ_ACTT_TYPE
	",\""CONFIG_LTE_EDRX_REQ_VALUE"\"";
/* Request eDRX to be disabled */
static const char edrx_disable[] = "AT+CEDRXS=3";
/* Request modem to go to power saving mode */
static const char psm_req[] = "AT+CPSMS=1,,,\""CONFIG_LTE_PSM_REQ_RPTAU
			      "\",\""CONFIG_LTE_PSM_REQ_RAT"\"";

/* Request PSM to be disabled */
static const char psm_disable[] = "AT+CPSMS=";
/* Set the modem to power off mode */
static const char power_off[] = "AT+CFUN=0";
/* Set the modem to Normal mode */
static const char normal[] = "AT+CFUN=1";
/* Set the modem to Offline mode */
static const char offline[] = "AT+CFUN=4";

#if defined(CONFIG_LTE_NETWORK_MODE_NBIOT)
/* Preferred network mode: Narrowband-IoT */
static const char nw_mode_preferred[] = "AT%XSYSTEMMODE=0,1,0,0";
/* Fallback network mode: LTE-M */
static const char nw_mode_fallback[] = "AT%XSYSTEMMODE=1,0,0,0";
#elif defined(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS)
/* Preferred network mode: Narrowband-IoT and GPS */
static const char nw_mode_preferred[] = "AT%XSYSTEMMODE=0,1,1,0";
/* Fallback network mode: LTE-M and GPS*/
static const char nw_mode_fallback[] = "AT%XSYSTEMMODE=1,0,1,0";
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M)
/* Preferred network mode: LTE-M */
static const char nw_mode_preferred[] = "AT%XSYSTEMMODE=1,0,0,0";
/* Fallback network mode: Narrowband-IoT */
static const char nw_mode_fallback[] = "AT%XSYSTEMMODE=0,1,0,0";
#elif defined(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS)
/* Preferred network mode: LTE-M and GPS*/
static const char nw_mode_preferred[] = "AT%XSYSTEMMODE=1,0,1,0";
/* Fallback network mode: Narrowband-IoT and GPS */
static const char nw_mode_fallback[] = "AT%XSYSTEMMODE=0,1,1,0";
#endif

static struct k_sem link;
static struct at_param_list params;

#if defined(CONFIG_LTE_PDP_CMD) && defined(CONFIG_LTE_PDP_CONTEXT)
static const char cgdcont[] = "AT+CGDCONT="CONFIG_LTE_PDP_CONTEXT;
#endif
#if defined(CONFIG_LTE_PDN_AUTH_CMD) && defined(CONFIG_LTE_PDN_AUTH)
static const char cgauth[] = "AT+CGAUTH="CONFIG_LTE_PDN_AUTH;
#endif
#if defined(CONFIG_LTE_LEGACY_PCO_MODE)
static const char legacy_pco[] = "AT%XEPCO=0";
#endif

void at_handler(char *response)
{
	char  id[16];
	u32_t val;
	size_t len = 16;

	LOG_DBG("recv: %s", log_strdup(response));

	at_parser_params_from_str(response, NULL, &params);
	at_params_string_get(&params, 0, id, &len);

	/* Waiting to receive either a +CEREG: 1 or +CEREG: 5 string from
	 * from the modem which means 'registered, home network' or
	 * 'registered, roaming' respectively.
	 **/

	if ((len > 0) &&
	    (memcmp(id, "+CEREG", 6) == 0)) {
		at_params_int_get(&params, 1, &val);

		if ((val == 1) || (val == 5)) {
			k_sem_give(&link);
		}
	}
}

static int w_lte_lc_init(void)
{
#if defined(CONFIG_LTE_EDRX_REQ)
	/* Request configured eDRX settings to save power */
	if (at_cmd_write(edrx_req, NULL, 0, NULL) != 0) {
		return -EIO;
	}
#endif
#if defined(CONFIG_BSD_LIBRARY_TRACE_ENABLED)
	if (at_cmd_write(mdm_trace, NULL, 0, NULL) != 0) {
		return -EIO;
	}
#endif
	if (at_cmd_write(cereg_5_subscribe, NULL, 0, NULL) != 0) {
		return -EIO;
	}

#if defined(CONFIG_LTE_LOCK_BANDS)
	/* Set LTE band lock (volatile setting).
	 * Has to be done every time before activating the modem.
	 */
	if (at_cmd_write(lock_bands, NULL, 0, NULL) != 0) {
		return -EIO;
	}
#endif
#if defined(CONFIG_LTE_LOCK_PLMN)
	/* Set Operator (volatile setting).
	 * Has to be done every time before activating the modem.
	 */
	if (at_cmd_write(lock_plmn, NULL, 0, NULL) != 0) {
		return -EIO;
	}
#endif
#if defined(CONFIG_LTE_LEGACY_PCO_MODE)
	if (at_cmd_write(legacy_pco, NULL, 0, NULL) != 0) {
		return -EIO;
	}
	LOG_INF("Using legacy LTE PCO mode...");
#endif
#if defined(CONFIG_LTE_PDP_CMD)
	if (at_cmd_write(cgdcont, NULL, 0, NULL) != 0) {
		return -EIO;
	}
	LOG_INF("PDP Context: %s", log_strdup(cgdcont));
#endif
#if defined(CONFIG_LTE_PDN_AUTH_CMD)
	if (at_cmd_write(cgauth, NULL, 0, NULL) != 0) {
		return -EIO;
	}
	LOG_INF("PDN Auth: %s", log_strdup(cgauth));
#endif

	return 0;
}

static int w_lte_lc_connect(void)
{
	int err;
	const char *current_network_mode = nw_mode_preferred;
	bool retry;

	k_sem_init(&link, 0, 1);
	at_cmd_set_notification_handler(at_handler);
	at_params_list_init(&params, 10);

	do {
		retry = false;

		LOG_DBG("Network mode: %s", log_strdup(current_network_mode));

		if (at_cmd_write(current_network_mode, NULL, 0, NULL) != 0) {
			err = -EIO;
			goto exit;
		}

		if (at_cmd_write(normal, NULL, 0, NULL) != 0) {
			err = -EIO;
			goto exit;
		}

		err = k_sem_take(&link, K_SECONDS(CONFIG_LTE_NETWORK_TIMEOUT));
		if (err == -EAGAIN) {
			LOG_INF("Network connection attempt timed out");

			if (IS_ENABLED(CONFIG_LTE_NETWORK_USE_FALLBACK) &&
			    (current_network_mode == nw_mode_preferred)) {
				current_network_mode = nw_mode_fallback;
				retry = true;

				if (at_cmd_write(offline, NULL, 0, NULL) != 0) {
					err = -EIO;
					goto exit;
				}

				LOG_INF("Using fallback network mode");
			} else {
				err = -ETIMEDOUT;
			}
		}
	} while (retry);

exit:
	at_params_list_free(&params);
	at_cmd_set_notification_handler(NULL);

	return err;
}

static int w_lte_lc_init_and_connect(struct device *unused)
{
	int ret;

	ret = w_lte_lc_init();
	if (ret) {
		return ret;
	}

	return w_lte_lc_connect();
}

/* lte lc Init wrapper */
int lte_lc_init(void)
{
	return w_lte_lc_init();
}

/* lte lc Connect wrapper */
int lte_lc_connect(void)
{
	return w_lte_lc_connect();
}

/* lte lc Init and connect wrapper */
int lte_lc_init_and_connect(void)
{
	struct device *x = 0;

	int err = w_lte_lc_init_and_connect(x);

	return err;
}

int lte_lc_offline(void)
{
	if (at_cmd_write(offline, NULL, 0, NULL) != 0) {
		return -EIO;
	}

	return 0;
}

int lte_lc_power_off(void)
{
	if (at_cmd_write(power_off, NULL, 0, NULL) != 0) {
		return -EIO;
	}

	return 0;
}

int lte_lc_normal(void)
{
	if (at_cmd_write(normal, NULL, 0, NULL) != 0) {
		return -EIO;
	}

	return 0;
}

int lte_lc_psm_req(bool enable)
{
	if (at_cmd_write(enable ? psm_req : psm_disable,
			 NULL, 0, NULL) != 0) {
		return -EIO;
	}

	return 0;
}

int lte_lc_psm_get(int *tau, int *active_time)
{
	int err;
	struct at_param_list at_resp_list = {0};
	char buf[AT_CEREG_RESPONSE_MAX_LEN] = {0};
	char timer_str[9] = {0};
	char unit_str[4] = {0};
	size_t timer_str_len = sizeof(timer_str) - 1;
	size_t unit_str_len = sizeof(unit_str) - 1;
	size_t index;
	u32_t timer_unit, timer_value;

	if ((tau == NULL) || (active_time == NULL)) {
		return -EINVAL;
	}

	/* Enable network registration status with PSM information */
	err = at_cmd_write(AT_CEREG_5, NULL, 0, NULL);
	if (err) {
		LOG_ERR("Could not set CEREG, error: %d", err);
		return err;
	}

	/* Read network registration status */
	err = at_cmd_write(AT_CEREG_READ, buf, sizeof(buf), NULL);
	if (err) {
		LOG_ERR("Could not get CEREG response, error: %d", err);
		return err;
	}

	err = at_params_list_init(&at_resp_list, AT_CEREG_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);
		return err;
	}

	err = at_parser_max_params_from_str(buf,
					    NULL,
					    &at_resp_list,
					    AT_CEREG_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not parse AT+CEREG response, error: %d", err);
		goto parse_psm_clean_exit;
	}

	/* Parse periodic TAU string */
	err = at_params_string_get(&at_resp_list,
				   AT_CEREG_TAU_INDEX,
				   timer_str,
				   &timer_str_len);
	if (err) {
		LOG_ERR("Could not get TAU, error: %d", err);
		goto parse_psm_clean_exit;
	}

	memcpy(unit_str, timer_str, unit_str_len);

	index = strtoul(unit_str, NULL, 2);
	if (index > (ARRAY_SIZE(t3412_lookup) - 1)) {
		LOG_ERR("Unable to parse periodic TAU string");
		err = -EINVAL;
		goto parse_psm_clean_exit;
	}

	timer_unit = t3412_lookup[index];
	timer_value = strtoul(timer_str + unit_str_len, NULL, 2);
	*tau = timer_unit ? timer_unit * timer_value : -1;

	/* Parse active time string */
	err = at_params_string_get(&at_resp_list,
				   AT_CEREG_ACTIVE_TIME_INDEX,
				   timer_str,
				   &timer_str_len);
	if (err) {
		LOG_ERR("Could not get TAU, error: %d", err);
		goto parse_psm_clean_exit;
	}

	memcpy(unit_str, timer_str, unit_str_len);

	index = strtoul(unit_str, NULL, 2);
	if (index > (ARRAY_SIZE(t3324_lookup) - 1)) {
		LOG_ERR("Unable to parse active time string");
		err = -EINVAL;
		goto parse_psm_clean_exit;
	}

	timer_unit = t3324_lookup[index];
	timer_value = strtoul(timer_str + unit_str_len, NULL, 2);
	*active_time = timer_unit ? timer_unit * timer_value : -1;

	LOG_DBG("TAU: %d sec, active time: %d sec\n", *tau, *active_time);

parse_psm_clean_exit:
	at_params_list_free(&at_resp_list);

	return err;
}

int lte_lc_edrx_req(bool enable)
{
	if (at_cmd_write(enable ? edrx_req : edrx_disable,
			 NULL, 0, NULL) != 0) {
		return -EIO;
	}

	return 0;
}

#if defined(CONFIG_LTE_AUTO_INIT_AND_CONNECT)
DEVICE_DECLARE(lte_link_control);
DEVICE_AND_API_INIT(lte_link_control, "LTE_LINK_CONTROL",
		    w_lte_lc_init_and_connect, NULL, NULL, APPLICATION,
		    CONFIG_APPLICATION_INIT_PRIORITY, NULL);
#endif /* CONFIG_LTE_AUTO_INIT_AND_CONNECT */