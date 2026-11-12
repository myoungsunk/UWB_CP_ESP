#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t);int xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,unsigned,TaskHandle_t*,int);
