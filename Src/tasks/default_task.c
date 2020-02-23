#include "main.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"
#include "lwip.h"
#include "stdio.h"
#include "http_task.h"

extern struct netif gnetif;

void StartDefaultTask(void const *argument)
{
	int32_t got_ip = 0;

	printf("[DefaultTask] Started\r\n");

	MX_LWIP_Init();
	printf("[DefaultTask] LWIP initialized\r\n");

	for (;;)
	{
		if(got_ip == 0) 
		{
			if(dhcp_supplied_address(&gnetif)) 
			{
				printf("IP: %d.%d.%d.%d\r\n", ip4_addr1(&gnetif.ip_addr), 
											ip4_addr2(&gnetif.ip_addr), 
											ip4_addr3(&gnetif.ip_addr),
											ip4_addr4(&gnetif.ip_addr));
				got_ip = 1;
				if(xTaskCreate(StartHttpTask, "HttpTask", 512, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS)
				{
					_Error_Handler(__FILE__, __LINE__);
				}
			}
			else
			{
				printf("Waiting for IP address\r\n");
			}
		}

		HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
		osDelay(500);
	}
}