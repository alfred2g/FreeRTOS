/*
 * FreeRTOS V202012.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */
/*! @file timers_utest.c */


/* Test includes. */
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "portmacro.h"
#include "timers.h"

#include "unity.h"
#include "unity_memory.h"

/* Mock includes. */
#include "mock_queue.h"
#include "mock_list.h"
#include "mock_list_macros.h"
#include "mock_fake_assert.h"
#include "mock_portable.h"
#include "mock_task.h"

/* C runtime includes. */
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

/* ======================  DEFINITIONS FROM timers.c ======================== */
#define tmrNO_DELAY    ( TickType_t ) 0U

#define tmrSTATUS_IS_ACTIVE                  ( ( uint8_t ) 0x01 )
#define tmrSTATUS_IS_STATICALLY_ALLOCATED    ( ( uint8_t ) 0x02 )
#define tmrSTATUS_IS_AUTORELOAD              ( ( uint8_t ) 0x04 )

#define tmrCOMMAND_EXECUTE_CALLBACK_FROM_ISR    ( ( BaseType_t ) -2 )
#define tmrCOMMAND_EXECUTE_CALLBACK             ( ( BaseType_t ) -1 )
#define tmrCOMMAND_START_DONT_TRACE             ( ( BaseType_t ) 0 )
#define tmrCOMMAND_START                        ( ( BaseType_t ) 1 )
#define tmrCOMMAND_RESET                        ( ( BaseType_t ) 2 )
#define tmrCOMMAND_STOP                         ( ( BaseType_t ) 3 )
#define tmrCOMMAND_CHANGE_PERIOD                ( ( BaseType_t ) 4 )
#define tmrCOMMAND_DELETE                       ( ( BaseType_t ) 5 )

#define tmrFIRST_FROM_ISR_COMMAND               ( ( BaseType_t ) 6 )
#define tmrCOMMAND_START_FROM_ISR               ( ( BaseType_t ) 6 )
#define tmrCOMMAND_RESET_FROM_ISR               ( ( BaseType_t ) 7 )
#define tmrCOMMAND_STOP_FROM_ISR                ( ( BaseType_t ) 8 )
#define tmrCOMMAND_CHANGE_PERIOD_FROM_ISR       ( ( BaseType_t ) 9 )

typedef struct tmrTimerControl                  /* The old naming convention is used to prevent breaking kernel aware debuggers. */
{
    const char * pcTimerName;                   /*<< Text name.  This is not used by the kernel, it is included simply to make debugging easier. */ /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
    ListItem_t xTimerListItem;                  /*<< Standard linked list item as used by all kernel features for event management. */
    TickType_t xTimerPeriodInTicks;             /*<< How quickly and often the timer expires. */
    void * pvTimerID;                           /*<< An ID to identify the timer.  This allows the timer to be identified when the same callback is used for multiple timers. */
    TimerCallbackFunction_t pxCallbackFunction; /*<< The function that will be called when the timer expires. */
    #if ( configUSE_TRACE_FACILITY == 1 )
        UBaseType_t uxTimerNumber;              /*<< An ID assigned by trace tools such as FreeRTOS+Trace */
    #endif
    uint8_t ucStatus;                           /*<< Holds bits to say if the timer was statically allocated or not, and if it is active or not. */
} xTIMER;

typedef xTIMER Timer_t;

typedef struct tmrTimerParameters
{
    TickType_t xMessageValue; /*<< An optional value used by a subset of commands, for example, when changing the period of a timer. */
    Timer_t * pxTimer;        /*<< The timer to which the command will be applied. */
} TimerParameter_t;

typedef struct tmrCallbackParameters
{
    PendedFunction_t pxCallbackFunction; /* << The callback function to execute. */
    void * pvParameter1;                 /* << The value that will be used as the callback functions first parameter. */
    uint32_t ulParameter2;               /* << The value that will be used as the callback functions second parameter. */
} CallbackParameters_t;

typedef struct tmrTimerQueueMessage
{
    BaseType_t xMessageID; /*<< The command being sent to the timer service task. */
    union
    {
        TimerParameter_t xTimerParameters;

        /* Don't include xCallbackParameters if it is not going to be used as
            * it makes the structure (and therefore the timer queue) larger. */
        #if ( INCLUDE_xTimerPendFunctionCall == 1 )
            CallbackParameters_t xCallbackParameters;
        #endif /* INCLUDE_xTimerPendFunctionCall */
    } u;
} DaemonTaskMessage_t;

/* ============================  GLOBAL VARIABLES =========================== */
static uint16_t usMallocFreeCalls = 0;
static uint32_t critical_section_counter;
static Timer_t  pxNewTimer;
static char task_memory[ 200 ];

static TickType_t saved_last_time = 0;
static bool port_yield_within_api_called = false;

/* =============================  FUNCTION HOOKS  =========================== */
void vFakePortEnterCriticalSection( void )
{
    critical_section_counter++;
}

void vFakePortExitCriticalSection( void )
{
    critical_section_counter--;
}

void vFakePortYieldWithinAPI()
{
 //   HOOK_DIAG();
    printf("fake port yield called\n");
    port_yield_within_api_called = true;
    pthread_exit(NULL);
 //   py_operation();
}

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationDaemonTaskStartupHook( void )
{
    printf("timer started\n");
}
static void xCallback_Test( TimerHandle_t xTimer )
{
}
/* ==========================  CALLBACK FUNCTIONS =========================== */

/*
void * pvPortMalloc( size_t xSize )
{
    return unity_malloc( xSize );
}
void vPortFree( void * pv )
{
    return unity_free( pv );
}
*/

/*******************************************************************************
 * Unity fixtures
 ******************************************************************************/
void stopTimers();
void setUp( void )
{
    vFakeAssert_Ignore();
    port_yield_within_api_called = false;
    /* Track calls to malloc / free */
    UnityMalloc_StartTest();
    critical_section_counter = 0;
    stopTimers();
}

/*! called before each testcase */
void tearDown( void )
{
    TEST_ASSERT_EQUAL_INT_MESSAGE( 0, usMallocFreeCalls,
                                   "free is not called the same number of times as malloc,"
                                   "you might have a memory leak!!" );
    usMallocFreeCalls = 0;

    UnityMalloc_EndTest();
}

/*! called at the beginning of the whole suite */
void suiteSetUp()
{
}

/*! called at the end of the whole suite */
int suiteTearDown( int numFailures )
{
    return numFailures;
}

TimerHandle_t create_timer()
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;
    QueueHandle_t queue_handle = ( QueueHandle_t ) 3; /* not zero */

    pvPortMalloc_ExpectAndReturn( sizeof( Timer_t ), &pxNewTimer );
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    vListInitialiseItem_ExpectAnyArgs();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );
    return xTimer;
}

void create_timer_task( void )
{
    BaseType_t ret_xtimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */
    TaskHandle_t timer_handle  = ( TaskHandle_t )task_memory;
    /* Setup */
    /* Expectations */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    xTaskCreateStatic_ExpectAnyArgsAndReturn( timer_handle );
    /* API Call */
    ret_xtimer = xTimerCreateTimerTask();
    /* Validations */
    TEST_ASSERT_TRUE( ret_xtimer );
}

/* ==============================  TEST FUNCTIONS  ========================== */

/**
 * @brief xTimerCreate happy path
 *
 */
void test_xTimerCreate_success( void )
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;
    Timer_t  pxNewTimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */

    pvPortMalloc_ExpectAndReturn( sizeof(Timer_t), &pxNewTimer );
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    vListInitialiseItem_ExpectAnyArgs();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_NOT_EQUAL( NULL, xTimer );
    TEST_ASSERT_EQUAL_PTR( &pxNewTimer, xTimer );
    TEST_ASSERT_EQUAL( tmrSTATUS_IS_AUTORELOAD , pxNewTimer.ucStatus );
    TEST_ASSERT_EQUAL_STRING( "ut-timer", pxNewTimer.pcTimerName );
    TEST_ASSERT_EQUAL( pdMS_TO_TICKS(1000), pxNewTimer.xTimerPeriodInTicks );
    TEST_ASSERT_EQUAL_PTR( &ulID, pxNewTimer.pvTimerID );
    TEST_ASSERT_EQUAL_PTR( xCallback_Test, pxNewTimer.pxCallbackFunction );
}

void test_xTimerCreate_success_no_auto_reload( void )
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;
    Timer_t  pxNewTimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */

    pvPortMalloc_ExpectAndReturn( sizeof(Timer_t), &pxNewTimer );
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    vListInitialiseItem_ExpectAnyArgs();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdFALSE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_EQUAL_PTR( &pxNewTimer, xTimer );
    TEST_ASSERT_EQUAL( 0, pxNewTimer.ucStatus );
}

void test_xTimerCreate_success_twice( void )
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;
    Timer_t  pxNewTimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */

    pvPortMalloc_ExpectAndReturn( sizeof(Timer_t), &pxNewTimer );
    /* prvInitialiseNewTimer */
    /* prvCheckForValidListAndQueue */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    /* back prvInitialiseNewTimer  */
    vListInitialiseItem_ExpectAnyArgs();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_EQUAL_PTR( &pxNewTimer, xTimer );
    TEST_ASSERT_EQUAL_PTR( &pxNewTimer, xTimer );
    TEST_ASSERT_EQUAL( tmrSTATUS_IS_AUTORELOAD , pxNewTimer.ucStatus );
    TEST_ASSERT_EQUAL_STRING( "ut-timer", pxNewTimer.pcTimerName );
    TEST_ASSERT_EQUAL( pdMS_TO_TICKS(1000), pxNewTimer.xTimerPeriodInTicks );
    TEST_ASSERT_EQUAL_PTR( &ulID, pxNewTimer.pvTimerID );
    TEST_ASSERT_EQUAL_PTR( xCallback_Test, pxNewTimer.pxCallbackFunction );

    /* Second call to xTimerCreate */
    pvPortMalloc_ExpectAndReturn( sizeof(Timer_t), &pxNewTimer );
    vListInitialiseItem_ExpectAnyArgs();
    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );
    TEST_ASSERT_EQUAL_PTR( &pxNewTimer, xTimer );
}

void test_xTimerCreate_fail_timer_allocation( void )
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;

    pvPortMalloc_ExpectAndReturn(sizeof(Timer_t), NULL);

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_EQUAL( NULL, xTimer );
}
void test_xTimerCreate_fail_queue_allocation( void )
{
    uint32_t ulID = 0;
    Timer_t  pxNewTimer;
    TimerHandle_t xTimer = NULL;

    /* Expectations */
    pvPortMalloc_ExpectAndReturn(sizeof(Timer_t), &pxNewTimer);
    /* prvInitialiseNewTimer */
    /* prvCheckForValidListAndQueue */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn(NULL);
    /* Back prvInitialiseNewTimer */
    vListInitialiseItem_ExpectAnyArgs();

    /* API Call */
    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );
    /* Validations */
    TEST_ASSERT_EQUAL( &pxNewTimer, xTimer );
}

void test_xTimerCreateTimerTask_success( void )
{
    BaseType_t ret_xtimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */
    char task_memory[ 200 ];
    TaskHandle_t timer_handle  = ( TaskHandle_t )task_memory;
    /* Setup */
    /* Expectations */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    xTaskCreateStatic_ExpectAnyArgsAndReturn( timer_handle );
    /* API Call */
    ret_xtimer = xTimerCreateTimerTask();
    /* Validations */
    TEST_ASSERT_TRUE( ret_xtimer );
}

void test_xTimerCreateTimerTask_fail_null_task( void )
{
    BaseType_t ret_xtimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */
    /* Setup */
    /* Expectations */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_ExpectAnyArgs();
    xTaskCreateStatic_ExpectAnyArgsAndReturn( NULL );
    /* API Call */
    ret_xtimer = xTimerCreateTimerTask();
    /* Validations */
    TEST_ASSERT_FALSE( ret_xtimer );
}

void test_xTimerCreateTimerTask_fail_null_queue( void )
{
    BaseType_t ret_xtimer;
    /* Setup */
    /* Expectations */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( NULL );
    /* API Call */
    ret_xtimer = xTimerCreateTimerTask();
    /* Validations */
    TEST_ASSERT_FALSE( ret_xtimer );
}

void test_xTimerCreateStatic_success( void )
{
    TimerHandle_t  ret_timer_create;
    UBaseType_t  pvTimerID;
    StaticTimer_t pxTimerBuffer[ sizeof( StaticTimer_t ) ];
    /* Setup */
    /* Expectations */
    /* prvInitialiseNewTimer */
    /* prvCheckForValidListAndQueue */
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn(NULL);
    /* Back prvInitialiseNewTimer */
    vListInitialiseItem_ExpectAnyArgs();
    /* API Call */
    ret_timer_create = xTimerCreateStatic( "ut_timer_task",
                                           pdMS_TO_TICKS( 1000 ),
					   pdTRUE,
					   ( void * ) &pvTimerID,
					   xCallback_Test,
					   pxTimerBuffer );
    /* Validations */
    TEST_ASSERT_TRUE( ret_timer_create );
}

void test_xTimerCreateStatic_fail_null_buffer( void )
{
    TimerHandle_t  ret_timer_create;
    UBaseType_t  pvTimerID;
    /* Setup */
    /* Expectations */
    /* prvInitialiseNewTimer */
    /* prvCheckForValidListAndQueue */
    /* API Call */
    ret_timer_create = xTimerCreateStatic( "ut_timer_task",
                                           pdMS_TO_TICKS( 1000 ),
					   pdTRUE,
					   ( void * ) &pvTimerID,
					   xCallback_Test,
					   NULL );
    /* Validations */
    TEST_ASSERT_FALSE( ret_timer_create );
}


void test_xTimerGenericCommand_success_queue_pass( void )
{
    BaseType_t ret_timer_generic;
    TimerHandle_t xTimer;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
    const TickType_t xTicksToWait = 400;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */

    xQueueGenericSendFromISR_ExpectAnyArgsAndReturn( pdPASS );
    /* API Call */
    ret_timer_generic = xTimerGenericCommand( xTimer,
                                              tmrFIRST_FROM_ISR_COMMAND,
                                              34,
                                              &pxHigherPriorityTaskWoken,
                                              xTicksToWait );
    /* Validations */
    TEST_ASSERT_TRUE( ret_timer_generic );
}

void test_xTimerGenericCommand_fail_queue_fail( void )
{
    BaseType_t ret_timer_generic;
    TimerHandle_t xTimer;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
    const TickType_t xTicksToWait = 400;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */

    xQueueGenericSendFromISR_ExpectAnyArgsAndReturn( pdFAIL );
    /* API Call */
    ret_timer_generic = xTimerGenericCommand( xTimer,
                                              tmrFIRST_FROM_ISR_COMMAND,
                                              34,
                                              &pxHigherPriorityTaskWoken,
                                              xTicksToWait );
    /* Validations */
    TEST_ASSERT_FALSE( ret_timer_generic );
}

void test_xTimerGenericCommand_success_sched_running( void )
{
    BaseType_t ret_timer_generic;
    TimerHandle_t xTimer;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
    const TickType_t xTicksToWait = 400;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    xTaskGetSchedulerState_ExpectAndReturn( taskSCHEDULER_RUNNING );
    xQueueGenericSend_ExpectAnyArgsAndReturn ( pdPASS );
    /* API Call */
    ret_timer_generic = xTimerGenericCommand( xTimer,
                                              tmrCOMMAND_START,
                                              34,
                                              &pxHigherPriorityTaskWoken,
                                              xTicksToWait );
    /* Validations */
    TEST_ASSERT_TRUE( ret_timer_generic );
}

void test_xTimerGenericCommand_success_sched_not_running( void )
{
    BaseType_t ret_timer_generic;
    TimerHandle_t xTimer;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
    const TickType_t xTicksToWait = 400;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    xTaskGetSchedulerState_ExpectAndReturn( taskSCHEDULER_NOT_STARTED );
    xQueueGenericSend_ExpectAnyArgsAndReturn ( pdPASS );

    /* API Call */
    ret_timer_generic = xTimerGenericCommand( xTimer,
                                              tmrCOMMAND_START,
                                              34,
                                              &pxHigherPriorityTaskWoken,
                                              xTicksToWait );
    /* Validations */
    TEST_ASSERT_TRUE( ret_timer_generic );
}

void test_xTimerGenericCommand_success_null_timer_not_started( void )
{
    BaseType_t ret_timer_generic;
    TimerHandle_t xTimer = NULL;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
    const TickType_t xTicksToWait = 400;

    /* Setup */
    /* Expectations */
    /* API Call */
    ret_timer_generic = xTimerGenericCommand( xTimer,
                                              tmrCOMMAND_START,
                                              34,
                                              &pxHigherPriorityTaskWoken,
                                              xTicksToWait );
    /* Validations */
    TEST_ASSERT_FALSE( ret_timer_generic );
}

void test_xTimerGetTimerDaemonTaskHandle_success( void )
{
    TaskHandle_t  ret_get_timer_handle;
    /* Setup */
    create_timer_task();
    /* Expectations */
    /* API Call */
    ret_get_timer_handle = xTimerGetTimerDaemonTaskHandle( );
    /* Validations */
    TEST_ASSERT_NOT_NULL( ret_get_timer_handle );
}

void test_xTimerGetPeriod_success( void )
{
    TickType_t  ret_get_period;
    TimerHandle_t  xTimer;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    /* API Call */
    ret_get_period = xTimerGetPeriod( xTimer );
    /* Validations */
    TEST_ASSERT_EQUAL( xTimer->xTimerPeriodInTicks, ret_get_period );
}

void test_vTimer_Set_Get_ReloadMode_success( void )
{
    TimerHandle_t  xTimer;
    UBaseType_t  reload_mode;
    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    /* API Call */
    vTimerSetReloadMode( xTimer, pdTRUE );
    reload_mode =  uxTimerGetReloadMode( xTimer );
    /* Validations */
    TEST_ASSERT_TRUE( (xTimer->ucStatus & tmrSTATUS_IS_AUTORELOAD) );
    TEST_ASSERT_TRUE( reload_mode);

    /* API Call */
    vTimerSetReloadMode( xTimer, pdFALSE );
    reload_mode =  uxTimerGetReloadMode( xTimer );
    /* Validations */
    TEST_ASSERT_FALSE( xTimer->ucStatus & tmrSTATUS_IS_AUTORELOAD );
    TEST_ASSERT_FALSE( reload_mode);
}


void test_xTimerGetExpiryTime( void )
{
    TickType_t  ret_timer_expiry;
    TimerHandle_t  xTimer;
    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    listGET_LIST_ITEM_VALUE_ExpectAnyArgsAndReturn(35);
    /* API Call */
    ret_timer_expiry = xTimerGetExpiryTime( xTimer );
    /* Validations */
    TEST_ASSERT_EQUAL( 35, ret_timer_expiry );
}

void test_pcTimerGetName( void )
{
    TimerHandle_t  xTimer;
    const char * ret_timer_name;
    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    /* API Call */
    ret_timer_name =  pcTimerGetName( xTimer );
    /* Validations */
    TEST_ASSERT_EQUAL_STRING("ut-timer", ret_timer_name);
}

void test_xTimerIsTimerActive_true( void )
{
    BaseType_t  ret_is_timer_active;

    TimerHandle_t  xTimer;
    /* Setup */
    xTimer = create_timer();
    xTimer->ucStatus |= tmrSTATUS_IS_ACTIVE;
    /* Expectations */
    /* API Call */
    ret_is_timer_active = xTimerIsTimerActive( xTimer );
    /* Validations */
    TEST_ASSERT_TRUE( ret_is_timer_active );
}

void test_xTimerIsTimerActive_false( void )
{
    BaseType_t  ret_is_timer_active;
    TimerHandle_t  xTimer;
    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    /* API Call */
    ret_is_timer_active = xTimerIsTimerActive( xTimer );
    /* Validations */
    TEST_ASSERT_FALSE( ret_is_timer_active );
}

void test_vTimerSetTimerID( void )
{
    TimerHandle_t  xTimer;
    UBaseType_t  pvNewID = 45;
    UBaseType_t  *saved_pvNewID;

    /* Setup */
    xTimer = create_timer();
    /* Expectations */
    /* API Call */
    vTimerSetTimerID( xTimer, (void *) & pvNewID );
    /* Validations */
    TEST_ASSERT_EQUAL( pvNewID, ( * ( UBaseType_t * )xTimer->pvTimerID ) );

    saved_pvNewID = pvTimerGetTimerID( xTimer );
    TEST_ASSERT_EQUAL( pvNewID, *saved_pvNewID );
}

typedef void (* PendedFunction_t)( void *,
                                   uint32_t );

static void pended_function( void * arg1, uint32_t arg2 )
{
}
void test_xTimerPendFunctionCall_success( void )
{
    BaseType_t  ret_timer_pend;
    UBaseType_t  pvParameter1 = 0xb0b0b0;
    uint32_t  ulParameter2 = 0xa0a0a0;
    /* Setup */
    /* Expectations */
    xQueueGenericSend_ExpectAnyArgsAndReturn(pdTRUE);
    /* API Call */
    ret_timer_pend = xTimerPendFunctionCall( pended_function,
                                             (void *) &pvParameter1,
                                             ulParameter2,
                                             500);
    /* Validations */
    TEST_ASSERT_EQUAL( pdTRUE, ret_timer_pend );
}

void test_xTimerPendFunctionCallFromISR_success( void )
{
    BaseType_t  ret_timer_pend;
    UBaseType_t  pvParameter1 = 0xb0b0b0;
    uint32_t  ulParameter2 = 0xa0a0a0;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    /* Setup */
    /* Expectations */
    xQueueGenericSendFromISR_ExpectAnyArgsAndReturn(pdTRUE);
    /* API Call */
    ret_timer_pend = xTimerPendFunctionCallFromISR( pended_function,
                                                    (void *) &pvParameter1,
                                                    ulParameter2,
                                                    &xHigherPriorityTaskWoken);
    /* Validations */
    TEST_ASSERT_EQUAL( pdTRUE, ret_timer_pend );
}

static void * timer_thread_function( void * args)
{
    void * pvParameters = NULL;
    portTASK_FUNCTION( prvTimerTask, pvParameters );
    (void) fool_static2; /* ignore unused variable warning */
    /* API Call */
    prvTimerTask(pvParameters);
    return NULL;
}
static int32_t end_4_timer = 0;
static void pended_function_4_end( void * arg1, uint32_t arg2 )
{
    printf("end 4 timer called\n");
    static int i = 4;
    if ( end_4_timer -1 <= 0 )
    {
        pthread_exit(&i);
    }
    end_4_timer--;
}

static int32_t end_1_timer = 0;
static void xCallback_Test_1_end( TimerHandle_t xTimer )
{
    printf("end 1 timer called\n");
    static int i = 1;
    if (end_1_timer - 1 <= 0)
    {
        pthread_exit(&i);
    }
    end_1_timer--;
}

static int32_t end_2_timer = 0;
static void xCallback_Test_2_end( TimerHandle_t xTimer )
{
    printf("xCallback_Test_2_end called \n");
    static int i = 2;
    if (end_2_timer - 1 <= 0)
    {
        pthread_exit(&i);
    }
    end_2_timer--;
}
/*
static void xCallback_Test_3_end( TimerHandle_t xTimer )
{
    static int i = 3;
    pthread_exit(&i);
}
*/

void test_timer_function_success(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    /* Setup */
    end_1_timer = 1;
    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.xTimerPeriodInTicks = 0;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */

    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    /* prvProcessReceivedCommands */

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(1, *retVal);
}

void test_timer_function_success3(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    /* Setup */
    end_1_timer = 2;
    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.xTimerPeriodInTicks = 0;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    //listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    /* back to prvProcessTimerOrBlockTask */
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn( pdFALSE );
    // yield called

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */

    TEST_ASSERT_TRUE( port_yield_within_api_called );
}

void test_timer_function_success4(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    CallbackParameters_t callback_param;
    /* Setup */
    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    end_1_timer = 2;
    end_4_timer = 2;
    DaemonTaskMessage_t xMessage;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time + 300;

    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.xTimerPeriodInTicks = 20;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    //listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* back to prvProcessTimerOrBlockTask */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* yield called */
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn( pdFALSE );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 50 );
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );
    /* callback called */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    /* Validations */
    TEST_ASSERT_EQUAL(1, *retVal);
}

void test_timer_function_success5(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    CallbackParameters_t callback_param;
    /* Setup */
    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    end_1_timer = 2;

    DaemonTaskMessage_t xMessage;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;

    end_4_timer = 2;
    DaemonTaskMessage_t xMessage2;
    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK_FROM_ISR;
    xMessage2.u.xCallbackParameters = callback_param;
    xMessage2.u.xTimerParameters.pxTimer = &xTimer;
    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.xTimerPeriodInTicks = 0;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    //listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    /* back to prvProcessTimerOrBlockTask */
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* yield called */
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2,
                                             sizeof (DaemonTaskMessage_t) );
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAnyArgsAndReturn(pdFAIL);
    /* back prvTimerTask */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( saved_last_time  + 1 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back prvProcessTimerOrBlockTask */
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn(pdFALSE);

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    //TEST_ASSERT_EQUAL(1, *retVal);
    TEST_ASSERT_TRUE( port_yield_within_api_called );
}

void test_timer_function_success6(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    CallbackParameters_t callback_param;
    /* Setup */
    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    end_1_timer = 2;
    DaemonTaskMessage_t xMessage;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.xTimerPeriodInTicks = 0;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    //listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    /* back to prvProcessTimerOrBlockTask */
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* yield called */
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAnyArgsAndReturn(pdFAIL);
    /* back prvTimerTask */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( saved_last_time  + 1 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time  - 5); /* time now / static last_time = 0 */
    saved_last_time -= 5;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdTRUE);
    /* back prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn(pdFALSE);
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    //vTaskSuspendAll_Expect();

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(1, *retVal);
}
void test_timer_function_success2(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    DaemonTaskMessage_t xMessage;
    CallbackParameters_t callback_param;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;
    xTimer.pxCallbackFunction = xCallback_Test;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    xMessage.xMessageID = -1;
    xMessage.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
}
void test_timer_function_success3_command_start(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    DaemonTaskMessage_t xMessage;
    CallbackParameters_t callback_param;
    end_2_timer = 2;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = 0;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 100 ); /* time now / static last_time = 0 */
    saved_last_time += 100;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(2, *retVal);
}

void test_timer_function_success3_command_start2(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    DaemonTaskMessage_t xMessage;
    CallbackParameters_t callback_param;
    end_2_timer = 2;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = 0;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 100 ); /* time now / static last_time = 0 */
    saved_last_time += 100;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdFALSE);
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(600);
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(&xTimer);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvInsertInActiveList */
    //listSET_LIST_ITEM_VALUE_ExpectAnyArgs();

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(2, *retVal);
}

void test_timer_function_success3_command_start3(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    DaemonTaskMessage_t xMessage;
    CallbackParameters_t callback_param;
    end_2_timer = 3;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = 0;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 100 ); /* time now / static last_time = 0 */
    saved_last_time += 100;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdFALSE);
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(600);
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(&xTimer);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(2, *retVal);
}

void test_timer_function_success3_command_start4(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    DaemonTaskMessage_t xMessage;
    CallbackParameters_t callback_param;
    end_2_timer = 3;

    /* Setup */
    xTimer.ucStatus &= ~tmrSTATUS_IS_AUTORELOAD;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = 0;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 100 ); /* time now / static last_time = 0 */
    saved_last_time += 100;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    //listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn(pdFALSE);
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(600);
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn(&xTimer);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvInsertInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    //prvTimerTask(pvParameters);
    /* Validations */
    TEST_ASSERT_EQUAL(2, *retVal);
}

void test_timer_function_success3_command_start5(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time + 50;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time - 500;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK;
    xMessage2.u.xCallbackParameters = callback_param;
    //xMessage2.u.xTimerParameters.pxTimer = &xTimer2;
    //xMessage2.u.xTimerParameters.xMessageValue = saved_last_time - 500;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 1000 ); /* time now / static last_time = 0 */
    saved_last_time += 1000;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 5000 ); /* time now / static last_time = 0 */
    saved_last_time += 5000;
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
}

void test_timer_function_success3_command_stop(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.ucStatus |= tmrSTATUS_IS_ACTIVE;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time + 50;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_STOP;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time - 500;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK; /* used to end the loop */
    xMessage2.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 1000 ); /* time now / static last_time = 0 */
    saved_last_time += 1000;
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 5000 ); /* time now / static last_time = 0 */
    saved_last_time += 5000;
    /* prvInsertTimerInActiveList */
    //listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    //vListInsert_ExpectAnyArgs();
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
    printf("xTimer %p\n", &xTimer);
    printf("status %d\n", (xTimer.ucStatus & tmrSTATUS_IS_ACTIVE ) );
    TEST_ASSERT_FALSE(xTimer.ucStatus & tmrSTATUS_IS_ACTIVE);
}

void test_timer_function_success3_command_change_period(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.ucStatus &= ~tmrSTATUS_IS_ACTIVE;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_CHANGE_PERIOD;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK; /* used to end the loop */
    xMessage2.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
    TEST_ASSERT_TRUE(xTimer.ucStatus & tmrSTATUS_IS_ACTIVE);
    TEST_ASSERT_EQUAL(saved_last_time, xTimer.xTimerPeriodInTicks);
}

void test_timer_function_success3_command_delete_static(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.ucStatus |= tmrSTATUS_IS_STATICALLY_ALLOCATED;
    xTimer.ucStatus &= ~tmrSTATUS_IS_ACTIVE;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_DELETE;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK; /* used to end the loop */
    xMessage2.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
    TEST_ASSERT_FALSE(xTimer.ucStatus & tmrSTATUS_IS_ACTIVE);
}

void test_timer_function_success3_command_delete_dynamic(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.ucStatus &= ~tmrSTATUS_IS_STATICALLY_ALLOCATED;
    xTimer.ucStatus &= ~tmrSTATUS_IS_ACTIVE;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_DELETE;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK; /* used to end the loop */
    xMessage2.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    vPortFree_Expect( &xTimer ); /* testcase is testing this clause */
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
}

void test_timer_function_success3_command_unknown(void)
{
    Timer_t xTimer = { 0 };
    Timer_t xTimer2 = { 0 };
    pthread_t thread_id;
    int * retVal;

    DaemonTaskMessage_t xMessage;
    DaemonTaskMessage_t xMessage2;
    CallbackParameters_t callback_param;
    end_2_timer = 2;
    end_4_timer = 1;

    /* Setup */
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer.ucStatus &= ~tmrSTATUS_IS_STATICALLY_ALLOCATED;
    xTimer.ucStatus &= ~tmrSTATUS_IS_ACTIVE;
    xTimer.pxCallbackFunction = xCallback_Test_2_end;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;

    xTimer2.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    xTimer2.pxCallbackFunction = xCallback_Test_2_end;
    xTimer2.xTimerPeriodInTicks = saved_last_time;

    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;

    xMessage.xMessageID = tmrCOMMAND_CHANGE_PERIOD_FROM_ISR  + 1   ;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time;

    xMessage2.xMessageID = tmrCOMMAND_EXECUTE_CALLBACK; /* used to end the loop */
    xMessage2.u.xCallbackParameters = callback_param;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessTimerOrBlockTask */
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* prvProcessExpiredTimer */
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAndReturn( &xTimer.xTimerListItem, pdTRUE );
    /* prvInsertTimerInActiveList */
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage, sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn(pdFALSE);
    uxListRemove_ExpectAnyArgsAndReturn(pdTRUE);
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time ); /* time now / static last_time = 0 */
    /* back to prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage2, sizeof (DaemonTaskMessage_t) );
    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, ( void ** )&retVal);
    /* Validations */
    TEST_ASSERT_EQUAL(4, *retVal);
}

void test_timer_function_success_wrap_timer(void)
{
    Timer_t xTimer = { 0 };
    pthread_t thread_id;
    int * retVal;
    CallbackParameters_t callback_param;
    /* Setup */
    callback_param.pxCallbackFunction = &pended_function_4_end;
    callback_param.pvParameter1 = NULL;
    callback_param.ulParameter2 = 0xa9a9a9a9;
    end_1_timer = 2;
    end_4_timer = 2;
    DaemonTaskMessage_t xMessage;

    xMessage.xMessageID = tmrCOMMAND_START;
    xMessage.u.xCallbackParameters = callback_param;
    xMessage.u.xTimerParameters.pxTimer = &xTimer;
    xMessage.u.xTimerParameters.xMessageValue = saved_last_time + 600;

    xTimer.ucStatus |= tmrCOMMAND_STOP;
    xTimer.xTimerPeriodInTicks = UINT32_MAX;
    xTimer.pxCallbackFunction = xCallback_Test_1_end;
    xTimer.ucStatus |= tmrSTATUS_IS_AUTORELOAD;
    /* Expectations */
    /* prvGetNextExpireTime */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    //listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 3 );
    /* prvProcessTimerOrBlockTask */
    vTaskSuspendAll_Expect();
    /* prvSampleTimeNow */
    xTaskGetTickCount_ExpectAndReturn( saved_last_time + 500 ); /* time now / static last_time = 0 */
    saved_last_time += 500;
    /* back to prvProcessTimerOrBlockTask */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    vQueueWaitForMessageRestricted_ExpectAnyArgs();
    xTaskResumeAll_ExpectAndReturn( pdTRUE );
    /* yield called */
    /* prvProcessReceivedCommands */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn( pdFALSE );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 50 );
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );
    /* callback called */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdTRUE );
    listSET_LIST_ITEM_VALUE_ExpectAnyArgs();
    vListInsert_ExpectAnyArgs();
    /* prvInsertInActiveList */
    /* back  prvInsertTimerInActiveList */
    xQueueReceive_ExpectAndReturn(NULL, NULL, tmrNO_DELAY, pdPASS);
    xQueueReceive_IgnoreArg_xQueue();
    xQueueReceive_IgnoreArg_pvBuffer();
    xQueueReceive_ReturnMemThruPtr_pvBuffer( &xMessage,
                                             sizeof (DaemonTaskMessage_t) );
    listIS_CONTAINED_WITHIN_ExpectAnyArgsAndReturn( pdFALSE );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );
    /* prvSampleTimeNow*/
    xTaskGetTickCount_ExpectAndReturn( saved_last_time - 50 ); /* time now / static last_time = 0 */
    saved_last_time -= 50;
    /* prvSwitchTimerLists */
    listLIST_IS_EMPTY_ExpectAnyArgsAndReturn( pdFALSE );
    listGET_ITEM_VALUE_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( 50 );
    listGET_OWNER_OF_HEAD_ENTRY_ExpectAnyArgsAndReturn( &xTimer );
    uxListRemove_ExpectAnyArgsAndReturn( pdTRUE );

    /* API Call */
    pthread_create( &thread_id, NULL,  &timer_thread_function, NULL );
    pthread_join( thread_id, (void**)&retVal);
    printf("thread joined \n");
    /* Validations */
    TEST_ASSERT_EQUAL(1, *retVal);
}
