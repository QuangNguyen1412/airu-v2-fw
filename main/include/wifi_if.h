/*
 * wifi_if.h
 *
 *  Created on: Oct 7, 2018
 *      Author: tombo
 */

#ifndef WIFI_IF_H
#define WIFI_IF_H


#include "esp_system.h"


/*
* @brief
*
* @param
*
* @return
*/
void wifi_sta_Initialize();


/*
* @brief
*
* @param
*
* @return
*/
esp_err_t wifi_sta_event_handler(void *ctx, system_event_t *event);


#endif /* WIFI_IF_H */
