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
#include "../re.h"


#define REQUESTS_QUEUE_LEN		16
#define NUMBER_HANDLER_THREADS	1

const static char indexdata[] =
"<html> \
<head><title>A test page</title></head> \
<body> \
This is a small test page. \
</body> \
</html>";

const static char http_html_hdr[] =
"Content-type: text/html\r\n\r\n";

typedef struct 
{
	struct netconn 		*conn;
	char 				*inbuf;
	struct phr_header 	*headers;
	size_t 				headers_len;
	int32_t 			num_headers;
	char				path[32];
} request_t;

// ------------------------------------------------------
// Routing table typedefs
// ------------------------------------------------------
typedef struct regex_t* re_t;
typedef int32_t (*endpoint_handler_t)(request_t *request);

typedef struct {
	char re_pattern[32];
	endpoint_handler_t handler;
} route_t;

// ------------------------------------------------------
// Function prototypes
// ------------------------------------------------------
static int32_t StartRequestHandlers();
static int32_t InitIncomingConn(struct netconn **conn);

static void StartRequestHandlerTask(void *argument);
static void process_connection(struct netconn *conn);

static int32_t RegisterRoutes();
static int32_t RegisterRoute(char *re_pattern, endpoint_handler_t handler);
static int32_t RouteDefault(request_t *request);
static int32_t RouteFileSys(request_t *request);

// ------------------------------------------------------
// Global variables
// ------------------------------------------------------
static QueueHandle_t requests_q;

#define MAX_ROUTES	16
static route_t routes[MAX_ROUTES];
static uint32_t num_routes = 0;

void StartHttpTask(void *argument)
{
	struct netconn *conn, *newconn;
	request_t request; 
	int32_t r;

	printf("[HttpTask] Started\r\n");
	
	// Initialize Request Handler tasks
	if((r = StartRequestHandlers()) < 0) {
		printf("[HttpTask] Failed to start request handler tasks: %d\r\n", (int)r);
		_Error_Handler(__FILE__, __LINE__);
	}

	// Setup incommong connections listener
	if((r = InitIncomingConn(&conn)) < 0) {
		printf("[HttpTask] Failed to create incomming connection: %d\r\n", (int)r);
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

int32_t StartRequestHandlers()
{
	requests_q = xQueueCreate(REQUESTS_QUEUE_LEN, sizeof(struct netconn *));
	if(requests_q == NULL) {
		return -1;
	}

	char task_name[16];
	for(int i = 0; i < NUMBER_HANDLER_THREADS; i++) {
		sprintf(task_name, "ReqHandler-%d", i);
		if(xTaskCreate(StartRequestHandlerTask, task_name, 2048, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
			return -2;
		}
	}

	RegisterRoutes();

	return 0;
}

int32_t InitIncomingConn(struct netconn **conn)
{
	*conn = netconn_new(NETCONN_TCP);
	if(*conn == NULL) {
		return -1;
	}

	if(netconn_bind(*conn, NULL, 80) != ERR_OK) {
		return -2;
	}

	if(netconn_listen(*conn) != ERR_OK) {
		return -3;
	}

	return 0;
}


void StartRequestHandlerTask(void *argument)
{
	struct netconn *conn;
	char *name = pcTaskGetName(NULL);

	printf("[%s] Started\r\n", name);

	if(requests_q == NULL) {
		printf("[%s] Requests Queue is NULL\r\n", name);
		_Error_Handler(__FILE__, __LINE__);
	}

	while(1) 
	{
		if(xQueueReceive(requests_q, &conn, portMAX_DELAY))
		{
			printf("[%s] Got request to process\r\n", name);
			if(conn != NULL) 
			{
				osDelay(50);
				process_connection(conn);
				netconn_close(conn);
				netconn_delete(conn);
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

	// Header parser
	const char *method, *path;
	int headers_len, minor_version;
	size_t buflen = 0, prevbuflen = 0, method_len, path_len, num_headers;

	// Request
	request_t req;


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
		headers_len = phr_parse_request(buf, buflen, &method, &method_len, &path, &path_len,
                             &minor_version, headers, &num_headers, prevbuflen);

		if(headers_len > 0)
		{
			printf("[%s] Buffer parsed OK\r\n", pcTaskGetName(NULL));
			break;
		}
    	else if(headers_len == -1) 
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

	printf("[%s] Header is %d bytes long\n", pcTaskGetName(NULL), headers_len);
	printf("[%s] Method is %.*s\n", pcTaskGetName(NULL), (int)method_len, method);
	printf("[%s] Path is %.*s\n", pcTaskGetName(NULL), (int)path_len, path);
	printf("[%s] HTTP version is 1.%d\n", pcTaskGetName(NULL), minor_version);
	printf("[%s] Headers:\n", pcTaskGetName(NULL));
	for(int i = 0; i != num_headers; ++i) 
	{
		printf("\t%.*s: %.*s\n", (int)headers[i].name_len, headers[i].name,
			(int)headers[i].value_len, headers[i].value);
	}
	
	req.conn = conn;
	req.inbuf = buf;
	req.headers = headers;
	req.headers_len = headers_len;
	req.num_headers = num_headers;
	memcpy(req.path, path, (size_t)path_len);
	req.path[path_len] = 0;

	// Find route, if no route found, pass to default route
	printf("[%s] Search for routes, for:%s\r\n",pcTaskGetName(NULL),req.path);
	route_t *route = NULL;
	for(int i = 0; i < num_routes; i++)
	{
		int match = re_match(routes[i].re_pattern, req.path);
		printf("[%s] (%d) Checking handler for [%s], match:%d\r\n",pcTaskGetName(NULL), i, req.path, match);
		if(match >= 0) {
			printf("[%s] Route handler found\r\n",pcTaskGetName(NULL));
			route = &routes[i];
			break;
		}
	}

	if(route != NULL) {
		route->handler(&req);
	}
	else {
		printf("[%s] No handler found, calling default route\r\n",pcTaskGetName(NULL));
		RouteDefault(&req);
	}
}

int32_t RegisterRoutes()
{
	RegisterRoute("/$", RouteFileSys);
	RegisterRoute("/api", RouteFileSys);
	RegisterRoute("/file.txt", RouteFileSys);

	return 0;
}


int32_t RegisterRoute(char *re_pattern, endpoint_handler_t handler)
{
	if(num_routes >= MAX_ROUTES) {
		return -1;
	}

	strncpy(routes[num_routes].re_pattern, re_pattern, 32 - 1);
	routes[num_routes].handler = handler;
	num_routes++;

	return 0;
}

int32_t RouteDefault(request_t *req)
{
	printf("[%s] Default route called\r\n", pcTaskGetName(NULL));
	printf("[%s] Path:%s\r\n", pcTaskGetName(NULL), req->path);

	netconn_write(req->conn, http_html_hdr, sizeof(http_html_hdr), NETCONN_NOCOPY);
	netconn_write(req->conn, indexdata, sizeof(indexdata), NETCONN_NOCOPY);


	return 0;
}

int32_t RouteFileSys(request_t *req)
{
	printf("[%s] RouteFileSys route called\r\n", pcTaskGetName(NULL));
	printf("[%s] Path:%s\r\n", pcTaskGetName(NULL), req->path);

	netconn_write(req->conn, http_html_hdr, sizeof(http_html_hdr), NETCONN_NOCOPY);
	netconn_write(req->conn, indexdata, sizeof(indexdata), NETCONN_NOCOPY);

	return 0;
}