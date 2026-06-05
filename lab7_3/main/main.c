/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "MorseRX";

#define GPIO_INPUT 0
#define GPIO_INPUT_MASK (1ULL << (GPIO_INPUT))

#define GPIO_STOP_BUTTON 9
#define GPIO_STOP_BUTTON_MASK (1ULL << (GPIO_STOP_BUTTON)) // GPIO9 "Boot" user button

#define LOG(...) ESP_LOGI(TAG, __VA_ARGS__)

#define WORDTIME_BUF_SIZE 16
#define WORD_BUF_SIZE 32
#define SYM_BUF_SIZE 16

typedef struct {
	char word[WORD_BUF_SIZE + 1];
	uint64_t time;
} wordtime;

enum symbol { DIT, DAH, GAP };
char decode(const uint16_t);

static int print_word_buf = false;

static void IRAM_ATTR gpio_interrupt(void* /* ignored */) {
	print_word_buf = true;
}

static void gpio_init() {
	gpio_config_t btncfg = {
		.pin_bit_mask = GPIO_STOP_BUTTON_MASK,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_POSEDGE,
	};

	ESP_ERROR_CHECK(
		gpio_config(&btncfg)
	);
	ESP_ERROR_CHECK(
		gpio_install_isr_service(0 /* no special flags */)
	);
	ESP_ERROR_CHECK(
		gpio_isr_handler_add(GPIO_STOP_BUTTON, gpio_interrupt, (void*) 0 /* ignored */)
	);

	// gpio_config_t ipcfg = {
	// 	.pin_bit_mask = GPIO_INPUT_MASK,
	// 	.mode         = GPIO_MODE_INPUT,
	// 	.pull_up_en   = GPIO_PULLUP_DISABLE,
	// 	.pull_down_en = GPIO_PULLUP_ENABLE,
	// 	.intr_type    = GPIO_INTR_DISABLE,
	// };
	// ESP_ERROR_CHECK(
	// 	gpio_config(&ipcfg)
	// );

	LOG("GPIO init.");
}

void app_main() {
	gpio_init();
	
	// -------------ADC1 Init---------------//
	adc_oneshot_unit_handle_t handle;
	const adc_oneshot_unit_init_cfg_t init_config = {
		.unit_id = ADC_UNIT_1,
	};
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &handle));

	//-------------ADC1 Config---------------//
	adc_oneshot_chan_cfg_t chan_config = {
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, ADC_CHANNEL_0, &chan_config));

	uint64_t dit_duration = 100; // us
	uint64_t last_change = esp_timer_get_time();
	int last_state = 0;

	uint64_t word_start_time = esp_timer_get_time();

	char wordbuffer[WORD_BUF_SIZE + 1] = {0};
	int wordidx = 0;

	enum symbol symbuffer[SYM_BUF_SIZE + 1] = {0};
	int symidx = 0;

	wordtime wordtimebuffer[WORDTIME_BUF_SIZE + 1] = {0};
	int wordtimeidx = 0;

	while (1) {
		if (print_word_buf) {
			print_word_buf = false;
			LOG("Received:");
			for (int i = 0; i < wordtimeidx; ++i) {
				LOG("%32s [%d]", wordtimebuffer[i].word, wordtimebuffer[i].time);
			}
			wordtimeidx = 0;
		}

		int adc;
		ESP_ERROR_CHECK(adc_oneshot_read(handle, ADC_CHANNEL_0, &adc));
		const int state = adc > 40;
		// const int state = gpio_get_level(GPIO_INPUT);
		if (last_state != state) {
			const int time = esp_timer_get_time();
			const int duration = time - last_change;
			const int hthirds = dit_duration / 2 * 3;
			last_change = time;

			if (last_state) { // On pulse
				if (duration > dit_duration / 8 && duration <= hthirds) { // Dit
					symbuffer[symidx++] = DIT;
					if (symidx > SYM_BUF_SIZE) {
						LOG("Overflow.");
						symidx = 0;
					}

					dit_duration = (dit_duration + duration) / 2;
				} else if (duration > hthirds) { // Dah
					symbuffer[symidx++] = DAH;
					if (symidx > SYM_BUF_SIZE) {
						LOG("Overflow.");
						symidx = 0;
					}
				}
			} else { // Off pulse
				if (duration > dit_duration / 4 && duration <= dit_duration * 2) { // Inter-symbol space
					symbuffer[symidx++] = GAP;
					if (symidx > SYM_BUF_SIZE) {
						LOG("Overflow.");
						symidx = 0;
					}

					dit_duration = (dit_duration + duration) / 2;
				} else if (duration > dit_duration * 2) {
					// Long space - either inter-character or inter-word
					// Decode the preceding character
					// We will store as a bit string, with first symbol MSB, last LSB
					// Characters are expressed as a sequence of 8 doublets
					// 00 -> (nothing)
					// 01 -> dit
					// 11 -> dah
					uint16_t character = 0;
					enum symbol previous = GAP;

					for (int i = 0; i < symidx; ++i) {
						switch (symbuffer[i]) {
						case DIT:
							if (previous == GAP) {
								character |= 0b01;
								break;
							} // Two consecutive symbols without a gap become a dah
							__attribute__ ((fallthrough));
						case DAH:
							character |= 0b11;
							break;
						case GAP:
							if (previous != GAP)
								character <<= 2;
							break;
						}
						previous = symbuffer[i];
					}
					symidx = 0;
					if (previous == GAP)
						character >>= 2;

					const char decoded = decode(character);
					if (decoded == ' ') {
						ESP_LOGW(TAG, "Unable to decode %d", character);
					}

					wordbuffer[wordidx++] = decoded;

					if (duration > dit_duration * 5 || wordidx > WORD_BUF_SIZE) {
						const uint64_t word_end_time = esp_timer_get_time();
						const uint64_t word_duration = word_end_time - word_start_time;
						// LOG("%s [%d]", wordbuffer, word_duration);
						wordtime w = {
							// .word = {0},
							.time = word_duration,
						};
						memcpy(w.word, wordbuffer, sizeof(wordbuffer));
						wordtimebuffer[wordtimeidx++] = w;
						if (wordtimeidx > WORDTIME_BUF_SIZE) {
							print_word_buf = true;
						}

						memset(wordbuffer, 0, sizeof(wordbuffer));
						wordidx = 0;

						word_start_time = esp_timer_get_time();
					}
				}
			}
			last_state = state;
		}
	}
}

char decode(const uint16_t character) {
	switch (character) {
	case 0b0111: return 'a';
	case 0b11010101: return 'b';
	case 0b11011101: return 'c';
	case 0b110101: return 'd';
	case 0b01: return 'e';
	case 0b01011101: return 'f';
	case 0b111101: return 'g';
	case 0b01010101: return 'h';
	case 0b0101: return 'i';
	case 0b01111111: return 'j';
	case 0b110111: return 'k';
	case 0b01110101: return 'l';
	case 0b1111: return 'm';
	case 0b1101: return 'n';
	case 0b111111: return 'o';
	case 0b01111101: return 'p';
	case 0b11110111: return 'q';
	case 0b011101: return 'r';
	case 0b010101: return 's';
	case 0b11: return 't';
	case 0b010111: return 'u';
	case 0b01010111: return 'v';
	case 0b011111: return 'w';
	case 0b11010111: return 'x';
	case 0b11011111: return 'y';
	case 0b11110101: return 'z';
	case 0b1111111111: return '0';
	case 0b0111111111: return '1';
	case 0b0101111111: return '2';
	case 0b0101011111: return '3';
	case 0b0101010111: return '4';
	case 0b0101010101: return '5';
	case 0b1101010101: return '6';
	case 0b1111010101: return '7';
	case 0b1111110101: return '8';
	case 0b1111111101: return '9';
	case 0b011101110111: return '.';
	case 0b111101011111: return ',';
	case 0b010111110101: return '?';
	case 0b011111111101: return '\'';
	case 0b110111011111: return '!';
	case 0b1101011101: return '/';
	case 0b1101111101: return '(';
	case 0b110111110111: return ')';
	case 0b0111010101: return '&';
	case 0b111111010101: return ':';
	case 0b110111011101: return ';';
	case 0b1101010111: return '=';
	case 0b0111011101: return '+';
	case 0b110101010111: return '-';
	case 0b010111110111: return '_';
	case 0b011101011101: return '"';
	case 0b01010111010111: return '$';
	case 0b01111101110: return '@';
	default: return ' ';
	}
}
