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
#include "timers.h"
#include "unity.h"
#include "unity_memory.h"

/* Mock includes. */
#include "mock_queue.h"
#include "mock_list.h"
#include "mock_fake_assert.h"
#include "mock_portable.h"

/* C runtime includes. */
#include <stdlib.h>
#include <stdbool.h>

/* ======================  DEFINITIONS FROM timers.c ======================== */
#define tmrNO_DELAY    ( TickType_t ) 0U

#define tmrSTATUS_IS_ACTIVE                  ( ( uint8_t ) 0x01 )
#define tmrSTATUS_IS_STATICALLY_ALLOCATED    ( ( uint8_t ) 0x02 )
#define tmrSTATUS_IS_AUTORELOAD              ( ( uint8_t ) 0x04 )

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


/* ============================  GLOBAL VARIABLES =========================== */
static uint16_t usMallocFreeCalls = 0;
static uint32_t critical_section_counter;

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
//    port_yield_within_api_called = true;
 //   py_operation();
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
void setUp( void )
{
    vFakeAssert_Ignore();
    /* Track calls to malloc / free */
    UnityMalloc_StartTest();
    critical_section_counter = 0;
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

static void xCallback_Test( TimerHandle_t xTimer )
{
}

/**
 * @brief xTimerCreate happy path
 *
 */
void test_xTimerCreate_Success( void )
{
    uint32_t ulID = 0;
    TimerHandle_t xTimer = NULL;
    Timer_t  pxNewTimer;
    QueueHandle_t queue_handle = (QueueHandle_t) 3; /* not zero */

    pvPortMalloc_ExpectAndReturn( sizeof(Timer_t), &pxNewTimer );
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn( queue_handle );
    vQueueAddToRegistry_Ignore();
    vListInitialiseItem_Ignore();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_NOT_EQUAL( NULL, xTimer );
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

    pvPortMalloc_ExpectAndReturn(sizeof(Timer_t), &pxNewTimer);
    vListInitialise_ExpectAnyArgs();
    vListInitialise_ExpectAnyArgs();
    xQueueGenericCreateStatic_ExpectAnyArgsAndReturn(NULL);
    vListInitialiseItem_Ignore();

    xTimer = xTimerCreate( "ut-timer",
                           pdMS_TO_TICKS( 1000 ),
                           pdTRUE,
                           &ulID,
                           xCallback_Test );

    TEST_ASSERT_EQUAL( &pxNewTimer, xTimer );
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
}
