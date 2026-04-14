#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_strip.h"
#include "esp_log.h"

#include <stdbool.h>

static bool led_en = 0;

/** Will be populated in main */
static led_strip_handle_t led;

void blink_task(void* /* unused */) {
	while (true) {
		if (led_en) {
			led_strip_set_pixel(led, 0, 0x10, 0x10, 0x10);
			led_strip_refresh(led);
		} else {
			led_strip_clear(led);
		}

		led_en = !led_en;

		// Delay 500ms
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}

void app_main() {
	// Setup LED strip on GPIO 2
	led_strip_config_t config = { .strip_gpio_num = 2, .max_leds = 1 };
	led_strip_rmt_config_t rmt_config = { .resolution_hz = 10'000'000, .flags.with_dma = false };

	const int res = led_strip_new_rmt_device(&config, &rmt_config, &led);

	ESP_ERROR_CHECK(res); // Truthfully I don't know what happens if this fails

	// Create repeating task
	xTaskCreate(blink_task, "blink_task", 2048, NULL, 5, NULL);
}
