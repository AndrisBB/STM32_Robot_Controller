#include "http_task.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"
#include "lwip.h"
#include "api.h"
#include "stdio.h"
#include "string.h"
#include "stdint.h"


#include "../picohttpparser.h"

#define NUMBER_HANDLER_THREADS	2

const static char indexdata[] =
"<html> \
<head><title>A test page</title></head> \
<body> \
This is a small test page. \
</body> \
</html>";

const static char http_html_hdr[] =
"Content-type: text/html\r\n\r\n";


static void CreateRequestHandlers();



static void StartRequestHandlerTask(void *argument);
static void process_connection(struct netconn *conn);

static QueueHandle_t requests_q;
typedef struct 
{
	struct netconn *conn;
} request_t;

void StartHttpTask(void *argument)
{
	printf("[HttpTask] Started\r\n");

	struct netconn *conn, *newconn;
	request_t request; 

	// Initialize Request Handler tasks



	conn = netconn_new(NETCONN_TCP);
	if(conn == NULL) 
	{
		printf("[HttpTask] Conn initialized OK\r\n");
		_Error_Handler(__FILE__, __LINE__);
	}

	if(netconn_bind(conn, NULL, 80) != ERR_OK)
	{
		printf("[HttpTask] Failed to bind port\r\n");
		_Error_Handler(__FILE__, __LINE__);
	}

	if(netconn_listen(conn) != ERR_OK)
	{
		printf("[HttpTask] Failed to start listening\r\n");
		_Error_Handler(__FILE__, __LINE__);
	}

	for (;;)
	{
		err_t err;
		if((err = netconn_accept(conn, &newconn)) == ERR_OK)
		{
			printf("[HttpTask] Accepted new connection\r\n");
			HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
			
			memset(&request, 0, sizeof(request));
			request.conn = newconn;

			if(xQueueSend(requests_q, &request,( TickType_t ) 10 ) != pdPASS )
			{
				printf("[HttpTask] Failed to enqueue request\r\n");
			}

			HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
		}
		else 
		{
			printf("[HttpTask] Failed to accept connection, %d\r\n", (int)err);
		}
	}
}

void CreateRequestHandlers()
{
	requests_q = xQueueCreate(16, sizeof(request_t));
	if(requests_q == NULL) {
		printf("[HttpTask] Failed to initialize requests queue\r\n");
		_Error_Handler(__FILE__, __LINE__);
	}

	char task_name[32];
	for(int i = 0; i < NUMBER_HANDLER_THREADS; i++) 
	{
		sprintf(task_name, "ReqHandler-%d", i);
		if(xTaskCreate(StartRequestHandlerTask, task_name, 2048, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS)
		{
			printf("[HttpTask] Failed to start handler task\r\n");
			_Error_Handler(__FILE__, __LINE__);
		}
	}
}

void StartRequestHandlerTask(void *argument)
{
	char *name = pcTaskGetName(NULL);
	printf("[%s] Started\r\n", name);

	request_t request;

	if(requests_q == NULL) 
	{
		printf("[%s] Requests Queue is NULL\r\n", name);
		_Error_Handler(__FILE__, __LINE__);
	}

	while(1) 
	{
		if(xQueueReceive(requests_q, &request, portMAX_DELAY))
		{
			printf("[%s] Got request to process\r\n", name);
			if(request.conn != NULL) 
			{
				osDelay(50);
				process_connection(request.conn);
				netconn_close(request.conn);
				netconn_delete(request.conn);
			}
		}
	}
}

static void process_connection(struct netconn *conn)
{
	char buf[1024];
	struct phr_header headers[16];

	struct netbuf *inbuf;
	char *rq;
	uint16_t len;
	err_t err = 0;

	// Pico
	const char *method, *path;
	int pret, minor_version;
	size_t buflen = 0, prevbuflen = 0, method_len, path_len, num_headers;

	if(conn == NULL)
	{
		printf("[%s]] Conn passed is NULL\r\n", pcTaskGetName(NULL));
		return;
	}

	while(1)
	{
		if((err = netconn_recv(conn, &inbuf)) != ERR_OK) {
			printf("[%s] Receive failure, %d\r\n", pcTaskGetName(NULL), (int)err);
			return;
		}

		if(netbuf_data(inbuf, (void *)&rq, &len) != ERR_OK)
		{
			printf("[%s] Failed to get data pointer\r\n", pcTaskGetName(NULL));
			_Error_Handler(__FILE__, __LINE__);
		}
		printf("[%s] Request length length %d bytes\r\n", pcTaskGetName(NULL), (int)len);

		memcpy(&buf[buflen], rq, (size_t)len);
		netbuf_delete(inbuf);

		prevbuflen = buflen;
		buflen += len;

		num_headers = sizeof(headers) / sizeof(headers[0]);
		pret = phr_parse_request(buf, buflen, &method, &method_len, &path, &path_len,
                             &minor_version, headers, &num_headers, prevbuflen);

		if(pret > 0)
		{
			printf("[%s] Buffer parsed OK\r\n", pcTaskGetName(NULL));
			break;
		}
    	else if(pret == -1) 
		{
			printf("[%s] Buffer parse Failure\r\n", pcTaskGetName(NULL));
			return;
		}

		if(buflen == sizeof(buf)) 
		{
			printf("[%s] Request to large\r\n", pcTaskGetName(NULL));
			return;
		}
	}

	printf("[%s] Header is %d bytes long\n", pcTaskGetName(NULL), pret);
	printf("[%s] Method is %.*s\n", pcTaskGetName(NULL), (int)method_len, method);
	printf("[%s] Path is %.*s\n", pcTaskGetName(NULL), (int)path_len, path);
	printf("[%s] HTTP version is 1.%d\n", pcTaskGetName(NULL), minor_version);
	printf("[%s] Headers:\n", pcTaskGetName(NULL));
	for(int i = 0; i != num_headers; ++i) 
	{
		printf("\t%.*s: %.*s\n", (int)headers[i].name_len, headers[i].name,
			(int)headers[i].value_len, headers[i].value);
	}

	printf("\r\n[%s] Send response\r\n", pcTaskGetName(NULL));
	netconn_write(conn, http_html_hdr, sizeof(http_html_hdr), NETCONN_NOCOPY);
	netconn_write(conn, indexdata, sizeof(indexdata), NETCONN_NOCOPY);
}