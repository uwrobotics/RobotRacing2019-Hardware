
/*
 * Copyright 2016-2018 NXP
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of NXP Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
/**
 * @file    Hummingbot_firmware_FreeRTOS.c
 * @brief   Application entry point.
 */


#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "MKE14F16.h"
#include "common.h"
#include "nrf24l01/RF24_common.h"
#include "nrf24l01/RF24.h"
#include "Servo/Servo.h"
#include "Hummingconfig.h"

#include "fsl_debug_console.h"
#include "fsl_device_registers.h"
#include "fsl_gpio.h"
#include "fsl_lpuart.h"
#include "fsl_lpspi.h"
#include "fsl_lpspi_freertos.h"
#include "fsl_lpi2c.h"
#include "fsl_lpi2c_freertos.h"

//libraries for freeRTOS
#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOSConfig.h"

/*************************************  
 ********* Macro Preference ********** 
 *************************************/
#define ENABLE_FEATURE_DEBUG_PRINT      1 //This will enable uart debug print out
#define ENABLE_TASK_RF24								0
#define ENABLE_TASK_VEHICLE_CONTROL    	0
#define TEST_FTM_PWM										1
/*************************************  
 ********* Macro Definitions ********** 
 *************************************/
/* Task Tick calculation */
#define HELPER_TASK_FREQUENCY_HZ(x)     (configTICK_RATE_HZ/(x))

/* Debug PRINTF helper functions */
#define DEBUG_PRINTLN(fmt, ...) \
            do { if (ENABLE_FEATURE_DEBUG_PRINT) PRINTF(fmt "\r\n", ##__VA_ARGS__); } while (0)
#define DEBUG_PRINT_ERR(fmt, ...) \
            do { if (ENABLE_FEATURE_DEBUG_PRINT) PRINTF("[ERR.]" fmt "\r\n", ##__VA_ARGS__); } while (0)
#define DEBUG_PRINT_WRN(fmt, ...) \
            do { if (ENABLE_FEATURE_DEBUG_PRINT) PRINTF("[WARN]" fmt "\r\n", ##__VA_ARGS__); } while (0)
#define DEBUG_PRINT_INFO(fmt, ...) \
            do { if (ENABLE_FEATURE_DEBUG_PRINT) PRINTF("[INFO]" fmt "\r\n", ##__VA_ARGS__); } while (0)

/***********************************
 ********* Macro Settings **********
 ***********************************/
/* Task Priority */
#define TASK_RF24_PRIORITY 							 			(configMAX_PRIORITIES - 1)
#define ENABLE_TASK_VEHICLE_CONTROL_PRIORITY 	(configMAX_PRIORITIES - 2)
/* Task Frequency */
#define TASK_RF24_FREQ                   			(HELPER_TASK_FREQUENCY_HZ(10)) //Hz
#define TENABLE_TASK_VEHICLE_CONTROL_FREQ     (HELPER_TASK_FREQUENCY_HZ(10)) //Hz //TODO: TBI, to be implemented

/***********************************
 ********* Macro Helpers **********
 ***********************************/
#if (ENABLE_TASK_VEHICLE_CONTROL)
#define SERVO_PWM_PERIOD_TICKS      		(int) (SERVO_PWM_PERIOD_MS * ((float)configTICK_RATE_HZ / 1000))
#define SERVO_CONVERT_CYCLE_2_TICKS(c)  (int)(SERVO_PWM_PERIOD_MS * (c / (float) 100) * (configTICK_RATE_HZ / 1000))
#define SERVO_CONVERT_ANGLE_2_TICKS(a)  SERVO_CONVERT_CYCLE_2_TICKS((float)(2.6986 + (7.346 - 2.6986) / 90.0 * a))
#define SERVO_MIN_TICKS         				SERVO_CONVERT_ANGLE_2_TICKS(SERVO_MIN_ANGLE)
#define SERVO_MAX_TICKS         				SERVO_CONVERT_ANGLE_2_TICKS(SERVO_MAX_ANGLE)
#endif //(ENABLE_TASK_VEHICLE_CONTROL)

/***************************************  
 *********  Struct/Enums Defs ********** 
 ***************************************/
typedef struct{
	uint16_t 	rf24_buf[2]; //[MSB] 12 bit (steer) | 12 bit (spd) | 8 bit (6 bit pattern + 2 bit modes)
  pin_t 		rf24_ce;
  uint8_t 	rf24_address[RF24_COMMON_ADDRESS_SIZE];
	lpspi_t   spi;
	uint8_t   buf_rx[9];
	uint8_t   buf_tx[9];
}Hummingbot_firmware_FreeRTOS_2_data_S;

/*************************************
 ********* Inline Definitions **********
 *************************************/
static inline void printHummingBoardLogo(void)
{
  // will not print, if the (ENABLE_FEATURE_DEBUG_PRINT) is disabled, [implicit relationship]
  DEBUG_PRINTLN("#############################################################################################");
  DEBUG_PRINTLN("##     ## ##     ## ##     ## ##     ## #### ##    ##  ######   ########   #######  ######## ");
  DEBUG_PRINTLN("##     ## ##     ## ###   ### ###   ###  ##  ###   ## ##    ##  ##     ## ##     ##    ##    ");
  DEBUG_PRINTLN("##     ## ##     ## #### #### #### ####  ##  ####  ## ##        ##     ## ##     ##    ##    ");
  DEBUG_PRINTLN("######### ##     ## ## ### ## ## ### ##  ##  ## ## ## ##   #### ########  ##     ##    ##    ");
  DEBUG_PRINTLN("##     ## ##     ## ##     ## ##     ##  ##  ##  #### ##    ##  ##     ## ##     ##    ##    ");
  DEBUG_PRINTLN("##     ## ##     ## ##     ## ##     ##  ##  ##   ### ##    ##  ##     ## ##     ##    ##    ");
  DEBUG_PRINTLN("##     ##  #######  ##     ## ##     ## #### ##    ##  ######   ########   #######     ##    ");
  DEBUG_PRINTLN("#############################################################################################");
}

/***************************************  
 *********  Private Variable ********** 
 ***************************************/
Hummingbot_firmware_FreeRTOS_2_data_S m_data;

/************************************************  
 ********* Private Function Prototypes ********** 
 ***********************************************/
#if (ENABLE_TASK_RF24)
  static void task_rf24(void *pvParameters);
#endif // (ENABLE_TASK_RF24
#if (ENABLE_TASK_VEHICLE_CONTROL)
  static void task_steeringControl(void *pvParameters);
#endif // (ENABLE_TASK_VEHICLE_CONTROL)
/**************************************  
 ********* Private Functions ********** 
 *************************************/
#if (ENABLE_TASK_RF24)
static void task_rf24(void *pvParameters)
{
  TickType_t xLastWakeTime;
  // Initialize the xLastWakeTime variable with the current time.
  xLastWakeTime = xTaskGetTickCount();
	while(1) 
	{
	  DEBUG_PRINT_INFO("Scanning");
		if (RF24_available())
		{
//		    char text[32] = "";
//		    RF24_read(&text, sizeof(text));
//		    DEBUG_PRINT_INFO("RCV: %s",text);
			RF24_read(&m_data.rf24_buf, sizeof(m_data.rf24_buf));
			uint32_t temp = (m_data.rf24_buf[1]<<16) + m_data.rf24_buf[0];
			uint16_t temp1 = temp >>20;
			uint16_t temp2 = (temp>>8)&(0xFFF);
			uint16_t temp3 = temp&(0xFF);
			if(temp3!=0) //TODO: filter out with pattern
			{
				DEBUG_PRINT_INFO("RCV: %d | %d | %d", temp1, temp2, temp3);
			}else{
				DEBUG_PRINT_ERR("Invalid Message %d", temp3);
			}
		}
		vTaskDelayUntil(&xLastWakeTime, TASK_RF24_FREQ);
	}
}
#endif

#if (ENABLE_TASK_VEHICLE_CONTROL)
static void task_steeringControl(void *pvParameters)
{
	int angle = 85;

	while(1) {
	  //toggle High
	  GPIO_PortSet(SERVO_GPIO_PORT, 1u << SERVO_GPIO_PIN);

	  //delay ticks
	  vTaskDelay(SERVO_CONVERT_ANGLE_2_TICKS(angle));

	  // Toggle Low
	  GPIO_PortClear(SERVO_GPIO_PORT, 1u << SERVO_GPIO_PIN);

	   //delay 20ms - ticks
	   vTaskDelay(SERVO_PWM_PERIOD_TICKS - SERVO_CONVERT_ANGLE_2_TICKS(angle));
	}
}
#endif //(ENABLE_TASK_VEHICLE_CONTROL)

/*********************** 
 ********* APP ********* 
 **********************/
/*
 * @brief   Application entry point.
 */
int main(void) {
	/*---- INIT --------------------------------------------------------*/
	/* Init board hardware. */
	BOARD_InitBootPins();
	BOARD_InitBootClocks();
	BOARD_InitBootPeripherals();
	BOARD_BootClockRUN();
	/* Init FSL debug console. */
	BOARD_InitDebugConsole();
	printHummingBoardLogo();
	DEBUG_PRINT_INFO(" ****** ******************* ******");
	DEBUG_PRINT_INFO(" ****** Hummingboard begin ******");
  /*---- Custom INIT --------------------------------------------------*/
	DEBUG_PRINT_INFO(" ****** Hummingboard Init ... ******");
	/* Init private data */
	memset(&m_data, 0, sizeof(m_data));
	/* Init rf24 */
#if (ENABLE_TASK_RF24)
	m_data.rf24_ce.port = RF24_COMMON_DEFAULT_CE_PORT;
	m_data.rf24_ce.pin = RF24_COMMON_DEFAULT_CE_PIN;
	memcpy(m_data.rf24_address, RF24_COMMON_ADDRESS, sizeof(char)*RF24_COMMON_ADDRESS_SIZE);
#endif // (ENABLE_TASK_RF24)

#if(TEST_FTM_PWM)
	// INIT =========
	SERVO_ServoConfig_S servoConfigs[1];
	memset(servoConfigs, 0, sizeof(servoConfigs));
	servoConfigs->gpio.pin = HUMMING_CONFIG_EXAMPLE_GPIO_PIN;
	servoConfigs->gpio.port= HUMMING_CONFIG_EXAMPLE_GPIO_PORT;
	servoConfigs->refreshingPeriod = HUMMING_CONFIG_EXAMPLE_PWM_PERIOD;
	servoConfigs->defaultPulseWidth_us =	HUMMING_CONFIG_EXAMPLE_DEFAULT_PW;
	servoConfigs->minPulseWidth_us =	HUMMING_CONFIG_EXAMPLE_MIN_PW;
	servoConfigs->maxPulseWidth_us =	HUMMING_CONFIG_EXAMPLE_MAX_PW;
	SERVO_init(servoConfigs, 1);

	// CODE ============
	uint32_t cnt;
	uint32_t loop = 4U;
	uint32_t milisecondCounts = 0;
	uint32_t secondLoop = 10000U;//10us
 	const char *signals = "-|";
	DEBUG_PRINT_INFO(" ****** Counter begin ******");
	cnt = 0;
	volatile uint16_t temp = 600U;
	bool increment = true;
	SERVO_requestStart(0);
    while (true)
    {
        if (SERVO_getNotifiedByNewTick())
        {
            milisecondCounts++;
            if (milisecondCounts >= secondLoop)//100ms
            {
//                DEBUG_PRINT_INFO(" %c", signals[cnt & 1]);
                cnt++;
                if (cnt >= loop)
                {
                    cnt = 0;
                }
                milisecondCounts = 0U;
//                SERVO_write_us(0, 1500U);
//                switch(cnt)
//                {
//                  case 0:
//                    SERVO_writeMicroseconds(1500U);//1250
//                    break;
//
//                  case 1:
//                    SERVO_writeMicroseconds(2400U);
//                    break;
//
//                  case 2:
//                    SERVO_writeMicroseconds(1500U);
//                    break;
//
//                  default:
//                    SERVO_writeMicroseconds(600U);
//                    break;
//                }
                if(temp>=2400)
                  increment = false;
                else if (temp<=600)
                  increment = true;
                temp += increment?(100):(-100);
                SERVO_write_us(0, temp);
            }
        }
        __WFI();
    }
#endif
	/*---- CONFIG --------------------------------------------------------*/
	 DEBUG_PRINT_INFO(" ****** Hummingboard Config ... ******");
	/* Config rf24 */
#if (ENABLE_TASK_RF24)
   RF24_config(&m_data.rf24_ce);
   RF24_INIT_STATUS_E status = RF24_init();
   if(status == RF24_INIT_STATUS_SUCCESS)
   {
     RF24_setDataRate( RF24_250KBPS );//low data rate => longer range and reliable
     RF24_enableAckPayload();
     RF24_setRetries(3,2);
     RF24_openReadingPipe(0, m_data.rf24_address);
     RF24_setPALevel(RF24_PA_LOW);
     RF24_startListening();
//     RF24_openReadingPipe(0, m_data.rf24_address);
//     RF24_setPALevel(RF24_PA_MIN);
//     RF24_startListening();
   }
	 else
	 {
			DEBUG_PRINT_ERR(" Failed to configure RF24 Module!");
	 }

 // TODO: remove these testing code
//  uint8_t temp = 10;
//  RF24_DEBUG_spiTestingCode();
//   while(1){
//     DEBUG_PRINT_INFO(" Ticking ...");
//     write_register_buf(1, &temp, 1);
//   }
#endif
		/* Config vehicle control steering & motoring */
#if (ENABLE_TASK_VEHICLE_CONTROL)
	 /* Define the init structure for the output LED pin*/
	   gpio_pin_config_t servo_motor_gpio_config = {
	     kGPIO_DigitalOutput, 0,
	   };

	 // INIT Servo PWM GPIO Pin
	 GPIO_PinInit(SERVO_GPIO_PORT, SERVO_GPIO_PIN, &servo_motor_gpio_config );
#endif //(ENABLE_TASK_VEHICLE_CONTROL)

	/*---- TASK CONFIGS --------------------------------------------------------*/
  DEBUG_PRINT_INFO(" ****** Hummingboard Config Tasks ... ******");
#if (ENABLE_TASK_RF24)
	if (xTaskCreate(task_rf24, "task_rf24", configMINIMAL_STACK_SIZE + 10, NULL, TASK_RF24_PRIORITY, NULL) != pdPASS)
	{
	  DEBUG_PRINT_ERR("Task creation failed!.");
		while (1);
	}
#endif
#if (ENABLE_TASK_VEHICLE_CONTROL)
	if (xTaskCreate(task_steeringControl, "task_steeringControl", configMINIMAL_STACK_SIZE + 10, NULL, ENABLE_TASK_VEHICLE_CONTROL_PRIORITY, NULL) != pdPASS)
	  {
	    DEBUG_PRINT_ERR("Task creation failed!.\r\n");
	    while (1);
	  }
#endif //(ENABLE_TASK_VEHICLE_CONTROL)


	/*---- TASK SCHEDULAR START --------------------------------------------------------*/
  DEBUG_PRINT_INFO(" ****** Hummingboard Running ******");
  DEBUG_PRINT_INFO(" ****** ******************* ******");
	vTaskStartScheduler();
	return 0;
}

