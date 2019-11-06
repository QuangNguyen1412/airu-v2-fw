/*
Copyright (c) 2018 University of Utah

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file main.c
@author Thomas Becnel
@author Trenton Taylor
@brief Entry point for the ESP32 application.
@thanks Special thanks to Tony Pottier for the esp32-wifi-manager repo
	@see https://idyl.io
	@see https://github.com/tonyp7/esp32-wifi-manager

Notes:
	- GPS: 	keep rolling average of GPS alt, lat, lon. Set up GPS UART handler
			similar to PM UART, where every sample that comes in gets parsed.
			Accumulate the last X measurements, and when we publish over MQTT
			take an average and send it. We need the GPS location to be the same
			within 4 decimal points, so we can use it as a tag in InfluxDB.
*/

// Base necessary
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_adc_cal.h"
#include "esp_spi_flash.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "app_utils.h"
#include "pm_if.h"
#include "led_if.h"
#ifdef CONFIG_USE_SD
#include "sd_if.h"
#endif
#include "hdc1080_if.h"
#include "mics4514_if.h"
#include "gps_if.h"

// Internet necessary
#include "esp_wifi.h"
#include "mdns.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "esp_ota_ops.h"

#include "http_file_upload.h"
//#include "http_server_if.h"
#include "wifi_manager.h"
#include "mqtt_if.h"
#include "time_if.h"
#include "ota_if.h"


/* GPIO */
#define STAT1_LED 21
#define STAT2_LED 19
#define STAT3_LED 18
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<STAT1_LED) | (1ULL<<STAT2_LED) | (1ULL<<STAT3_LED))

#define ONE_MIN 					60
#define ONE_HR						ONE_MIN * 60
#define ONE_DAY						ONE_HR * 24
#define FILE_UPLOAD_WAIT_TIME_SEC	30 //ONE_HR * 6


//static char DEVICE_MAC[13];
static TaskHandle_t task_http_server = NULL;
static TaskHandle_t task_wifi_manager = NULL;
static TaskHandle_t data_task_handle = NULL;
static TaskHandle_t task_ota = NULL;
static TaskHandle_t task_led = NULL;
static TaskHandle_t task_uploadcsv = NULL;
static TaskHandle_t task_offlinetracker = NULL;
static const char *TAG = "AIRU";
static const char *TAG_OFFLINE_TRACKER = "OFFLINE";
static const char *TAG_UPLOAD = "UPLOAD";

const char file_upload_nvs_namespace[] = "fileupload";
const char* earliest_missed_data_ts = "offline";
const char* last_upload_ts = "lastup";
time_t last_publish = 0;

///**
// * @brief RTOS task that periodically prints the heap memory available.
// * @note Pure debug information, should not be ever started on production code!
// */
//void monitoring_task(void *pvParameter)
//{
//	while(1){
//		printf("free heap: %d\n",esp_get_free_heap_size());
//		vTaskDelay(5000 / portTICK_PERIOD_MS);
//	}
//}

void panic_task(void *pvParameters)
{
	uint64_t free_stack;
	time_t now = 0;
	while(1) {
		vTaskDelay(60 * 60 * 1000 / portTICK_PERIOD_MS);
		ESP_LOGI(TAG, "Rebooting...");
		abort();
//		now = esp_timer_get_time() / 1000000;
//		ESP_LOGI(TAG, "now: %li, last: %li, diff: %li", now, last_publish, (now - last_publish));
//		if(last_publish != 0 && now - last_publish > 3600){
//			ESP_LOGE(TAG, "No pub in 1 hr. Rebooting.");
//			abort();
//		}
//		free_stack = uxTaskGetStackHighWaterMark(NULL);
//		ESP_LOGI(TAG, "%s free stack: %llu", __func__, free_stack);
	}
}

/*
 * Data gather task
 */
void data_task()
{
	esp_err_t err;
	pm_data_t pm_dat;
	double temp, hum;
	int co, nox;
	esp_gps_t gps;
	char *pkt;
	uint64_t uptime = 0;
	uint64_t hr, rm;
	time_t now;
	struct tm tm;
	char strftime_buf[64];
	uint8_t min, sec, system_time;

	// only need to get it once
	esp_app_desc_t *app_desc = esp_ota_get_app_description();
	MICS4514_Enable();

	while (1) {
		vTaskDelay(CONFIG_DATA_UPLOAD_PERIOD * 1000 / portTICK_PERIOD_MS);

		PMS_Poll(&pm_dat);
		HDC1080_Poll(&temp, &hum);
		MICS4514_Poll(&nox, &co);
		GPS_Poll(&gps);

		uptime = esp_timer_get_time() / 1000000;

		pkt = malloc(MQTT_PKT_LEN);

		//
		// Send data over MQTT
		//
		sprintf(pkt, MQTT_PKT, DEVICE_MAC,			/* ID 			*/
							   app_desc->version,	/* SensorModel 	*/
							   uptime, 				/* secActive 	*/
							   gps.alt,				/* Altitude 	*/
							   gps.lat, 			/* Latitude 	*/
							   gps.lon, 			/* Longitude 	*/
							   pm_dat.pm1,			/* PM1 			*/
							   pm_dat.pm2_5,		/* PM2.5 		*/
							   pm_dat.pm10, 		/* PM10 		*/
							   temp,				/* Temperature 	*/
							   hum,					/* Humidity 	*/
							   co,					/* CO 			*/
							   nox);				/* NOx 			*/

		ESP_LOGI(TAG, "MQTT PACKET:\n\r%s", pkt);
		err = MQTT_Publish_Data(pkt);
		if(err >= ESP_OK){
			ESP_LOGI(TAG, "MQTT publish success %d", err);
			last_publish = uptime;
		}
		else{
			ESP_LOGI(TAG, "MQTT publish fail %d", err);
		}

#ifdef CONFIG_SD_DATA_STORE
		/************************************
		 * Save to SD Card
		 *************************************/
		time(&now);
		localtime_r(&now, &tm);
		strftime(strftime_buf, sizeof(strftime_buf), "%c", &tm);
		ESP_LOGI(TAG, "SD card datetime: %s", strftime_buf);

		if (gps.year <= 18 || gps.year >= 80){
			hr = uptime / 3600;
			rm = uptime % 3600;
			min = rm / 60;
			sec = rm % 60;

			sprintf(strftime_buf, "%llu:%02d:%02d", hr, min, sec);
			system_time = 1;	// Using system time
		}
		else {
			sprintf(strftime_buf, "%02d:%02d:%02d", gps.hour, gps.min, gps.sec);
			system_time = 0;	// Using GPS time
		}

		sprintf(pkt, SD_PKT, strftime_buf,
							 DEVICE_MAC,
							 MQTT_DATA_PUB_TOPIC,
							 uptime,
							 gps.alt,
							 gps.lat,
							 gps.lon,
							 pm_dat.pm1,
							 pm_dat.pm2_5,
							 pm_dat.pm10,
							 temp,
							 hum,
							 co,
							 nox);

		sd_write_data(pkt, gps.year, gps.month, gps.day);
		periodic_timer_callback(NULL);
#endif

		free(pkt);

	}
}


void app_main()
{
	APP_Initialize();
	LED_Initialize();
	GPS_Initialize();
	PMS_Initialize();
	HDC1080_Initialize();
	MICS4514_Initialize();
	SD_Initialize();
//	ESP_LOGI(TAG, "Initialize WIFI");
	initialise_wifi();
	ESP_LOGI(TAG, "Initialize SNTP");
	SNTP_Initialize();
	ESP_LOGI(TAG, "Initialize MQTT");
	MQTT_Initialize();

	xTaskCreate(&led_task, "led_task", 2048, NULL, 3, &task_led);
	xTaskCreate(&data_task, "data_task", 4096, NULL, 1, &data_task_handle);
	xTaskCreate(&ota_task, "ota_task", 4096, NULL, 10, &task_ota);
	xTaskCreate(&panic_task, "panic", 2048, NULL, 5, NULL);
}
