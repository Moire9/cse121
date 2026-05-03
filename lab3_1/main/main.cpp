#include <stdio.h>

#include <esp_err.h>

#include "esp_log.h"
#include "esp_check.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "DFRobot_RGBLCD1602.h"

static const char *NAME = "LCDCTRL";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)

extern "C" {

void app_main() {
	DFRobot_RGBLCD1602 lcd(0x2D /* V2.0 RGB addr*/);
	LOG("Created obj");
	ESP_ERROR_CHECK_WITHOUT_ABORT(lcd.init());
	LOG("Initialized");
	lcd.setRGB(0x10, 0xFF, 0x10);
	LOG("Set RGB");
	lcd.print("Wendt");
	LOG("Print");
	lcd.goodbye();
	LOG("Goodbye");
}

}