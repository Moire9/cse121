#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *NAME = "UDS";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)

#define I2C_SCL_GPIO 8
#define I2C_SDA_GPIO 10
#define I2C_FREQ 300'000 // Hz
#define I2C_TIMEOUT -1 // ms
#define SHTC3_ADDR 0x70

#define GPIO_BOOT_BUTTON 9 // GPIO9 "Boot" user button
#define GPIO_ECHO 0 // Echo signal from UDS
#define GPIO_TRIG 1 // Trigger signal to UDS
#define GPIO_MASK(num) (1ULL << (num))

#define HIST_LOG2 4
#define HIST_SIZE (1 << HIST_LOG2)

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

static esp_timer_handle_t trig_timer;

static QueueHandle_t echo_queue;

static int64_t pulse_start = 0;

static bool active = true;

// Interrupts have to be super lightweight. Most functions have call stacks too
// deep to be called inside of them
// uint32_t gpio_num = (uint32_t) arg;
// xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
static void IRAM_ATTR int_shutdown(void* /* ignored */) {
	active = false;
}

static void IRAM_ATTR int_echo(void*) {
	if (gpio_get_level(GPIO_ECHO)) { // rising (pulse start)
		pulse_start = esp_timer_get_time();
	} else { // falling (pulse end)
		const int64_t pulse_end = esp_timer_get_time();
		const int64_t delta = pulse_end - pulse_start;
		xQueueSendFromISR(echo_queue, &delta, NULL);
	}
}

static void IRAM_ATTR int_stop_trig(void*) {
	gpio_set_level(GPIO_TRIG, 0);
}

/** Set trigger high, and then start 10us timer to call int_stop_trig to set low. */
static void trigger() {
	// The timer shouldn't be running, but we stop it just in case.
	// No error check because an error is generated if the timer is not running
	esp_timer_stop_blocking(trig_timer, portMAX_DELAY);

	gpio_set_level(GPIO_TRIG, 1);
	ESP_ERROR_CHECK(
		esp_timer_start_once(trig_timer, 10 /* us */)
	);
}

static void timer_init() {
	const esp_timer_create_args_t args = {
		.callback = int_stop_trig,
	};

	ESP_ERROR_CHECK(
		esp_timer_create(&args, &trig_timer)
	);
}

static void gpio_init() {
#define CFG_() ESP_ERROR_CHECK(gpio_config(&config))

	ESP_ERROR_CHECK(
		gpio_install_isr_service(0 /* no special flags */)
	);

	gpio_config_t config = {
		.pin_bit_mask = GPIO_MASK(GPIO_BOOT_BUTTON),
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_POSEDGE,
	};

	CFG_();
	ESP_ERROR_CHECK(
		gpio_isr_handler_add(GPIO_BOOT_BUTTON, int_shutdown, NULL /* user-specified argument we ignore */)
	);

	config.pin_bit_mask = GPIO_MASK(GPIO_ECHO);
	config.pull_up_en   = GPIO_PULLDOWN_DISABLE;
	config.intr_type    = GPIO_INTR_ANYEDGE;

	CFG_();
	ESP_ERROR_CHECK(
		gpio_isr_handler_add(GPIO_ECHO, int_echo, NULL)
	);

	config.pin_bit_mask = GPIO_MASK(GPIO_TRIG);
	config.mode         = GPIO_MODE_OUTPUT;
	config.intr_type    = GPIO_INTR_DISABLE;

	CFG_();

	gpio_set_level(GPIO_TRIG, 0);

	LOG("GPIO init.");
#undef CFG_
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

int8_t get_temperature(i2c_device_handle device_handle) {
#define CHK_(err) do { if ((err) != ESP_OK) {\
	ESP_ERROR_CHECK_WITHOUT_ABORT(err);\
	return -127 /* indicate error with value outside SHTC3 range */;\
} } while (0)

	CHK_( // WAKE
		i2c_write(device_handle, wake_cmd, 2)
	);

	vTaskDelay(20 / portTICK_PERIOD_MS);

	CHK_( // MEASURE
		i2c_write(device_handle, meas_cmd, 2)
	);

	vTaskDelay(20 / portTICK_PERIOD_MS);

	uint8_t meas_out[6];
	CHK_( // READ
		i2c_read(device_handle, meas_out, 6)
	);

	CHK_( // SLEEP
		i2c_write(device_handle, sleep_cmd, 2)
	);

	if (crc(meas_out[0], meas_out[1]) != meas_out[2])
		return -127;

	const uint16_t temp_data = meas_out[0] << 8 | meas_out[1];
	// If this doesn't work, cast raw to u32
	return ((temp_data * 175) >> 16) - 45;
#undef CHK_
}

double calculate_raw_dist(i2c_device_handle device_handle) {
	xQueueReset(echo_queue);
	trigger();

	int64_t delta;
	if (!xQueueReceive(echo_queue, &delta, 15 / portTICK_PERIOD_MS)) {
		// Maximum rated distance is 4m, corresponding to ~12ms, so we generously wait 15 ms
		LOG("Timed out waiting for bounce");
		return NAN;
	}

	const int8_t temp = get_temperature(device_handle); // Celcius
	if (temp == -127) {
		LOG("Failed to get temperature");
		return NAN;
	}

	const double sound_vel = 331 + 0.6 * temp;

	// LOG("dt = %"PRId64" us, T = %"PRId8" C, v = %.1f m/s", delta, temp, sound_vel);
	return delta * (sound_vel / 10000 /* cm/us */);
}

void app_main() {
	i2c_device_handle device_handle;
	i2c_bus_handle bus_handle;

	i2c_init(&device_handle, &bus_handle);

	echo_queue = xQueueCreate(1, sizeof(int64_t));

	timer_init();
	gpio_init();

	double hist[HIST_SIZE] = {0};
	uint8_t idx = 0;
	while (active) {
		const double dist = calculate_raw_dist(device_handle);
		if (dist != dist) continue; // NaN

		hist[idx] = dist;

		idx = (idx + 1) & (HIST_SIZE - 1);

		double d = 0; // average, short name for scaled equation
		for (int i = 0; i < HIST_SIZE; i++)
		    d += hist[i];
		d /= HIST_SIZE;

		const double scaled = // cubic regression I found
			0.000781598 * d*d*d - 0.0531792 * d*d +1.63765 * d - 7.18473;

		LOG("dx = %.2f cm%s", scaled, idx ? "" : " [!]");
	}

	i2c_master_bus_rm_device(device_handle);
	i2c_del_master_bus(bus_handle);
}
