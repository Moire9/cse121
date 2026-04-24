#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *NAME = "TempLog";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)


#define I2C_SCL_GPIO 8
#define I2C_SDA_GPIO 10
#define I2C_FREQ 300'000 // Hz
#define I2C_TIMEOUT -1 // ms
#define SHTC3_ADDR 0x70

#define GPIO_STOP_BUTTON 9
#define GPIO_STOP_BUTTON_MASK (1ULL << (GPIO_STOP_BUTTON)) // GPIO9 "Boot" user button


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

static bool active = true;

// We can't log inside this function
static void IRAM_ATTR gpio_interrupt(void* /* ignored */) {
	active = false;
	// uint32_t gpio_num = (uint32_t) arg;
	// xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_init() {
	gpio_config_t config = {
		.pin_bit_mask = GPIO_STOP_BUTTON_MASK,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_POSEDGE,
	};

	ESP_ERROR_CHECK(
		gpio_config(&config)
	);
	ESP_ERROR_CHECK(
		gpio_install_isr_service(0 /* no special flags */)
	);
	ESP_ERROR_CHECK(
		gpio_isr_handler_add(GPIO_STOP_BUTTON, gpio_interrupt, (void*) 0 /* ignored */)
	);

	LOG("GPIO init.");
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

void app_main() {
#define ERRCHK(expr) ESP_ERROR_CHECK_WITHOUT_ABORT(expr)
	gpio_init();

	i2c_device_handle device_handle;
	i2c_bus_handle bus_handle;

	i2c_init(&device_handle, &bus_handle);

	while (1) {
		if (!active) {
			LOG("Shutdown!");
			break;
		}

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

		if (crc(meas_out[0], meas_out[1]) != meas_out[2])
			LOG("Temperature FAILED crc");

		if (crc(meas_out[3], meas_out[4]) != meas_out[5])
			LOG(   "Humidity FAILED crc");

		const uint32_t tempraw = meas_out[0] << 8 | meas_out[1];
		const uint32_t  humraw = meas_out[3] << 8 | meas_out[4];

		const  int8_t deg_c = ((tempraw * 175) >> 16) - 45;
		const int32_t deg_f = ((tempraw * 315) >> 16) - 49;
		const uint8_t    rh =  ( humraw * 100) >> 16;

		ESP_LOGI(NAME, "Temperature is %dC (or %dF) with a %d%% humidity", deg_c, deg_f, rh);

		ERRCHK( // SLEEP
			i2c_write(device_handle, sleep_cmd, 2)
		);

		if (!active) {
			LOG("Shutdown!");
			break;
		}

		vTaskDelay(2000 / portTICK_PERIOD_MS);
	}

	i2c_master_bus_rm_device(device_handle);
	i2c_del_master_bus(bus_handle);

	LOG("I2C shutdown. Goodbye.");
#undef ERRCHK
}
