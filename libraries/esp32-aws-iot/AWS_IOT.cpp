/***************************************************************************************************
                                    ExploreEmbedded Copyright Notice
****************************************************************************************************
 * File:   AWS_IOT.cpp
 * Version: 1.0
 * Author: ExploreEmbedded
 * Website: http://www.exploreembedded.com/wiki
 * Description: ESP32  Arduino library for AWS IOT.

This code has been developed and tested on ExploreEmbedded boards.
We strongly believe that the library works on any of development boards for respective controllers.
Check this link http://www.exploreembedded.com/wiki for awesome tutorials on 8051,PIC,AVR,ARM,Robotics,RTOS,IOT.
ExploreEmbedded invests substantial time and effort developing open source HW and SW tools, to support consider buying the ExploreEmbedded boards.

The ExploreEmbedded libraries and examples are licensed under the terms of the new-bsd license(two-clause bsd license).
See also: http://www.opensource.org/licenses/bsd-license.php

EXPLOREEMBEDDED DISCLAIMS ANY KIND OF HARDWARE FAILURE RESULTING OUT OF USAGE OF LIBRARIES, DIRECTLY OR
INDIRECTLY. FILES MAY BE SUBJECT TO CHANGE WITHOUT PRIOR NOTICE. THE REVISION HISTORY CONTAINS THE INFORMATION
RELATED TO UPDATES.


Permission to use, copy, modify, and distribute this software and its documentation for any purpose
and without fee is hereby granted, provided that this copyright notices appear in all copies
and that both those copyright notices and this permission notice appear in supporting documentation.
**************************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "AWS_IOT.h"
#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"

#include "aws_iot_mqtt_client.h"
#include "aws_iot_mqtt_client_interface.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"



static const char *TAG = "AWS_IOT";
char AWS_IOT_HOST_ADDRESS[128];

AWS_IoT_Client client;
IoT_Publish_Message_Params paramsQOS1;
pSubCallBackHandler_t subApplCallBackHandler = 0;

void aws_iot_task(void *param);

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
        IoT_Publish_Message_Params *params, void *pData)
{
    if(subApplCallBackHandler != 0) //User call back if configured
    subApplCallBackHandler(topicName,params->payloadLen,(char *)params->payload);
}



    void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data)
    {
        ESP_LOGW(TAG, "MQTT Disconnect");
        IoT_Error_t rc = FAILURE;

        if(!pClient)
        {
            return;
        }

        if(aws_iot_is_autoreconnect_enabled(pClient)) {
            ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
        }
        else
        {
            ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
            // TODO - was commented out - check
            rc = aws_iot_mqtt_attempt_reconnect(pClient);
            if(NETWORK_RECONNECTED == rc) {
                ESP_LOGW(TAG, "Manual Reconnect Successful");
            }
            else {
                ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
            }
        }
    }


    int AWS_IOT::connect(const char *hostAddress, const char *clientID,
                    const char *aws_root_ca_pem,
                    const char *certificate_pem_crt,
                    const char *private_pem_key) {
        const size_t stack_size = 36*1024;

        strcpy(AWS_IOT_HOST_ADDRESS,hostAddress);
        IoT_Error_t rc = FAILURE;


        IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
        IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;


        ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

        mqttInitParams.enableAutoReconnect = false; // We enable this later below
        mqttInitParams.pHostURL = AWS_IOT_HOST_ADDRESS;
        mqttInitParams.port = CONFIG_AWS_IOT_MQTT_PORT;


        mqttInitParams.pRootCALocation = const_cast<char*>(aws_root_ca_pem);
        mqttInitParams.pDeviceCertLocation = const_cast<char*>(certificate_pem_crt);
        mqttInitParams.pDevicePrivateKeyLocation = const_cast<char*>(private_pem_key);


        mqttInitParams.mqttCommandTimeout_ms = 20000;
        mqttInitParams.tlsHandshakeTimeout_ms = 5000;
        mqttInitParams.isSSLHostnameVerify = true;
        mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = nullptr;


    rc = aws_iot_mqtt_init(&client, &mqttInitParams);

    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        return rc; //abort();
    }

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    /* Client ID is set in the menuconfig of the example */
    connectParams.pClientID = const_cast<char*>(clientID);
    connectParams.clientIDLen = (uint16_t) strlen(clientID);
    connectParams.isWillMsgPresent = false;
    connectParams.connectRetries = 3;

    ESP_LOGI(TAG, "Connecting to AWS...");
    int retries = 0;
    do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);

        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d, \n\rTrying to reconnect", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            if (retries > connectParams.connectRetries) {
              ESP_LOGE(TAG, "Retries exceeded %d. Exiting...", retries);
              return rc;

            }
            vTaskDelay(/*1000*/ 50 / portTICK_RATE_MS);
            //vTaskDelay(1);
            retries++;
        }
    } while(SUCCESS != rc);


    /*
     * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
     *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
     *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
     */
    // TODO - bock was commented out - check
    rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    if(rc == SUCCESS)
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", stack_size, nullptr, 5, nullptr, 1);

    return rc;
}


int AWS_IOT::publish(const char *pubtopic,const char *pubPayLoad)
{
    IoT_Error_t rc;

    paramsQOS1.qos = QOS1;
    paramsQOS1.payload = const_cast<char*>(pubPayLoad);
    paramsQOS1.isRetained = 0;

    paramsQOS1.payloadLen = strlen(pubPayLoad);
    rc = aws_iot_mqtt_publish(&client, pubtopic, strlen(pubtopic), &paramsQOS1);

    return rc;
}



int AWS_IOT::subscribe(const char *subTopic, pSubCallBackHandler_t pSubCallBackHandler)
{
    IoT_Error_t rc;

    subApplCallBackHandler = pSubCallBackHandler;

    ESP_LOGI(TAG, "Subscribing...");
    rc = aws_iot_mqtt_subscribe(&client, subTopic, strlen(subTopic), QOS1, iot_subscribe_callback_handler, nullptr);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Error subscribing : %d ", rc);
        return rc;
    }
    ESP_LOGI(TAG, "Subscribing... Successful");

    return rc;
}




void aws_iot_task(void *param) {

IoT_Error_t rc = SUCCESS;

    while(1)
    {
        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client, /*200*/ 5);

        if(NETWORK_ATTEMPTING_RECONNECT == rc)
        {
            // If the client is attempting to reconnect we will skip the rest of the loop.
            continue;
        }


        vTaskDelay(/*1000*/ 550 / portTICK_RATE_MS);
    }
}
