/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      "FartNet"  //"ResWiFi-Devices"
#define EXAMPLE_ESP_WIFI_PASS      "mariokart"//"AtiMyLHaWasq9MK9F2"
#define EXAMPLE_ESP_MAXIMUM_RETRY  5

#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER ""

#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WeatherStation";

#define LOG(...) ESP_LOGI(TAG, __VA_ARGS__)

#define BUFMAX 8192

#define I2C_SCL_GPIO 8
#define I2C_SDA_GPIO 10
#define I2C_FREQ 300'000 // Hz
#define I2C_TIMEOUT -1 // ms
#define SHTC3_ADDR 0x70

#include "esp_check.h"
#include "driver/i2c_master.h"

static int s_retry_num = 0;

typedef i2c_master_bus_handle_t i2c_bus_handle;
typedef i2c_master_dev_handle_t i2c_device_handle;
typedef i2c_master_bus_config_t i2c_bus_config;
typedef i2c_device_config_t     i2c_device_config;

static void i2c_init(i2c_device_handle* device_handle, i2c_bus_handle* bus_handle) {
	const i2c_bus_config busconf = {
		.i2c_port   = I2C_NUM_0, // i2c lib macro
		.scl_io_num = I2C_SCL_GPIO,
		.sda_io_num = I2C_SDA_GPIO,
		.clk_source = I2C_CLK_SRC_DEFAULT, // i2c lib macro
		.glitch_ignore_cnt = 7, // I have no idea what this means
		.flags.enable_internal_pullup = true,
	};

	const i2c_device_config devconf = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address  = SHTC3_ADDR,
		.scl_speed_hz    = I2C_FREQ,
	};

	ESP_ERROR_CHECK(
		i2c_new_master_bus(&busconf, bus_handle)
	);

	ESP_ERROR_CHECK(
		i2c_master_bus_add_device(*bus_handle, &devconf, device_handle)
	);

	LOG("I2C init.");
}

static uint8_t crc(const uint8_t msb, const uint8_t lsb) {
	uint8_t crc = 0xFF;
	crc ^= msb;
	for (int i = 0; i < 8; i++) {
		crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
	}
	crc ^= lsb;
	for (int i = 0; i < 8; i++) {
		crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
	}
	return crc;
}

static esp_err_t i2c_read(i2c_device_handle handle, uint8_t* data_ptr, const size_t len) {
	return i2c_master_receive(handle, data_ptr, len, I2C_TIMEOUT);
}

static esp_err_t i2c_write(i2c_device_handle handle, const uint8_t* data_ptr, const size_t len) {
	return i2c_master_transmit(handle, data_ptr, len, I2C_TIMEOUT);
}

const uint8_t reset_cmd[] = {
	0x80, 0x5D
};
const uint8_t wake_cmd[] = {
	0x35, 0x17
};
const uint8_t meas_cmd[] = {
	0x78, 0x66
};
const uint8_t sleep_cmd[] = {
	0xB0, 0x98
};

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
								int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG,"connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

void wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
														ESP_EVENT_ANY_ID,
														&wifi_event_handler,
														NULL,
														&instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
														IP_EVENT_STA_GOT_IP,
														&wifi_event_handler,
														NULL,
														&instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS,
			/* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
			 * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
			 * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
			 * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
			 */
			.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
			.sae_pwe_h2e = ESP_WIFI_SAE_MODE,
			.sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
			.disable_wpa3_compatible_mode = 0,
#endif
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	ESP_ERROR_CHECK(esp_wifi_start() );

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
	 * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
			WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
			pdFALSE,
			pdFALSE,
			portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
	 * happened. */
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
				 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
				 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}
}

esp_err_t http_event_handler(esp_http_client_event_t *event) {
	if (event->event_id == HTTP_EVENT_ON_DATA) {
		if (!esp_http_client_is_chunked_response(event->client)) {
			if (event->user_data) {
				const int len = event->data_len < BUFMAX ? event->data_len : BUFMAX;
				memcpy(event->user_data, event->data, len);
				((char*) event->user_data)[len - 1] = 0; // null-terminate
			}
		}
	}
	return ESP_OK;
}

void weather_loop(void* /* ignored */) {
#define ERRCHK(expr) ESP_ERROR_CHECK_WITHOUT_ABORT(expr)
	char response_buffer[BUFMAX] = {0};

	i2c_device_handle device_handle;
	i2c_bus_handle bus_handle;

	i2c_init(&device_handle, &bus_handle);

	while (1) {
		ERRCHK( // WAKE
			i2c_write(device_handle, wake_cmd, 2)
		);

		vTaskDelay(20 / portTICK_PERIOD_MS);

		ERRCHK( // MEASURE
			i2c_write(device_handle, meas_cmd, 2)
		);

		vTaskDelay(20 / portTICK_PERIOD_MS);

		uint8_t meas_out[6];
		ERRCHK( // READ
			i2c_read(device_handle, meas_out, 6)
		);

		if (crc(meas_out[0], meas_out[1]) != meas_out[2]) {
			LOG("Temperature FAILED crc");
			goto end;
		}

		const uint32_t tempraw = meas_out[0] << 8 | meas_out[1];
		const  int8_t deg_c = ((tempraw * 175) >> 16) - 45;

		ERRCHK( // SLEEP
			i2c_write(device_handle, sleep_cmd, 2)
		);


		// Post data to server
		xEventGroupWaitBits(
			s_wifi_event_group,
			WIFI_CONNECTED_BIT,
			pdFALSE, // don't change the bits
			pdFALSE, // ignored for single bit check
			60000 / portTICK_PERIOD_MS
		);

		char post_data[5];

		sprintf(post_data, "%d", deg_c);

		LOG("Sending `%s`", post_data);

		const esp_http_client_config_t config = {
			.url = "http://10.42.0.1:1234",
			.method = HTTP_METHOD_POST,
			// .user_data = response_buffer,
			.user_agent = "curl/7.68.0",
			// .event_handler = http_event_handler,
		};

		esp_http_client_handle_t client_inst = esp_http_client_init(&config);
		esp_http_client_set_header(client_inst, "Content-Type", "text/plain");
		esp_http_client_set_post_field(client_inst, post_data, strlen(post_data));

		const esp_err_t err = esp_http_client_perform(client_inst);
		// LOG("Requested.");

		esp_http_client_cleanup(client_inst);

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
			goto end;
		}

		// LOG("%s", response_buffer);

end:
		vTaskDelay(5000 / portTICK_PERIOD_MS);
	}
#undef ERRCHK
}

void app_main(void)
{
	//Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
	  ESP_ERROR_CHECK(nvs_flash_erase());
	  ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
		/* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
		 * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
		esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
	}

	ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
	wifi_init_sta();

	esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
	esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
	xTaskCreate(&weather_loop, "Weather", 32768, NULL, 5, NULL);
}
