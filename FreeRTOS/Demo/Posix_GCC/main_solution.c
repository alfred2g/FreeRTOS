#include <stdio.h>
#include <pthread.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"


#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

/* Local includes. */
#include "console.h"

int solution();
void vApplicationTickHook( void );
const char * pcApplicationHostnameHook( void );
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize );
int main()
    {
        solution();
        return 0;
    }
static void prvTaskOne( void *pvParameters );


const uint8_t ucMACAddress[ 6 ] = { configMAC_ADDR0, configMAC_ADDR1, configMAC_ADDR2, configMAC_ADDR3, configMAC_ADDR4, configMAC_ADDR5 };

static uint8_t MACAddress[ 6 ] = {  configMAC_ADDR0,  configMAC_ADDR1,  configMAC_ADDR2,  configMAC_ADDR3,  configMAC_ADDR4, configMAC_ADDR5 };
static const uint8_t IPAddress[ 4 ] = { 172, 19, 195, 37 };
static const uint8_t NetMask[ 4 ] = { 255, 255, 240, 0 };
//static const uint8_t GatewayAddress[ 4 ] = { 192, 168, 2, 1 };
static const uint8_t GatewayAddress[ 4 ] = { 172, 19, 192, 1 };

//static const uint8_t DNSServerAddress[ 4 ] = { 194, 14, 11, 200 };
static const uint8_t DNSServerAddress[ 4 ] = { 200, 11, 14, 194 };



int solution()
{

    console_init();
    FreeRTOS_IPInit( IPAddress,
                     NetMask,
                     GatewayAddress,
                     DNSServerAddress,
                     MACAddress );


    vTaskStartScheduler();
    return 0;
}

static void prvTaskOne(void *pvParameters)
{
    xSocket_t xSocket;

    console_print("Criando primeira task \n");
    xSocket = FreeRTOS_socket( FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP );
    configASSERT(xSocket != FREERTOS_INVALID_SOCKET);
    console_print("Socket criado");

    uint8_t ucBuffer[ 128 ];
    struct freertos_sockaddr xDestinationAddress;
    int32_t iReturned;

    /* Fill in the destination address and port number, which in this case is
    port 1024 on IP address 192.168.0.100. */
    xDestinationAddress.sin_addr = FreeRTOS_inet_addr_quick(172,19,195,36);
    xDestinationAddress.sin_port = FreeRTOS_htons( 5000 );

    /* The local buffer is filled with the data to be sent, in this case it is
    just filled with 0xff. */
    memset( ucBuffer, 0xff, 128 );

    /* Send the buffer with ulFlags set to 0, so the FREERTOS_ZERO_COPY bit
    is clear. */
    iReturned = FreeRTOS_sendto(
                                    /* The socket being send to. */
                                    xSocket,
                                    /* The data being sent. */
                                    ucBuffer,
                                    /* The length of the data being sent. */
                                    128,
                                    /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */
                                    0,
                                    /* Where the data is being sent. */
                                    &xDestinationAddress,
                                    /* Not used but should be set as shown. */
                                    sizeof( xDestinationAddress )
                               );

    if( iReturned == 128 )
    {
        /* The data was successfully queued for sending.  128 bytes will have
        been copied out of ucBuffer and into a buffer inside the IP stack.
        ucBuffer can be re-used now. */
        console_print("Dado enviado com sucesso! \n");
    }
    for(;;);
}

void vApplicationIPNetworkEventHook( eIPCallbackEvent_t eNetworkEvent )
{
uint32_t ulIPAddress, ulNetMask, ulGatewayAddress, ulDNSServerAddress;
char cBuffer[ 16 ];
static BaseType_t xTasksAlreadyCreated = pdFALSE;

    /* If the network has just come up...*/
    if( eNetworkEvent == eNetworkUp )
    {
        /* Create the tasks that use the IP stack if they have not already been
        created. */
        if( xTasksAlreadyCreated == pdFALSE )
        {
            /* See the comments above the definitions of these pre-processor
            macros at the top of this file for a description of the individual
            demo tasks. */

                xTaskCreate( prvTaskOne,            /* The function that implements the task. */
                    "Rx",                           /* The text name assigned to the task - for debug only as it is not used by the kernel. */
                    configMINIMAL_STACK_SIZE*30,        /* The size of the stack to allocate to the task. */
                    NULL,                           /* The parameter passed to the task - not used in this simple case. */
                    5,/* The priority assigned to the task. */
                    NULL );
            xTasksAlreadyCreated = pdTRUE;
        }

        /* Print out the network configuration, which may have come from a DHCP
        server. */
        FreeRTOS_GetAddressConfiguration( &ulIPAddress, &ulNetMask, &ulGatewayAddress, &ulDNSServerAddress );
        FreeRTOS_inet_ntoa( ulIPAddress, cBuffer );
      FreeRTOS_printf( ( "\r\n\r\nIP Address: %s\r\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulNetMask, cBuffer );
      FreeRTOS_printf( ( "Subnet Mask: %s\r\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulGatewayAddress, cBuffer );
      FreeRTOS_printf( ( "Gateway Address: %s\r\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulDNSServerAddress, cBuffer );
      FreeRTOS_printf( ( "DNS Server Address: %s\r\n\r\n\r\n", cBuffer ) );
    }
    else
    {
      FreeRTOS_printf( "Application idle hook network down\n" );
    }
}

static BaseType_t xTraceRunning = 1;
void vAssertCalled( const char * const pcFileName,
					unsigned long ulLine )
{
static BaseType_t xPrinted = pdFALSE;
volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;

	/* Called if an assertion passed to configASSERT() fails.  See
	http://www.freertos.org/a00110.html#configASSERT for more information. */

	/* Parameters are not used. */
	( void ) ulLine;
	( void ) pcFileName;


	taskENTER_CRITICAL();
	{
		/* Stop the trace recording. */
		if( xPrinted == pdFALSE )
		{
			xPrinted = pdTRUE;

			if( xTraceRunning == pdTRUE )
			{
//				prvSaveTraceFile();
			}
		}

		/* You can step out of this function to debug the assertion by using
		the debugger to set ulSetToNonZeroInDebuggerToContinue to a non-zero
		value. */
		while( ulSetToNonZeroInDebuggerToContinue == 0 )
		{
			__asm volatile ( "NOP" );
			__asm volatile ( "NOP" );
		}
	}
	taskEXIT_CRITICAL();
}

void vApplicationTickHook( void )
{
	/* This function will be called by each tick interrupt if
	configUSE_TICK_HOOK is set to 1 in FreeRTOSConfig.h.  User code can be
	added here, but the tick hook is called from an interrupt context, so
	code must not attempt to block, and only the interrupt safe FreeRTOS API
	functions can be used (those that end in FromISR()). */

	#if (mainSELECTED_APPLICATION == FULL_DEMO )
	{
		//vFullDemoTickHookFunction();
	}
	#endif /* mainSELECTED_APPLICATION */
}

static UBaseType_t ulNextRand;
UBaseType_t uxRand( void )
{
const uint32_t ulMultiplier = 0x015a4e35UL, ulIncrement = 1UL;

	/* Utility function to generate a pseudo random number. */

	ulNextRand = ( ulMultiplier * ulNextRand ) + ulIncrement;
	return( ( int ) ( ulNextRand >> 16UL ) & 0x7fffUL );
}

#define mainDEVICE_NICK_NAME	"linux_demo"
#define mainHOST_NAME		  "RTOSDemo"
BaseType_t xApplicationGetRandomNumber( uint32_t * pulNumber )
{
	*( pulNumber ) = uxRand();
	return pdTRUE;
}

#if ( ipconfigUSE_LLMNR != 0 ) || ( ipconfigUSE_NBNS != 0 )

	BaseType_t xApplicationDNSQueryHook( const char *pcName )
	{
	BaseType_t xReturn;

		/* Determine if a name lookup is for this node.  Two names are given
		to this node: that returned by pcApplicationHostnameHook() and that set
		by mainDEVICE_NICK_NAME. */
		if( strcasecmp( pcName, pcApplicationHostnameHook() ) == 0 )
		{
			xReturn = pdPASS;
		}
		else if( strcasecmp( pcName, mainDEVICE_NICK_NAME ) == 0 )
		{
			xReturn = pdPASS;
		}
		else
		{
			xReturn = pdFAIL;
		}

		return xReturn;
	}

#endif /* if ( ipconfigUSE_LLMNR != 0 ) || ( ipconfigUSE_NBNS != 0 ) */

#if ( ipconfigUSE_LLMNR != 0 ) || ( ipconfigUSE_NBNS != 0 ) || ( ipconfigDHCP_REGISTER_HOSTNAME == 1 )

	const char * pcApplicationHostnameHook( void )
	{
		/* Assign the name "FreeRTOS" to this network node.  This function will
		be called during the DHCP: the machine will be registered with an IP
		address plus this name. */
		return mainHOST_NAME;
	}

#endif

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize )
{
    int array[50];
    array[60] =  0;
/* If the buffers to be provided to the Idle task are declared inside this
function then they must be declared static - otherwise they will be allocated on
the stack and so not exists after this function exits. */
	static StaticTask_t xIdleTaskTCB;
	static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

	/* Pass out a pointer to the StaticTask_t structure in which the Idle task's
	state will be stored. */
	*ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

	/* Pass out the array that will be used as the Idle task's stack. */
	*ppxIdleTaskStackBuffer = uxIdleTaskStack;

	/* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
	Note that, as the array is necessarily of type StackType_t,
	configMINIMAL_STACK_SIZE is specified in words, not bytes. */
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

extern uint32_t ulApplicationGetNextSequenceNumber( uint32_t ulSourceAddress,
													uint16_t usSourcePort,
													uint32_t ulDestinationAddress,
													uint16_t usDestinationPort )
{
	( void ) ulSourceAddress;
	( void ) usSourcePort;
	( void ) ulDestinationAddress;
	( void ) usDestinationPort;

	return uxRand();
}

StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer,
									 StackType_t **ppxTimerTaskStackBuffer,
									 uint32_t *pulTimerTaskStackSize )
{
/* If the buffers to be provided to the Timer task are declared inside this
function then they must be declared static - otherwise they will be allocated on
the stack and so not exists after this function exits. */
	static StaticTask_t xTimerTaskTCB;

	/* Pass out a pointer to the StaticTask_t structure in which the Timer
	task's state will be stored. */
	*ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

	/* Pass out the array that will be used as the Timer task's stack. */
	*ppxTimerTaskStackBuffer = uxTimerTaskStack;

	/* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
	Note that, as the array is necessarily of type StackType_t,
	configMINIMAL_STACK_SIZE is specified in words, not bytes. */
	*pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationDaemonTaskStartupHook( void )
{
	/* This function will be called once only, when the daemon task starts to
	execute	(sometimes called the timer task).  This is useful if the
	application includes initialisation code that would benefit from executing
	after the scheduler has been started. */
}
