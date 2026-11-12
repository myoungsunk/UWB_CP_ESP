#pragma once
#include "FreeRTOS.h"
QueueHandle_t xQueueCreate(unsigned,unsigned);int xQueueSend(QueueHandle_t,const void*,TickType_t);int xQueueSendToFront(QueueHandle_t,const void*,TickType_t);int xQueueReceive(QueueHandle_t,void*,TickType_t);
