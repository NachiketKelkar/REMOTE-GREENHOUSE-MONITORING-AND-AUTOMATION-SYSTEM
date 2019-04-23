/*
 * sensor.c
 *
 *  Created on: Apr 21, 2019
 *      Author: nachi
 */

#include <stdint.h>
#include <stdbool.h>
#include "driverlib/sysctl.h"
#include "driverlib/debug.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "inc/hw_memmap.h"
#include "utils/uartstdio.h"
#include "../FreeRTOS/include/projdefs.h"

#include "sensor.h"
#include "spi.h"
#include "driverlib/gpio.h"
#include "driverlib/adc.h"
#include "driverlib/pin_map.h"
#include "driverlib/ssi.h"

// FreeRTOS includes
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

extern uint32_t g_ui32SysClock;
extern TaskHandle_t TempTaskHandle;
extern TaskHandle_t SMTaskHandle;
extern QueueHandle_t IBQueue;

void moisture_sensor_init()
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_3);
    ADCSequenceConfigure(ADC0_BASE, SEQUENCE_NO, ADC_TRIGGER_PROCESSOR, 0);
    ADCSequenceStepConfigure(ADC0_BASE, SEQUENCE_NO, 0, ADC_CTL_CH0 | ADC_CTL_IE | ADC_CTL_END);
    ADCSequenceEnable(ADC0_BASE, SEQUENCE_NO);
    ADCIntClear(ADC0_BASE, SEQUENCE_NO);
}

uint32_t moisture_data()
{
    uint32_t data;
    ADCProcessorTrigger(ADC0_BASE, SEQUENCE_NO);
    while(!ADCIntStatus(ADC0_BASE, SEQUENCE_NO, false))
    {
    }
    ADCIntClear(ADC0_BASE, SEQUENCE_NO);
    ADCSequenceDataGet(ADC0_BASE, SEQUENCE_NO, &data);
    SysCtlDelay(g_ui32SysClock / 12);
    return data;
}


void temp_sens_init(uint32_t mode, uint32_t clk_speed)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_SSI3);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF));

    /* Configure the GPIO pins for using it as SPI */
    GPIOPinConfigure(GPIO_PF0_SSI3XDAT1);
    GPIOPinConfigure(GPIO_PF1_SSI3XDAT0);
    GPIOPinConfigure(GPIO_PF2_SSI3FSS);
    GPIOPinConfigure(GPIO_PF3_SSI3CLK);

    GPIOPinTypeSSI(GPIO_PORTF_BASE, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3);

    SSIConfigSetExpClk(SSI3_BASE, g_ui32SysClock, SSI_FRF_MOTO_MODE_0, mode, clk_speed, 16);
    SSIEnable(SSI3_BASE);
}


uint16_t temp_data_get()
{
    uint32_t buffer;
    /* junk value to start the SPI transaction */
    uint16_t junk_val = 0x1234;
    SSIDataPut(SSI3_BASE, junk_val);
    SSIDataGet(SSI3_BASE, &buffer);
    return (uint16_t)buffer;
}


void TemperatureTask(void *pvParameters)
{
    IBStruct data_to_send;
    data_to_send.source = TEMP_SOURCE_ID;
    /* Initialize the temperature sensor */
    temp_sens_init(MASTER, TEMP_SPI_CLK);
    /* Initialize the timer for periodic measurements */
    TimerHandle_t TakeTempReadings = xTimerCreate("TakeTemperature", pdMS_TO_TICKS(2000), pdTRUE, (void*)0, TemperatureCallback);
    /* Start the timer after 100ms */
    BaseType_t return_val = xTimerStart(TakeTempReadings, pdMS_TO_TICKS(0));
    while(1)
    {
        /* Wait for notification from the timer to take reading from sensors */
        xTaskNotifyWait(0x00, 0xffffffff, NULL, portMAX_DELAY);
//        UARTprintf("Temp task notify reading\n");

        /* Take the reading from the sensor */
        data_to_send.data = temp_data_get()>>3;

        /* Send it to the queue of the SPI task */
        xQueueSend(IBQueue, &data_to_send, pdMS_TO_TICKS(0));
    }
}


void TemperatureCallback(TimerHandle_t xtimer)
{
    /* Notify the task to take the readings */
    if(TempTaskHandle != NULL)
    {
        xTaskNotify(TempTaskHandle, 1, eSetBits);
    }
}


void SoilMoistureTask(void *pvParameters)
{
    UARTprintf("Moist task\n");
    IBStruct data_to_send;
    data_to_send.source = SM_SOURCE_ID;
    /* Initialize the soil moisture sensor ADC. */
    moisture_sensor_init();
    // Initialize the timer for periodic measurements */
    TimerHandle_t TakeSoilReadings = xTimerCreate("TakeSoilMoisture", pdMS_TO_TICKS(2000), pdTRUE, (void*)0, MoistureCallback);
    /*  Start the timer after 100ms */
    BaseType_t return_val = xTimerStart(TakeSoilReadings, pdMS_TO_TICKS(1000));
    if(return_val != pdPASS)
    {
        UARTprintf("Moisture timer failed\n");
    }
    while(1)
    {
        /* Wait for notification from the timer to take reading from sensors */
        xTaskNotifyWait(0x00, 0xffffffff, NULL, portMAX_DELAY);
//        UARTprintf("Moist task notify rec\n");

        /* Take the reading from the sensor */
        data_to_send.data = moisture_data();

        /* Send it to the queue of the SPI task */
        xQueueSend(IBQueue, &data_to_send, pdMS_TO_TICKS(0));
    }
}


void MoistureCallback(TimerHandle_t xtimer)
{
    /* Notify the task to take the readings */
    if(SMTaskHandle != NULL)
    {
        xTaskNotify(SMTaskHandle, 1, eSetBits);
    }
}


float temperature_in_c(uint16_t hex_val)
{
    hex_val = hex_val >> 3;
    return (hex_val * TEMP_CONV_FACTOR);
}
