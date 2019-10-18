/*
 * Copyright (c) 2014, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Contiki OS
 *
 */

/**
 * \file
 *         Contiki main for NXP JN5168 platform
 *
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dev/watchdog.h"
#include <AppHardwareApi.h>
#include <AppApi.h>
#include "dev/uart0.h"

#include "contiki.h"
#include "net/netstack.h"

#include "dev/serial-line.h"

#include "net/ip/uip.h"
#include "dev/leds.h"

#include "dev/button-sensor.h"
//#include "dev/pir-sensor.h"
//#include "dev/vib-sensor.h"

#include "lib/random.h"
#include "sys/node-id.h"
#include "rtimer-arch.h"

#if NETSTACK_CONF_WITH_IPV6
#include "net/ipv6/uip-ds6.h"
#endif /* NETSTACK_CONF_WITH_IPV6 */

#include "net/rime/rime.h"
#include "Recal/Include/recal.h"

#include "dev/micromac-radio.h"
#include "MMAC.h"
unsigned char node_mac[8];

/* Symbol defined by the linker script
 * marks the end of the stack taking into account the used heap  */
extern uint32_t heap_location;

/*&pir_sensor, &vib_sensor*/
SENSORS(&button_sensor);

#ifndef NETSTACK_CONF_WITH_IPV4
#define NETSTACK_CONF_WITH_IPV4 0
#endif

#if NETSTACK_CONF_WITH_IPV4
#include "net/ip/uip.h"
#include "net/ipv4/uip-fw.h"
#include "net/ipv4/uip-fw-drv.h"
#include "net/ipv4/uip-over-mesh.h"
static struct uip_fw_netif slipif =
  {UIP_FW_NETIF(192,168,1,2, 255,255,255,255, slip_send)};
static struct uip_fw_netif meshif =
  {UIP_FW_NETIF(172,16,0,0, 255,255,0,0, uip_over_mesh_send)};

#define UIP_OVER_MESH_CHANNEL 8
static uint8_t is_gateway;
#endif /* NETSTACK_CONF_WITH_IPV4 */

#ifdef EXPERIMENT_SETUP
#include "experiment-setup.h"
#endif

/*---------------------------------------------------------------------------*/
#define DEBUG 1
#if DEBUG
#define PRINTF(...) do {printf(__VA_ARGS__);} while(0)
#else
#define PRINTF(...) do {} while (0)
#endif
/*---------------------------------------------------------------------------*/
/* Reads MAC from SoC
 * Must be called before node_id_restore()
 * and network addresses initialization */
static void
init_node_mac(void)
{
  tuAddr psExtAddress;
  vMMAC_GetMacAddress(&psExtAddress.sExt);
  node_mac[7] = psExtAddress.sExt.u32L;
  node_mac[6] = psExtAddress.sExt.u32L >> (uint32_t)8;
  node_mac[5] = psExtAddress.sExt.u32L >> (uint32_t)16;
  node_mac[4] = psExtAddress.sExt.u32L >> (uint32_t)24;
  node_mac[3] = psExtAddress.sExt.u32H;
  node_mac[2] = psExtAddress.sExt.u32H >> (uint32_t)8;
  node_mac[1] = psExtAddress.sExt.u32H >> (uint32_t)16;
  node_mac[0] = psExtAddress.sExt.u32H >> (uint32_t)24;
}
/*---------------------------------------------------------------------------*/
#if !PROCESS_CONF_NO_PROCESS_NAMES
static void
print_processes(struct process * const processes[])
{
  /*  const struct process * const * p = processes;*/
  PRINTF("Starting");
  while(*processes != NULL) {
    PRINTF(" '%s'", (*processes)->name);
    processes++;
  }
  putchar('\n');
}
#endif /* !PROCESS_CONF_NO_PROCESS_NAMES */
/*---------------------------------------------------------------------------*/
#if NETSTACK_CONF_WITH_IPV4
static void
set_gateway(void)
{
  if(!is_gateway) {
    leds_on(LEDS_RED);
    printf("%d.%d: making myself the IP network gateway.\n\n",
     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
    printf("IPv4 address of the gateway: %d.%d.%d.%d\n\n",
     uip_ipaddr_to_quad(&uip_hostaddr));
    uip_over_mesh_set_gateway(&linkaddr_node_addr);
    uip_over_mesh_make_announced_gateway();
    is_gateway = 1;
  }
}
#endif /* NETSTACK_CONF_WITH_IPV4 */
/*---------------------------------------------------------------------------*/
static void
start_autostart_processes()
{
#if !PROCESS_CONF_NO_PROCESS_NAMES
  print_processes(autostart_processes);
#endif /* !PROCESS_CONF_NO_PROCESS_NAMES */
  autostart_start(autostart_processes);
}
/*---------------------------------------------------------------------------*/
#if NETSTACK_CONF_WITH_IPV6
static void
start_uip6(void)
{
  NETSTACK_NETWORK.init();

#ifndef WITH_SLIP_RADIO
  process_start(&tcpip_process, NULL);
#else
#if WITH_SLIP_RADIO == 0
  process_start(&tcpip_process, NULL);
#endif
#endif /* WITH_SLIP_RADIO */

#if DEBUG
  PRINTF("Tentative link-local IPv6 address ");
  {
    uip_ds6_addr_t *lladdr;
    int i;
    lladdr = uip_ds6_get_link_local(-1);
    for(i = 0; i < 7; ++i) {
      PRINTF("%02x%02x:", lladdr->ipaddr.u8[i * 2],
             lladdr->ipaddr.u8[i * 2 + 1]);
    }
    /* make it hardcoded... */
    lladdr->state = ADDR_AUTOCONF;

    PRINTF("%02x%02x\n", lladdr->ipaddr.u8[14], lladdr->ipaddr.u8[15]);
  }
#endif /* DEBUG */

  if(!UIP_CONF_IPV6_RPL) {
    uip_ipaddr_t ipaddr;
    int i;
    uip_ip6addr(&ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
    uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
    uip_ds6_addr_add(&ipaddr, 0, ADDR_TENTATIVE);
    PRINTF("Tentative global IPv6 address ");
    for(i = 0; i < 7; ++i) {
      PRINTF("%02x%02x:",
             ipaddr.u8[i * 2], ipaddr.u8[i * 2 + 1]);
    }
    PRINTF("%02x%02x\n",
           ipaddr.u8[7 * 2], ipaddr.u8[7 * 2 + 1]);
  }
}
#endif /* NETSTACK_CONF_WITH_IPV6 */
/*---------------------------------------------------------------------------*/
static void
start_network_layer(void)
{
#if NETSTACK_CONF_WITH_IPV6
  start_uip6();
#endif /* NETSTACK_CONF_WITH_IPV6 */
  start_autostart_processes();
  /* To support link layer security in combination with NETSTACK_CONF_WITH_IPV4 and
   * TIMESYNCH_CONF_ENABLED further things may need to be moved here */
}
/*--------------------------------------------------------------------------*/
static void
set_rime_addr(void)
{
  int i;
  linkaddr_t addr;
  memset(&addr, 0, sizeof(linkaddr_t));
#if NETSTACK_CONF_WITH_IPV6
  memcpy(addr.u8, node_mac, sizeof(addr.u8));
#else
  if(node_id == 0) {
    for(i = 0; i < sizeof(linkaddr_t); ++i) {
      addr.u8[i] = node_mac[7 - i];
    }
  } else {
    addr.u8[0] = node_id & 0xff;
    addr.u8[1] = node_id >> 8;
  }
#endif
  linkaddr_set_node_addr(&addr);
#if DEBUG
  PRINTF("Rime started with address ");
  for(i = 0; i < sizeof(addr.u8) - 1; i++) {
    PRINTF("%d.", addr.u8[i]);
  }
  PRINTF("%d\n", addr.u8[i]);
#endif
}
/*---------------------------------------------------------------------------*/
#if WITH_TINYOS_AUTO_IDS
uint16_t TOS_NODE_ID = 0x1234; /* non-zero */
uint16_t TOS_LOCAL_ADDRESS = 0x1234; /* non-zero */
#endif /* WITH_TINYOS_AUTO_IDS */
int
main(void)
{
  /* Set stack overflow address for detecting overflow in runtime */
  vAHI_SetStackOverflow(TRUE, ((uint32_t *)&heap_location)[0]);

  clock_init();
  watchdog_init();
  leds_init();
  leds_on(LEDS_ALL);
  init_node_mac();
  node_id_restore();
#if WITH_TINYOS_AUTO_IDS
  node_id = TOS_NODE_ID;
#endif /* WITH_TINYOS_AUTO_IDS */
  /* for setting "hardcoded" IEEE 802.15.4 MAC addresses */
#ifdef IEEE_802154_MAC_ADDRESS
  {
    uint8_t ieee[] = IEEE_802154_MAC_ADDRESS;
    memcpy(node_mac, ieee, sizeof(uip_lladdr.addr));
    node_mac[7] = node_id & 0xff;
  }
#endif
  /* TODO initialize random with a seed from the SoC random generator */
  random_init(node_mac[0] + node_mac[7] + node_id);
  process_init();
  ctimer_init();
  uart0_init(UART_BAUD_RATE); /* Must come before first PRINTF */

#if USE_SLIP_UART1
#include "dev/uart1.h"
  uart1_init(UART_BAUD_RATE);
#endif /* USE_SLIP_UART1 */

#if NETSTACK_CONF_WITH_IPV4
  slip_arch_init(UART_BAUD_RATE);
#endif /* NETSTACK_CONF_WITH_IPV4 */

  /* check for reset source */
  if (bAHI_WatchdogResetEvent()) {
		PRINTF("Init: Watchdog timer has reset device!\r\n");
	}

  process_start(&etimer_process, NULL);
  set_rime_addr();
  netstack_init();

#if NETSTACK_CONF_WITH_IPV6
#if UIP_CONF_IPV6_RPL
  PRINTF(CONTIKI_VERSION_STRING " started with IPV6, RPL\n");
#else
  PRINTF(CONTIKI_VERSION_STRING " started with IPV6\n");
#endif
#elif NETSTACK_CONF_WITH_IPV4
  PRINTF(CONTIKI_VERSION_STRING " started with IPV4\n");
#else
  PRINTF(CONTIKI_VERSION_STRING " started\n");
#endif

  if(node_id > 0) {
    PRINTF("Node id is set to %u.\n", node_id);
  } else {
    PRINTF("Node id is not set.\n");
  }

#if NETSTACK_CONF_WITH_IPV6
  memcpy(&uip_lladdr.addr, node_mac, sizeof(uip_lladdr.addr));

  queuebuf_init();
  NETSTACK_RDC.init();
  NETSTACK_MAC.init();
#else
  NETSTACK_RDC.init();
  NETSTACK_MAC.init();
  NETSTACK_NETWORK.init();
#endif /* NETSTACK_CONF_WITH_IPV6 */

  PRINTF("%s %s %s\n", NETSTACK_LLSEC.name, NETSTACK_MAC.name, NETSTACK_RDC.name);

#if !NETSTACK_CONF_WITH_IPV4 && !NETSTACK_CONF_WITH_IPV6
  uart0_set_input(serial_line_input_byte);
  serial_line_init();
#endif

#if TIMESYNCH_CONF_ENABLED
  timesynch_init();
  timesynch_set_authority_level((linkaddr_node_addr.u8[0] << 4) + 16);
#endif /* TIMESYNCH_CONF_ENABLED */

#if NETSTACK_CONF_WITH_IPV4
  process_start(&tcpip_process, NULL);
  process_start(&uip_fw_process, NULL); /* Start IP output */
  process_start(&slip_process, NULL);

  slip_set_input_callback(set_gateway);

  {
    uip_ipaddr_t hostaddr, netmask;

    uip_init();

    uip_ipaddr(&hostaddr, 172,16,
         linkaddr_node_addr.u8[0],linkaddr_node_addr.u8[1]);
    uip_ipaddr(&netmask, 255,255,0,0);
    uip_ipaddr_copy(&meshif.ipaddr, &hostaddr);

    uip_sethostaddr(&hostaddr);
    uip_setnetmask(&netmask);
    uip_over_mesh_set_net(&hostaddr, &netmask);
    /*    uip_fw_register(&slipif);*/
    uip_over_mesh_set_gateway_netif(&slipif);
    uip_fw_default(&meshif);
    uip_over_mesh_init(UIP_OVER_MESH_CHANNEL);
    PRINTF("uIP started with IP address %d.%d.%d.%d\n",
     uip_ipaddr_to_quad(&hostaddr));
  }
#endif /* NETSTACK_CONF_WITH_IPV4 */

#if !PROCESS_CONF_NO_PROCESS_NAMES
  print_processes(autostart_processes);
#endif /* !PROCESS_CONF_NO_PROCESS_NAMES */

  watchdog_start();
  start_network_layer();
//  NETSTACK_LLSEC.bootstrap(start_network_layer);

  leds_off(LEDS_ALL);
  int r;
  while(1) {
    do {
      /* Reset watchdog. */
    	watchdog_periodic();
      r = process_run();
    } while(r > 0);
    /*
     * Idle processing.
     */
    watchdog_stop();

#if DCOSYNCH_CONF_ENABLED
    /* Calibrate the DCO every DCOSYNCH_PERIOD
     * if we have more than 500uSec until next rtimer
     * PS: Calibration disables interrupts and blocks for 200uSec.
     *  */
    static unsigned long last_dco_calibration_time = 0;
    if(clock_seconds() - last_dco_calibration_time > DCOSYNCH_PERIOD) {
      if(rtimer_arch_get_time_until_next_wakeup() > RTIMER_SECOND/2000) {
        /* PRINTF("ContikiMain: Calibrating the DCO\n"); */
        eAHI_AttemptCalibration();
        last_dco_calibration_time = clock_seconds();
      }
    }
#endif /* DCOSYNCH_CONF_ENABLED */
    vAHI_CpuDoze();
    watchdog_start();
  }

  return 0;
}
/*---------------------------------------------------------------------------*/
#if LOG_CONF_ENABLED
void
log_message(char *m1, char *m2)
{
  printf("%s%s\n", m1, m2);
}
#endif /* LOG_CONF_ENABLED */
/*---------------------------------------------------------------------------*/
void
uip_log(char *m)
{
  PRINTF("uip_log: %s\n", m);
}
/*---------------------------------------------------------------------------*/
void
AppColdStart(void)
{
  /* After reset or sleep with memory off */
	main();

}
/*---------------------------------------------------------------------------*/
void
AppWarmStart(void)
{
  /* TODO Wakeup after sleep with memory on
   * Need to initialize devices but not the application state */
	main();
}
/*---------------------------------------------------------------------------*/
#if UIP_ARCH_ADD32
void
uip_add32(uint8_t *op32, uint16_t op16)
{
  uip_acc32[3] = op32[3] + (op16 & 0xff);
  uip_acc32[2] = op32[2] + (op16 >> 8);
  uip_acc32[1] = op32[1];
  uip_acc32[0] = op32[0];

  if(uip_acc32[2] < (op16 >> 8)) {
    ++uip_acc32[1];
    if(uip_acc32[1] == 0) {
      ++uip_acc32[0];
    }
  }


  if(uip_acc32[3] < (op16 & 0xff)) {
    ++uip_acc32[2];
    if(uip_acc32[2] == 0) {
      ++uip_acc32[1];
      if(uip_acc32[1] == 0) {
	++uip_acc32[0];
      }
    }
  }
}

#endif /* UIP_ARCH_ADD32 */

#if UIP_ARCH_CHKSUM
/*---------------------------------------------------------------------------*/
static uint16_t
chksum(uint16_t sum, const uint8_t *data, uint16_t len)
{
  uint16_t t;
  const uint8_t *dataptr;
  const uint8_t *last_byte;

  dataptr = data;
  last_byte = data + len - 1;

  while(dataptr < last_byte) {	/* At least two more bytes */
    t = (dataptr[0] << 8) + dataptr[1];
    sum += t;
    if(sum < t) {
      sum++;		/* carry */
    }
    dataptr += 2;
  }

  if(dataptr == last_byte) {
    t = (dataptr[0] << 8) + 0;
    sum += t;
    if(sum < t) {
      sum++;		/* carry */
    }
  }

  /* Return sum in host byte order. */
  return sum;
}
/*---------------------------------------------------------------------------*/
uint16_t
uip_chksum(uint16_t *data, uint16_t len)
{
  return uip_htons(chksum(0, (uint8_t *)data, len));
}
#endif  /*UIP_ARCH_CHKSUM*/
/*---------------------------------------------------------------------------*/
#ifdef UIP_ARCH_IPCHKSUM
uint16_t
uip_ipchksum(void)
{
  uint16_t sum;

  sum = chksum(0, &uip_buf[UIP_LLH_LEN], UIP_IPH_LEN);
  DEBUG_PRINTF("uip_ipchksum: sum 0x%04x\n", sum);
  return (sum == 0) ? 0xffff : uip_htons(sum);
}
#endif
