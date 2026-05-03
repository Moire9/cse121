/*!
 * @file DFRobot_RGBLCD1602.cpp
 * @brief DFRobot_RGBLCD1602 class infrastructure, the implementation of basic methods
 * @copyright	Copyright (c) 2010 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @licence     The MIT License (MIT)
 * @maintainer [yangfeng](feng.yang@dfrobot.com)
 * @version  V1.0
 * @date  2021-09-24
 * @url https://github.com/DFRobot/DFRobot_RGBLCD1602
 */

#include <stdio.h>

#include "DFRobot_RGBLCD1602.h"

#include "esp_check.h"
#include "driver/i2c_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ERRTRY(expr) do { const esp_err_t _result = (expr); if (_result != ESP_OK) return _result; } while(0)

static const char *NAME = "LcdLib";
#define LOG(...) ESP_LOGI(NAME, __VA_ARGS__)

const uint8_t color_define[4][3] = 
{
	{255, 255, 255},            // white
	{255, 0, 0},                // red
	{0, 255, 0},                // green
	{0, 0, 255},                // blue
};

/*******************************public*******************************/
DFRobot_RGBLCD1602::DFRobot_RGBLCD1602(uint8_t RGBAddr, uint8_t lcdCols,uint8_t lcdRows, uint8_t lcdAddr)
{
	_lcdAddr = lcdAddr;
	_RGBAddr = RGBAddr;
	_cols = lcdCols;
	_rows = lcdRows;
}

esp_err_t DFRobot_RGBLCD1602::init()
{
	LOG("Init call...");
	// I2C INIT:

	const i2c_master_bus_config_t busconf = {
		.i2c_port   = I2C_NUM_0, // i2c lib macro
		.sda_io_num = I2C_SDA_GPIO,
		.scl_io_num = I2C_SCL_GPIO,
		.clk_source = I2C_CLK_SRC_DEFAULT, // i2c lib macro
		.glitch_ignore_cnt = 7, // I have no idea what this means
		.intr_priority = 0, // default
		.trans_queue_depth = 0, // not async so doesn't matter
		.flags = {
			.enable_internal_pullup = true,
			.allow_pd = false
		},
	};

{
	const esp_err_t res = i2c_new_master_bus(&busconf, &_bus_handle);
	if (res != ESP_OK) return res;
}
	LOG("Bus init");

	i2c_device_config_t devconf = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address  = _lcdAddr,
		.scl_speed_hz    = I2C_FREQ,
		.scl_wait_us     = 0, // use default
		.flags = {
			.disable_ack_check = false,
		},
	};
	
{
	const esp_err_t res = i2c_master_bus_add_device(_bus_handle, &devconf, &_lcd_handle); // create LCD device
	if (res != ESP_OK) return res;
}
	
	LOG("LCD init");

	devconf.device_address = _RGBAddr;

	ERRTRY(i2c_master_bus_add_device(_bus_handle, &devconf, &_rgb_handle)); // create backlight device

	LOG("RGB init");

	if(_RGBAddr == (0x60)){
		REG_RED   =      0x04;
		REG_GREEN =      0x03;
		REG_BLUE  =      0x02;
		REG_ONLY  =      0x02 ;
	} else if(_RGBAddr == (0x60>>1)){
		REG_RED      =   0x06 ;       // pwm2
		REG_GREEN    =   0x07 ;       // pwm1
		REG_BLUE     =   0x08 ;       // pwm0
		REG_ONLY     =   0x08 ;
	} else if(_RGBAddr == (0x6B)){
		REG_RED      =   0x06 ;       // pwm2
		REG_GREEN    =   0x05 ;       // pwm1
		REG_BLUE     =   0x04 ;       // pwm0
		REG_ONLY     =   0x04 ; 
	} else if(_RGBAddr == (0x2D)){
		REG_RED      =   0x01 ;       // pwm2
		REG_GREEN    =   0x02 ;       // pwm1
		REG_BLUE     =   0x03 ;       // pwm0
		REG_ONLY     =   0x01 ; 
	}
	_showFunction = LCD_4BITMODE | LCD_1LINE | LCD_5x8DOTS;
	begin(_rows);

	LOG("Init done");

	return ESP_OK;
}

void DFRobot_RGBLCD1602::clear()
{
	command(LCD_CLEARDISPLAY);        // clear display, set cursor position to zero
	vTaskDelay(2 / portTICK_PERIOD_MS);   // this command takes a long time!
}

void DFRobot_RGBLCD1602::home()
{
	command(LCD_RETURNHOME);        // set cursor position to zero
	vTaskDelay(2 / portTICK_PERIOD_MS); // this command takes a long time!
}

void DFRobot_RGBLCD1602::noDisplay()
{
	_showControl &= ~LCD_DISPLAYON;
	command(LCD_DISPLAYCONTROL | _showControl);
}

void DFRobot_RGBLCD1602::display() {
	_showControl |= LCD_DISPLAYON;
	command(LCD_DISPLAYCONTROL | _showControl);
}

void DFRobot_RGBLCD1602::stopBlink()
{
	_showControl &= ~LCD_BLINKON;
	command(LCD_DISPLAYCONTROL | _showControl);
}
void DFRobot_RGBLCD1602::blink()
{
	_showControl |= LCD_BLINKON;
	command(LCD_DISPLAYCONTROL | _showControl);
}

void DFRobot_RGBLCD1602::noCursor()
{
	_showControl &= ~LCD_CURSORON;
	command(LCD_DISPLAYCONTROL | _showControl);
}

void DFRobot_RGBLCD1602::cursor() {
	_showControl |= LCD_CURSORON;
	command(LCD_DISPLAYCONTROL | _showControl);
}

void DFRobot_RGBLCD1602::scrollDisplayLeft(void)
{
	command(LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVELEFT);
}

void DFRobot_RGBLCD1602::scrollDisplayRight(void)
{
	command(LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVERIGHT);
}

void DFRobot_RGBLCD1602::leftToRight(void)
{
	_showMode |= LCD_ENTRYLEFT;
	command(LCD_ENTRYMODESET | _showMode);
}

void DFRobot_RGBLCD1602::rightToLeft(void)
{
	_showMode &= ~LCD_ENTRYLEFT;
	command(LCD_ENTRYMODESET | _showMode);
}

void DFRobot_RGBLCD1602::noAutoscroll(void)
{
	_showMode &= ~LCD_ENTRYSHIFTINCREMENT;
	command(LCD_ENTRYMODESET | _showMode);
}

void DFRobot_RGBLCD1602::autoscroll(void)
{
	_showMode |= LCD_ENTRYSHIFTINCREMENT;
	command(LCD_ENTRYMODESET | _showMode);
}

void DFRobot_RGBLCD1602::customSymbol(uint8_t location, uint8_t charmap[])
{

	location &= 0x7; // we only have 8 locations 0-7
	command(LCD_SETCGRAMADDR | (location << 3));
	
	
	uint8_t data[9];
	data[0] = 0x40;
	for(int i=0; i<8; i++)
	{
		data[i+1] = charmap[i];
	}
	send(data, 9);
}

void DFRobot_RGBLCD1602::setCursor(uint8_t col, uint8_t row)
{

	col = (row == 0 ? col|0x80 : col|0xc0);
	uint8_t data[3] = {0x80, col};

	send(data, 2);

}

void DFRobot_RGBLCD1602::setRGB(uint8_t r, uint8_t g, uint8_t b)
{
	uint16_t temp_r,temp_g,temp_b;
	if(_RGBAddr == 0x60>>1){
		temp_r = (uint16_t)r*192/255;
		temp_g = (uint16_t)g*192/255;
		temp_b = (uint16_t)b*192/255;
		backlightCmd(REG_RED, temp_r);
		backlightCmd(REG_GREEN, temp_g);
		backlightCmd(REG_BLUE, temp_b);
	} else{
		backlightCmd(REG_RED, r);
		backlightCmd(REG_GREEN, g);
		backlightCmd(REG_BLUE, b);
		if(_RGBAddr == 0x6B){
			backlightCmd(0x07, 0xFF);
		}
	}
}

void DFRobot_RGBLCD1602::setColor(uint8_t color)
{
	if(color > 3)return ;
	setRGB(color_define[color][0], color_define[color][1], color_define[color][2]);
}

void DFRobot_RGBLCD1602::closeBacklight(){setRGB(0, 0, 0);}
void DFRobot_RGBLCD1602::setColorWhite(){setRGB(255, 255, 255);}


inline void DFRobot_RGBLCD1602::writeChar(uint8_t value)
{
	uint8_t data[3] = {0x40, value};
	send(data, 2);
}

void DFRobot_RGBLCD1602::print(const char* text) {
	for (int i = 0; ; i++) {
		const char c = text[i];
		if (c == 0) break;

		writeChar(c);
	}
}

inline void DFRobot_RGBLCD1602::command(uint8_t value)
{
	uint8_t data[3] = {0x80, value};
	send(data, 2);
}


void DFRobot_RGBLCD1602::setBacklight(bool mode){
	if(mode){
		setColorWhite();		// turn backlight on
	}else{
		closeBacklight();		// turn backlight off
	}
}

void DFRobot_RGBLCD1602::goodbye() {
	i2c_master_bus_rm_device(_lcd_handle);
	i2c_master_bus_rm_device(_rgb_handle);
	i2c_del_master_bus(_bus_handle);
}

/*******************************private*******************************/
void DFRobot_RGBLCD1602::begin( uint8_t rows, uint8_t charSize) 
{
	if (rows > 1) {
		_showFunction |= LCD_2LINE;
	}
	_numLines = rows;
	_currLine = 0;
	///< for some 1 line displays you can select a 10 pixel high font
	if ((charSize != 0) && (rows == 1)) {
		_showFunction |= LCD_5x10DOTS;
	}

	///< SEE PAGE 45/46 FOR INITIALIZATION SPECIFICATION!
	///< according to datasheet, we need at least 40ms after power rises above 2.7V
	///< before sending commands. Arduino can turn on way befer 4.5V so we'll wait 50
	vTaskDelay(50 / portTICK_PERIOD_MS);

	///< this is according to the hitachi HD44780 datasheet
	///< page 45 figure 23

	///< Send function set command sequence
	command(LCD_FUNCTIONSET | _showFunction);
	vTaskDelay(5 / portTICK_PERIOD_MS);  // wait more than 4.1ms
	
	///< second try
	command(LCD_FUNCTIONSET | _showFunction);
	vTaskDelay(5 / portTICK_PERIOD_MS);

	///< third go
	command(LCD_FUNCTIONSET | _showFunction);

	///< turn the display on with no cursor or blinking default
	_showControl = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
	display();

	///< clear it off
	clear();

	///< Initialize to default text direction (for romance languages)
	_showMode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
	///< set the entry mode
	command(LCD_ENTRYMODESET | _showMode);
	
	if(_RGBAddr == (0xc0>>1)){
	  ///< backlight init
	  backlightCmd(REG_MODE1, 0);
	  ///< set LEDs controllable by both PWM and GRPPWM registers
	  backlightCmd(REG_OUTPUT, 0xFF);
	  ///< set MODE2 values
	  ///< 0010 0000 -> 0x20  (DMBLNK to 1, ie blinky mode)
	  backlightCmd(REG_MODE2, 0x20);
	}else if(_RGBAddr == (0x60>>1)){
	   backlightCmd(0x01, 0x00);
	   backlightCmd(0x02, 0xfF);
	   backlightCmd(0x04, 0x15);
	}else if(_RGBAddr==0x6B){
		backlightCmd(0x2F, 0x00);
		backlightCmd(0x00, 0x20);
		backlightCmd(0x01, 0x00);
		backlightCmd(0x02, 0x01);
		backlightCmd(0x03, 4);
	}
	setColorWhite();
}

void DFRobot_RGBLCD1602::send(uint8_t *data, uint8_t len)
{
	i2c_master_transmit(_lcd_handle, data, len, I2C_TIMEOUT);

	// _pWire->beginTransmission(_lcdAddr);        // transmit to device #4
	// for(int i=0; i<len; i++) {
	// 	_pWire->write(data[i]);
	// 	delayMicroseconds(100);
	// }
	// _pWire->endTransmission();                     // stop transmitting
}

void DFRobot_RGBLCD1602::backlightCmd(uint8_t addr, uint8_t data)
{
	uint8_t packet[] = {addr, data};
	i2c_master_transmit(_rgb_handle, packet, 2, I2C_TIMEOUT);

	// _pWire->beginTransmission(_RGBAddr); // transmit to device #4
	// _pWire->write(addr);
	// _pWire->write(data);
	// _pWire->endTransmission();    // stop transmitting
}
