#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_regs.h"

static const char *NAME = "Mouse";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)
#define WAIT(ms) vTaskDelay(ms / portTICK_PERIOD_MS)

#define I2C_SCL_GPIO 8
#define I2C_SDA_GPIO 10
#define I2C_FREQ 400'000 // Hz
#define I2C_TIMEOUT 100 // ms
#define IMU_ADDR 0x68

// Minimum size of typical random error spikes on the imu
#define ERR_SPIKE 400

typedef i2c_master_bus_handle_t i2c_bus_handle;
typedef i2c_master_dev_handle_t i2c_device_handle;
typedef i2c_master_bus_config_t i2c_bus_config;
typedef i2c_device_config_t     i2c_device_config;

static void i2c_init(i2c_bus_handle* bus_handle, i2c_device_handle* device_handle, const uint8_t device_address) {
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
		.device_address  = device_address,
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

// static esp_err_t i2c_read_reg(i2c_device_handle handle, uint8_t* data_ptr, const size_t data_len, const uint8_t* reg_ptr, const size_t reg_len) {
static esp_err_t i2c_read_reg(i2c_device_handle handle, const uint8_t reg, uint8_t* data_ptr, const size_t len) {
	return i2c_master_transmit_receive(handle, &reg, 1, data_ptr, len, I2C_TIMEOUT);
	// return i2c_master_receive(handle, data_ptr, len, I2C_TIMEOUT);
}

static esp_err_t i2c_write(i2c_device_handle handle, const uint8_t* data_ptr, const size_t len) {
	return i2c_master_transmit(handle, data_ptr, len, I2C_TIMEOUT);
}

static uint8_t read_reg(i2c_device_handle handle, const uint8_t reg) {
	uint8_t data = 0;
	ESP_ERROR_CHECK_WITHOUT_ABORT(
		i2c_read_reg(handle, reg, &data, 1)
	);
	return data;
}

static int error = 0;

static int16_t read_reg16(i2c_device_handle handle, const uint8_t reg_lower, const uint8_t reg_upper) {
	uint8_t data[2] = {43, 103}; // 11111
	// WAIT(10);
	if (i2c_read_reg(handle, reg_lower, data, 1) != ESP_OK) error = 1;
	WAIT(10);
	if (i2c_read_reg(handle, reg_upper, data + 1, 1) != ESP_OK) error = 1;
	return (data[1] << 8) | data[0];
}

static esp_err_t write_reg(i2c_device_handle handle, const uint8_t reg, const uint8_t value) {
	const uint8_t packet[] = {reg, value};
	return i2c_write(handle, packet, 2);
}

static void write_reg_chk(i2c_device_handle handle, const uint8_t reg, const uint8_t value) {
	ESP_ERROR_CHECK_WITHOUT_ABORT(
		write_reg(handle, reg, value)
	);
}

// https://d17t6iyxenbwp1.cloudfront.net/s3fs-public/2026-05/ds-000451-icm-42670-p-datasheet.pdf?VersionId=N_6riEzGYuWEHO3quauImb_3ntFIed8G
void app_main() {
initialize_i2c:
	#define WCHK(...) ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(__VA_ARGS__))
	i2c_bus_handle bus_handle;
	i2c_device_handle dev;

	i2c_init(&bus_handle, &dev, IMU_ADDR);

	WAIT(50);

	// Wait for device to fully power on (bit three = ready)
	while (!(read_reg(dev, MCLK_RDY) & 0b0000'1000)) {
		WAIT(1);
	}

	// Soft reset
	WCHK(dev, SIGNAL_PATH_RESET, 0b0001'0000);

	WAIT(10);

	// Set gyro & accel to low noise mode
	WCHK(dev, PWR_MGMT0, 0b0000'1111);

	WAIT(50); // minimum 200us

	// Disable all interrupts
	WCHK(dev, INT_SOURCE0, 0b0000'0000);

	WAIT(10);

	// Disable packet queue
	WCHK(dev, FIFO_CONFIG1, 0b0000'0001);

	WAIT(10);

	// Write to MREG1 to disable APEX
	WCHK(dev, BLK_SEL_W, 0);
	WAIT(10);
	WCHK(dev, MADDR_W, MREG1_SENSOR_CONFIG3);
	WAIT(10);
	WCHK(dev, M_W, 0x40);
	WAIT(10);

	// Wait for data ready signal
	while (!(read_reg(dev, INT_STATUS_DRDY) & 0b0000'0001)) {
		WAIT(1);
	}

	WAIT(100);

	LOG("IMU ready");

	int consecutive_fails = 0;
	int16_t accel_hist_x[16] = {0}, accel_hist_y[16] = {0};
	uint8_t idx = 0;
	uint8_t discrepency_width = 0; // How long we are suddenly very different from average
	while (true) {
		const int16_t accel_x = read_reg16(dev, ACCEL_DATA_X0, ACCEL_DATA_X1);
		const int16_t accel_y = read_reg16(dev, ACCEL_DATA_Y0, ACCEL_DATA_Y1);
		// const int16_t accel_z = read_reg16(dev, ACCEL_DATA_Z0, ACCEL_DATA_Z1);
		// const int16_t  gyro_y = read_reg16(dev,  GYRO_DATA_Y0,  GYRO_DATA_Y1);
		// const int16_t  gyro_z = read_reg16(dev,  GYRO_DATA_Z0,  GYRO_DATA_Z1);
		// const int16_t  gyro_x = read_reg16(dev,  GYRO_DATA_X0,  GYRO_DATA_X1);

		if (error) {
			consecutive_fails++;
			if (consecutive_fails > 32) {
				ESP_LOGE(NAME, "Too many errors. Restarting.");
				i2c_master_bus_rm_device(dev);
				i2c_del_master_bus(bus_handle);
				goto initialize_i2c;
				// esp_restart();
			}
			error = 0;
			continue;
		}

		if (consecutive_fails > 1) {
			ESP_LOGW(NAME, "%d errors.", consecutive_fails);
			consecutive_fails = 0;
		}

		// const int16_t old_acc_x = accel_hist_x[idx];
		// const int16_t old_acc_y = accel_hist_y[idx];

		int32_t accel_x_avg = 0, accel_y_avg = 0;
		for (int i = 0; i < 16; i++) {
			accel_x_avg += accel_hist_x[i];
			accel_y_avg += accel_hist_y[i];
		}
		accel_x_avg >>= 4;
		accel_y_avg >>= 4;

		const int16_t deltax = accel_x_avg - accel_x;
		const int16_t deltay = accel_y_avg - accel_y;

		// If we are significantly different, it might be a momentary error from the IMU
		// So we are having a discrepency
		if (deltax > ERR_SPIKE || deltax < -ERR_SPIKE || deltay > ERR_SPIKE || deltay < -ERR_SPIKE) {
			discrepency_width++;
		} else {
			discrepency_width = 0;
		}

		// If we are having no problems, we should write it in
		// But if we are having a long discrepency, it could just mean that we made a sudden
		// movement and it's not a momentary spike - this is real. So then we write it in.
		if (discrepency_width == 0 || discrepency_width > 8) {
			idx = (idx + 1) & 0xF;

			accel_hist_x[idx] = accel_x;
			accel_hist_y[idx] = accel_y;

			const int16_t x_norm = accel_x_avg + 60;
			const int16_t y_norm = accel_y_avg + 13;

			const int8_t x_motion = x_norm > 600 ? 1 : x_norm < -600 ? -1 : 0;
			const int8_t y_motion = y_norm > 600 ? 1 : y_norm < -600 ? -1 : 0;
			const char *x_dir, *y_dir;
			switch(x_motion) {
			case -1: x_dir = "LEFT" ; break;
			case +1: x_dir = "RIGHT"; break;
			default: x_dir = "";
			}
			switch(y_motion) {
			case -1: y_dir = "DOWN"; break;
			case +1: y_dir = "UP"  ; break;
			default: y_dir = "";
			}
			// LOG("Accel: (%+5d, %+5d) [%+5d, %+5d]", x_norm, y_norm, accel_x, accel_y);
			LOG("%s %s", y_dir, x_dir);
		}
	}
}



