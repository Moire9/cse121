#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *NAME = "Mouse";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)

#define I2C_SCL_GPIO 8
#define I2C_SDA_GPIO 10
#define I2C_FREQ 300'000 // Hz
#define I2C_TIMEOUT -1 // ms
#define IMU_ADDR 0x68


typedef i2c_master_bus_handle_t i2c_bus_handle;
typedef i2c_master_dev_handle_t i2c_device_handle;
typedef i2c_master_bus_config_t i2c_bus_config;
typedef i2c_device_config_t     i2c_device_config;

static void i2c_init(i2c_bus_handle* bus_handle, i2c_device_handle* device_handle) {
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

static esp_err_t i2c_read(i2c_device_handle handle, uint8_t* data_ptr, const size_t len) {
	return i2c_master_receive(handle, data_ptr, len, I2C_TIMEOUT);
}

static esp_err_t i2c_write(i2c_device_handle handle, const uint8_t* data_ptr, const size_t len) {
	return i2c_master_transmit(handle, data_ptr, len, I2C_TIMEOUT);
}

// https://d17t6iyxenbwp1.cloudfront.net/s3fs-public/2026-05/ds-000451-icm-42670-p-datasheet.pdf?VersionId=N_6riEzGYuWEHO3quauImb_3ntFIed8G
void app_main() {
	i2c_bus_handle bus_handle;
	i2c_device_handle device_handle;

	i2c_init(&bus_handle, &device_handle);


}
