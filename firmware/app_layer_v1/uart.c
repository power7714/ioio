/*
 * Copyright 2011 Ytai Ben-Tsvi. All rights reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list
 *       of conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL ARSHAN POURSOHI OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those of the
 * authors and should not be interpreted as representing official policies, either expressed
 * or implied.
 */

#include "uart.h"

#include <assert.h>
#include "atomic.h"
#include "Compiler.h"
#include "field_accessors.h"
#include "logging.h"
#include "platform.h"
#include "byte_queue.h"
#include "pp_util.h"
#include "protocol.h"
#include "sync.h"

#define RX_BUF_SIZE 256
#define TX_BUF_SIZE 256

#define TX_PRIORITY 2
#define RX_PRIORITY 4

typedef struct {
  volatile int num_tx_since_last_report;
  BYTE_QUEUE rx_queue;
  BYTE_QUEUE tx_queue;
  BYTE rx_buffer[RX_BUF_SIZE];
  BYTE tx_buffer[TX_BUF_SIZE];
} UART_STATE;

static UART_STATE uarts[NUM_UART_MODULES];

#define _UARTREG_REF_COMMA(num, dummy) (volatile UART*) &U##num##MODE,

volatile UART* uart_reg[NUM_UART_MODULES] = {
  REPEAT_1B(_UARTREG_REF_COMMA, NUM_UART_MODULES, 0)
};

static void UARTConfigInternal(int uart_num, int rate, int speed4x, int two_stop_bits, int parity, int external);

void UARTInit() {
  int i;
  for (i = 0; i < NUM_UART_MODULES; ++i) {
    UARTConfigInternal(i, 0, 0, 0, 0, 0);
    AssignUxRXIP(i, RX_PRIORITY);  // RX int. priority
    AssignUxTXIP(i, TX_PRIORITY);  // TX int. priority
  }
}

static inline void UARTSendStatus(int uart_num, int enabled) {
  OUTGOING_MESSAGE msg;
  msg.type = UART_STATUS;
  msg.args.uart_status.uart_num = uart_num;
  msg.args.uart_status.enabled = enabled;
  AppProtocolSendMessage(&msg);
}

static void UARTConfigInternal(int uart_num, int rate, int speed4x, int two_stop_bits, int parity, int external) {
  volatile UART* regs = uart_reg[uart_num];
  UART_STATE* uart = &uarts[uart_num];
  if (external) {
    log_printf("UARTConfig(%d, %d, %d, %d, %d)", uart_num, rate, speed4x,
               two_stop_bits, parity);
  }
  SAVE_UART_FOR_LOG(uart_num);
  AssignUxRXIE(uart_num, 0);  // disable RX int.
  AssignUxTXIE(uart_num, 0);  // disable TX int.
  regs->uxmode = 0x0000;  // disable UART.
  // clear SW buffers
  ByteQueueInit(&uart->rx_queue, uart->rx_buffer, RX_BUF_SIZE);
  ByteQueueInit(&uart->tx_queue, uart->tx_buffer, TX_BUF_SIZE);
  uart->num_tx_since_last_report = 0;
  if (rate) {
    if (external) {
      UARTSendStatus(uart_num, 1);
    }
    regs->uxbrg = rate;
    AssignUxRXIF(uart_num, 0);  // clear RX int.
    AssignUxTXIF(uart_num, 1);  // set TX int, since the hardware FIFO is empty.
    AssignUxRXIE(uart_num, 1);  // enable RX int.
    regs->uxmode = 0x8000 | (speed4x ? 0x0008 : 0x0000) | two_stop_bits | (parity << 1);  // enable
    regs->uxsta = 0x8400;  // IRQ when TX buffer is empty, enable TX, IRQ when character received.
    uart->num_tx_since_last_report = TX_BUF_SIZE;
  } else {
    if (external) {
      UARTSendStatus(uart_num, 0);
    }
  }
}

void UARTConfig(int uart_num, int rate, int speed4x, int two_stop_bits, int parity) {
  UARTConfigInternal(uart_num, rate, speed4x, two_stop_bits, parity, 1);
}

static void UARTReportTxStatus(int uart_num) {
  int report;
  UART_STATE* uart = &uarts[uart_num];
  PRIORITY(TX_PRIORITY) {
    report = uart->num_tx_since_last_report;
    uart->num_tx_since_last_report = 0;
  }
  OUTGOING_MESSAGE msg;
  msg.type = UART_REPORT_TX_STATUS;
  msg.args.uart_report_tx_status.uart_num = uart_num;
  msg.args.uart_report_tx_status.bytes_to_add = report;
  AppProtocolSendMessage(&msg);
}

void UARTTasks() {
  int i;
  for (i = 0; i < NUM_UART_MODULES; ++i) {
    int size1, size2;
    const BYTE *data1, *data2;
    UART_STATE* uart = &uarts[i];
    BYTE_QUEUE* q = &uart->rx_queue;
    ByteQueuePeekMax(q, 64, &data1, &size1, &data2, &size2);
    if (size1) {
      log_printf("UART %d received %d bytes", i, size1 + size2);
      OUTGOING_MESSAGE msg;
      msg.type = UART_DATA;
      msg.args.uart_data.uart_num = i;
      msg.args.uart_data.size = size1 + size2 - 1;
      AppProtocolSendMessageWithVarArgSplit(&msg, data1, size1, data2, size2);
      ByteQueuePull(q, size1 + size2);
    }
    if (uart->num_tx_since_last_report > TX_BUF_SIZE / 2) {
      UARTReportTxStatus(i);
    }
  }
}

static void TXInterrupt(int uart_num) {
  volatile UART* reg = uart_reg[uart_num];
  UART_STATE* uart = &uarts[uart_num];
  BYTE_QUEUE* q = &uart->tx_queue;
  while (ByteQueueSize(q) && !(reg->uxsta & 0x0200)) {
    // TXIF == 1 iff the hardware FIFO is empty. We're just about to make it
    // non-empty, so we are safe to clear.
    AssignUxTXIF(uart_num, 0);
    reg->uxtxreg = ByteQueuePullByte(q);
    ++uart->num_tx_since_last_report;
  }
  // IE == 1 iff there is data in the software queue pending to be sent.
  AssignUxTXIE(uart_num, ByteQueueSize(q) != 0);
}

static void RXInterrupt(int uart_num) {
  volatile UART* reg = uart_reg[uart_num];
  BYTE_QUEUE* q = &uarts[uart_num].rx_queue;
  while (reg->uxsta & 0x0001) {
    BYTE b = reg->uxrxreg;
    if ((reg->uxsta & 0x000C) == 0) {
      // there is no frame/parity err
      ByteQueuePushByte(q, b);
    } // Otherwise, discard
    // It is OK to clear the interrupt now, since we're just about to poll for
    // any remaining characters in the FIFO, so we'll never miss an interrupt.
    AssignUxRXIF(uart_num, 0);
  }
}

void UARTTransmit(int uart_num, const void* data, int size) {
  log_printf("UARTTransmit(%d, %p, %d)", uart_num, data, size);
  SAVE_UART_FOR_LOG(uart_num);
  BYTE_QUEUE* q = &uarts[uart_num].tx_queue;
  PRIORITY(TX_PRIORITY) {
    ByteQueuePushBuffer(q, data, size);
    AssignUxTXIE(uart_num, 1);  // enable TX int.
  }
}

#define DEFINE_INTERRUPT_HANDLERS(uart_num)                                   \
 void __attribute__((__interrupt__, auto_psv)) _U##uart_num##RXInterrupt() {  \
   RXInterrupt(uart_num - 1);                                                 \
 }                                                                            \
                                                                              \
 void __attribute__((__interrupt__, auto_psv)) _U##uart_num##TXInterrupt() {  \
   TXInterrupt(uart_num - 1);                                                 \
 }

#if NUM_UART_MODULES > 4
  #error Currently only devices with 4 or less UARTs are supported. Please fix below.
#endif

#if NUM_UART_MODULES >= 1
  DEFINE_INTERRUPT_HANDLERS(1)
#endif

#if NUM_UART_MODULES >= 2 && !ENABLE_LOGGING
  DEFINE_INTERRUPT_HANDLERS(2)
#endif

#if NUM_UART_MODULES >= 3
  DEFINE_INTERRUPT_HANDLERS(3)
#endif

#if NUM_UART_MODULES >= 4
  DEFINE_INTERRUPT_HANDLERS(4)
#endif

