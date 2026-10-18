/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt
 *
 */
#ifndef LWIPOPTS_H__
#define LWIPOPTS_H__

// Pulls in tusb_option.h → tusb_config.h, which defines LWIP_HIGH_THROUGHPUT
// based on the target MCU's SRAM tier.
#include "tusb_option.h"

#ifndef LWIP_HIGH_THROUGHPUT
  #define LWIP_HIGH_THROUGHPUT          0
#endif

/* Prevent having to link sys_arch.c (we don't test the API layers in unit tests) */
#define NO_SYS                          1
#define MEM_ALIGNMENT                   4
#define LWIP_RAW                        0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_DHCP                       0
#define LWIP_ICMP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define ETH_PAD_SIZE                    0
#define LWIP_IP_ACCEPT_UDP_PORT(p)      ((p) == PP_NTOHS(67))

#define TCP_MSS 1460
#define TCP_WND (12 * TCP_MSS)       // Increase window to ~17KB
#define TCP_SND_BUF (12 * TCP_MSS)   // Increase send buffer to ~17KB
#define TCP_SND_QUEUELEN 32          // Massively increase segment queue (default is too low)
#define MEMP_NUM_PBUF 64             // Give lwIP more PBUF structures to track the segments
#define MEMP_NUM_TCP_PCB 10          // Allow more simultaneous HTTP connections so fetch() doesn't bottleneck
#define MEMP_NUM_TCP_SEG 128
//#define LWIP_DISABLE_TCP_SANITY_CHECKS  0

#define ETHARP_SUPPORT_STATIC_ENTRIES   1

#define LWIP_HTTPD_CGI                  0
#define LWIP_HTTPD_SSI                  0
#define LWIP_HTTPD_SSI_INCLUDE_TAG      0
#define LWIP_HTTPD_CGI                  1
#define LWIP_HTTPD_SUPPORT_POST         1

#define LWIP_SINGLE_NETIF               1
#define LWIP_NETIF_LINK_CALLBACK        1

#define HTTPD_USE_CUSTOM_FSDATA         0
#define HTTPD_FSDATA_FILE               "pico_fsdata.inc"

#define LWIP_MULTICAST_PING             1
#define LWIP_BROADCAST_PING             1
#define LWIP_IPV6_MLD                   0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   0

#define MEM_SIZE                        (32 * 1024)
#define LWIP_HTTPD_LIMIT_SENDING_TO_2MSS 0
#define LWIP_HTTPD_TCP_NODELAY          1
#define LWIP_TCP_INITIAL_CWND           (10 * TCP_MSS)
#define PBUF_POOL_SIZE                  64
#define MEMP_NUM_TCP_PCB_LISTEN         5

void hw_debug_print(const char *format, ...);

// Route lwIP prints to UART function
//#define LWIP_PLATFORM_DIAG(x) do { hw_debug_print x; } while(0)

// Enable debug mode
//#define LWIP_DEBUG 1
// Turn on debugging for specific modules
//#define TCP_DEBUG       LWIP_DBG_ON
//#define HTTPD_DEBUG     LWIP_DBG_ON

#endif /* __LWIPOPTS_H__ */