
#include <stdio.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_spiffs.h>
#include <esp_http_server.h>
#include <driver/gpio.h>

#include "settings.h"
#include "event.h"
#include "log.h"

/**
 * Transfer buffer size for files
 */
#define TXBUFSZ (CONFIG_HTTPD_TXBUF_SIZE * 1024)

static const char *TAG       = "http";
static httpd_handle_t server = NULL;
static httpd_config_t config = HTTPD_DEFAULT_CONFIG();

static esp_vfs_spiffs_conf_t fs_conf = {
	.base_path       = "/www",
	.max_files       = 4,
	.partition_label = "www",
	.format_if_mount_failed = false
};

/**
 * HEAD /on
 */
static esp_err_t on(httpd_req_t *req)
{
	settings_lock();
	if (!settings.lights_status) {
		esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, ON,
		                  NULL, 0, 10);
	}
	settings_unlock();

	httpd_resp_set_status(req, HTTPD_200);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/**
 * HEAD /off
 */
static esp_err_t off(httpd_req_t *req)
{
	settings_lock();
	if (settings.lights_status) {
		esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, OFF,
		                  NULL, 0, 10);
	}
	settings_unlock();

	httpd_resp_set_status(req, HTTPD_200);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/**
 * HEAD /status
 *
 * Lights status / Manual override status / Schedule status /
 * Start time / Stop time
 */
static esp_err_t status(httpd_req_t *req)
{
	char buf[64];
	const char *override = "auto";

	settings_lock();
	if (settings.override_sw & 2)      override = "off";
	else if (settings.override_sw & 1) override = "on";
	sprintf(buf, "299 %s/%s/%s/%02u:%02u/%02u:%02u",
	        override,
	        settings.light_sw    ? "on" : "off",
	        settings.sched_sw    ? "on" : "off",
	        settings.shr, settings.smn,
	        settings.ehr, settings.emn);
	settings_unlock();

	httpd_resp_set_status(req, buf);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/**
 * HEAD /schedule/on?on=xx:xx&off=xx:xx
 *
 * The on/off times are expected to be UTC.
 */
static esp_err_t schedule_on(httpd_req_t *req)
{
	char qstr[20]; char on[6], off[6];
	unsigned int shr, smn, ehr, emn;

	/* XXX: Yes, all three of these work by way of strlcpy(). */
	if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) != ESP_OK ||
	    httpd_query_key_value(qstr, "on", on, sizeof(on))    != ESP_OK ||
	    httpd_query_key_value(qstr, "off", off, sizeof(off)) != ESP_OK)
		goto bad_request;

	/* Verify our input parameters */
	if (sscanf(on,  "%u:%u", &shr, &smn) != 2 ||
	    sscanf(off, "%u:%u", &ehr, &emn) != 2 ||
	    (shr == ehr && smn == emn) ||
	    shr > 23 || smn > 59 || ehr > 23 || emn > 59)
		goto bad_request;

	settings_lock();
	settings.sched_sw = 1;
	settings.shr      = shr;
	settings.smn      = smn;
	settings.ehr      = ehr;
	settings.emn      = emn;
	settings_unlock();

	esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, SCHED_ON,
	                  NULL, 0, 10);
	httpd_resp_set_status(req, HTTPD_200);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;

bad_request:
	httpd_resp_set_status(req, HTTPD_400);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_FAIL;
}

/**
 * HEAD /schedule/off
 */
static esp_err_t schedule_off(httpd_req_t *req)
{
	esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, SCHED_OFF,
	                  NULL, 0, 10);
	httpd_resp_set_status(req, HTTPD_200);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

/**
 * GET /...
 */
static esp_err_t idx(httpd_req_t *req)
{
	size_t n;
	char *buf = NULL;
	FILE *fp = NULL;
	const char *fn = strrchr(req->uri, '/');

	if (!fn || strlen(fn) > 20) goto not_found;
	else ++fn;

	/* Set the MIME type and cache-control header */
	switch (req->uri[strlen(req->uri) - 2]) {
	case 's': /* .css */
		httpd_resp_set_type(req, "text/css");
		httpd_resp_set_hdr(req, "Cache-Control",
		                   "public, max-age=31536000");
		break;
	case 'j': /* .js */
		httpd_resp_set_type(req, "application/javascript");
		httpd_resp_set_hdr(req, "Cache-Control",
		                   "public, max-age=31536000");
		break;
	default: /* index.html */
		httpd_resp_set_type(req, "text/html");
		fn = "index.html";
	}

	/* Buffer the file out */
	if (!(buf = malloc(TXBUFSZ)))
		goto internal_error;

	sprintf(buf, "/www/%s.gz", fn);
	if (!(fp = fopen(buf, "r")))
		goto not_found;

	httpd_resp_set_status(req, HTTPD_200);
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	while (!feof(fp) && !ferror(fp) && (n = fread(buf, 1, TXBUFSZ, fp))) {
		if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK)
			goto tx_error;
	}

	if (ferror(fp))
		goto tx_error;

tx_error:
	free(buf);
	fclose(fp);
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;

not_found:
	if (buf) free(buf);
	httpd_resp_set_status(req, HTTPD_404);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_FAIL;

internal_error:
	httpd_resp_set_status(req, HTTPD_500);
	httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
	httpd_resp_send(req, NULL, 0);
	return ESP_FAIL;
}

static httpd_uri_t on_uri = {
	.uri      = "/on",
	.method   = HTTP_HEAD,
	.handler  = on,
	.user_ctx = NULL
};

static httpd_uri_t off_uri = {
	.uri      = "/off",
	.method   = HTTP_HEAD,
	.handler  = off,
	.user_ctx = NULL
};

static httpd_uri_t status_uri = {
	.uri      = "/status",
	.method   = HTTP_HEAD,
	.handler  = status,
	.user_ctx = NULL
};

static httpd_uri_t schedule_on_uri = {
	.uri      = "/schedule/on",
	.method   = HTTP_HEAD,
	.handler  = schedule_on,
	.user_ctx = NULL
};

static httpd_uri_t schedule_off_uri = {
	.uri      = "/schedule/off",
	.method   = HTTP_HEAD,
	.handler  = schedule_off,
	.user_ctx = NULL
};

static httpd_uri_t index_uri = {
	.uri      = "/*",
	.method   = HTTP_GET,
	.handler  = idx,
	.user_ctx = NULL
};

void http_start(void)
{
	if (server) return;

	ESP_ERROR_CHECK(esp_vfs_spiffs_register(&fs_conf));
	config.uri_match_fn = httpd_uri_match_wildcard;
	if (httpd_start(&server, &config) != ESP_OK) {
		server = NULL;
		err("failed to start");
		return;
	}

	info("registering uri handlers");
	httpd_register_uri_handler(server, &on_uri);
	httpd_register_uri_handler(server, &off_uri);
	httpd_register_uri_handler(server, &status_uri);
	httpd_register_uri_handler(server, &schedule_on_uri);
	httpd_register_uri_handler(server, &schedule_off_uri);
	httpd_register_uri_handler(server, &index_uri);
	info("done");
}

void http_stop(void)
{
	if (!server) return;

	info("stopping");
	httpd_stop(server);
	esp_vfs_spiffs_unregister(fs_conf.partition_label);
	server = NULL;
}

