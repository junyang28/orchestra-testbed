/*
 * Copyright (c) 2014, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         IEEE 802.15.4 TSCH MAC implementation. Must be used with nullmac as NETSTACK_CONF_MAC
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 *         Simon Duquennoy <simonduq@sics.se>
 *
 */

#include "contiki.h"
#include "dev/leds.h"
#include "dev/radio.h"
#include "net/nbr-table.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-log.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/frame802154.h"
#include "lib/random.h"
#include "lib/ringbufindex.h"
#include "sys/process.h"
#include "sys/rtimer.h"
/* TODO: remove dependencies to RPL */
#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#include <string.h>

/* TODO: move platform-specific code away from core */
#if CONTIKI_TARGET_JN5168
#include "dev/micromac-radio.h"
#else
#include "dev/cc2420/cc2420.h"
#endif

/* Truncate received drift correction information to maximum half
 * of the guard time. */
#define TRUNCATE_SYNC_IE 1
#define TRUNCATE_SYNC_IE_BOUND ((int)TsLongGT/2)

/* inject drift to test drift correction */
#define DEBUG_INJECT_DRIFT 0

#ifdef TSCH_CALLBACK_DO_NACK
int TSCH_CALLBACK_DO_NACK(struct tsch_link *link, linkaddr_t *src, linkaddr_t *dst);
#endif

#ifdef TSCH_CALLBACK_JOINING_NETWORK
void TSCH_CALLBACK_JOINING_NETWORK();
#endif

#ifdef TSCH_CALLBACK_LEAVING_NETWORK
void TSCH_CALLBACK_LEAVING_NETWORK();
#endif

/* When associating, check ASN against our own uptime (time in minutes) */
#ifdef TSCH_CONF_CHECK_TIME_AT_ASSOCIATION
#define TSCH_CHECK_TIME_AT_ASSOCIATION TSCH_CONF_CHECK_TIME_AT_ASSOCIATION
#else
#define TSCH_CHECK_TIME_AT_ASSOCIATION 0
#endif

#ifdef TSCH_CONF_USE_SFD_FOR_SYNC
#define TSCH_USE_SFD_FOR_SYNC TSCH_CONF_USE_SFD_FOR_SYNC
#else
#define TSCH_USE_SFD_FOR_SYNC 0
#endif

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

#ifdef TSCH_CONF_N_CHANNELS
#define TSCH_N_CHANNELS TSCH_CONF_N_CHANNELS
#else
#define TSCH_N_CHANNELS 16
#endif /* TSCH_CONF_N_CHANNELS */

#ifdef TSCH_CONF_ADDRESS_FILTER
#define TSCH_ADDRESS_FILTER TSCH_CONF_ADDRESS_FILTER
#else
#define TSCH_ADDRESS_FILTER 0
#endif /* TSCH_CONF_ADDRESS_FILTER */

#ifdef TSCH_CONF_EB_AUTOSELECT
#define TSCH_EB_AUTOSELECT TSCH_CONF_EB_AUTOSELECT
#else
#define TSCH_EB_AUTOSELECT 0
#endif /* TSCH_CONF_EB_AUTOSELECT */

#ifndef TSCH_802154_DUPLICATE_DETECTION
#ifdef TSCH_CONF_802154_DUPLICATE_DETECTION
#define TSCH_802154_DUPLICATE_DETECTION TSCH_CONF_802154_DUPLICATE_DETECTION
#else
#define TSCH_802154_DUPLICATE_DETECTION 1
#endif /* TSCH_CONF_802154_AUTOACK */
#endif /* TSCH_802154_AUTOACK */

#if TSCH_802154_DUPLICATE_DETECTION
struct seqno {
  linkaddr_t sender;
  uint8_t seqno;
};

#ifdef NETSTACK_CONF_MAC_SEQNO_HISTORY
#define MAX_SEQNOS NETSTACK_CONF_MAC_SEQNO_HISTORY
#else /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
#define MAX_SEQNOS 8
#endif /* NETSTACK_CONF_MAC_SEQNO_HISTORY */

#if TSCH_EB_AUTOSELECT
int best_neighbor_eb_count;
struct eb_stat {
  int rx_count;
  int jp;
};
NBR_TABLE(struct eb_stat, eb_stats);
#endif

static struct seqno received_seqnos[MAX_SEQNOS];
#endif /* TSCH_802154_DUPLICATE_DETECTION */

// TODO use the standard hopping sequence
/* Channel hopping: a list of channels (ordered by measured goodness from a specific testbed experiment in Indriya) */
uint8_t hopping_sequence_list[] = { 26, 15, 25, 20, 16, 19, 14, 24, 18, 17, 17, 11, 21, 23, 12, 22, 13 };
//uint8_t hopping_sequence_list[] = { 23, 12, 22, 13 };
struct asn_divisor_t hopping_sequence_length;

/* 802.15.4 broadcast MAC address  */
const linkaddr_t tsch_broadcast_address = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
/* Address used for the EB virtual neighbor queue */
const linkaddr_t tsch_eb_address = { { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* A global variable telling whether we are coordinator of the TSCH network
 * TODO: have a function to set this */
int tsch_is_coordinator = 0;

/* The current radio channel */
static uint8_t current_channel = -1;
/* The current Absolute Slot Number (ASN) */
struct asn_t current_asn;
/* Last time we received Sync-IE (ACK or data packet from a time source) */
static struct asn_t last_sync_asn;
/* Are we associated to a TSCH network? */
int associated = 0;
/* Time of association in seconds */
static clock_time_t association_time;
/* Device rank or join priority:
 * For PAN coordinator: 0 -- lower is better
 * To be inherited from RPL
 * TODO: remove dependencies from RPL */
uint8_t tsch_join_priority;
/* The current TSCH sequence number, used for both data and EBs */
static uint8_t tsch_packet_seqno = 0;
/* Current period for EB output */
static clock_time_t tsch_current_eb_period;

/* timer for sending keepalive messages */
static struct ctimer keepalive_timer;

/* Ringbuf for dequeued outgoing packets */
#define DEQUEUED_ARRAY_SIZE 16
#if DEQUEUED_ARRAY_SIZE < QUEUEBUF_NUM
#error TSCH_QUEUE_NUM_PER_NEIGHBOR must be greater than QUEUEBUF_NUM
#endif
#if (DEQUEUED_ARRAY_SIZE & (DEQUEUED_ARRAY_SIZE-1)) != 0
#error TSCH_QUEUE_NUM_PER_NEIGHBOR must be power of two
#endif

static struct ringbufindex dequeued_ringbuf;
static struct tsch_packet *dequeued_array[DEQUEUED_ARRAY_SIZE];

/* Ringbuf for incoming packets: must be power of two for atomic ringbuf operation */
#ifdef TSCH_CONF_MAX_INCOMING_PACKETS
#define TSCH_MAX_INCOMING_PACKETS TSCH_CONF_MAX_INCOMING_PACKETS
#else
#define TSCH_MAX_INCOMING_PACKETS 4
#endif
#if (TSCH_MAX_INCOMING_PACKETS & (TSCH_MAX_INCOMING_PACKETS-1)) != 0
#error TSCH_MAX_INCOMING_PACKETS must be power of two
#endif

 struct input_packet {
   uint8_t payload[TSCH_MAX_PACKET_LEN];
   struct asn_t rx_asn;
   int len;
   uint16_t rssi;
 };

static struct input_packet input_eb;
struct ringbufindex input_ringbuf;
struct input_packet input_array[TSCH_MAX_INCOMING_PACKETS];

/* Last estimated drift in RTIMER ticks
 * (Sky: 1 tick ~= 30.52 uSec) */
static int32_t drift_correction = 0;
static struct tsch_neighbor *drift_neighbor = NULL;

/* Used from tsch_link_operation and sub-protothreads */
static rtimer_clock_t volatile current_link_start;

/* Are we currently inside a link? */
static volatile int tsch_in_link_operation = 0;

/* Time to next wake up */
static rtimer_clock_t tsch_time_until_next_active_link = 0;

struct tsch_link *current_link;
static struct tsch_packet *current_packet;
static struct tsch_neighbor *current_neighbor;

/* Protothread for link operation, called from rtimer interrupt
 * and scheduled from tsch_schedule_link_operation */
static PT_THREAD(tsch_link_operation(struct rtimer *t, void *ptr));
static struct pt link_operation_pt;
/* Sub-protothreads of tsch_link_operation */
static PT_THREAD(tsch_tx_link(struct pt *pt, struct rtimer *t));
static PT_THREAD(tsch_rx_link(struct pt *pt, struct rtimer *t));

/* Sub-protothread of tsch_process */
static PT_THREAD(tsch_associate(struct pt *pt));
/* TSCH Contiki processes */
PROCESS(tsch_send_eb_process, "TSCH: send EB process");
PROCESS(tsch_process, "TSCH: main process");
PROCESS(tsch_pending_events_process, "TSCH: pending events process");

/* Other function prototypes */
static void tsch_reset(void);
static void tsch_tx_process_pending();
static void tsch_rx_process_pending();
static void tsch_schedule_keepalive();

/* Debug timing */
static rtimer_clock_t t0prepare = 0, t0tx = 0, t0txack = 0, t0post_tx = 0, t0rx = 0, t0rxack = 0;

/* A global lock for manipulating data structures safely from outside of interrupt */
static volatile int tsch_locked = 0;
/* As long as this is set, skip all link operation */
static volatile int tsch_lock_requested = 0;

/* Is TSCH locked? */
int tsch_is_locked() {
  return tsch_locked;
}

/* Lock TSCH (no link operation) */
int tsch_get_lock() {
  if(!tsch_locked) {
    rtimer_clock_t busy_wait_time;
    int busy_wait = 0; /* Flag used for logging purposes */
    /* Make sure no new link operation will start */
    tsch_lock_requested = 1;
    /* Wait for the end of current link operation. */
    if(tsch_in_link_operation) {
      busy_wait = 1;
      busy_wait_time = RTIMER_NOW();
      while(tsch_in_link_operation);
      busy_wait_time = RTIMER_NOW() - busy_wait_time;
    }
    if(!tsch_locked) {
      /* Take the lock if it is free */
      tsch_locked = 1;
      tsch_lock_requested = 0;
      if(busy_wait) {
        /* Issue a log whenever we had to busy wait until getting the lock */
        TSCH_LOG_ADD(tsch_log_message,
            snprintf(log->message, sizeof(log->message),
                "!get lock delay %u", busy_wait_time);
        );
      }
      return 1;
    }
  }
  TSCH_LOG_ADD(tsch_log_message,
      snprintf(log->message, sizeof(log->message),
                      "!failed to lock");
          );
  return 0;
}

/* Release TSCH lock */
void tsch_release_lock() {
  tsch_locked = 0;
}

/*---------------------------------------------------------------------------*/
static void
on(void)
{
  NETSTACK_RADIO.on();
}
/*---------------------------------------------------------------------------*/
static void
off(void)
{
  NETSTACK_RADIO.off();
}
/*---------------------------------------------------------------------------*/
static unsigned short
channel_check_interval(void)
{
  return 0;
}
/*
 * Timing
 */

/* Checks if the current time has past a ref time + offset. Assumes
 * a single overflow and ref time prior to now. */
static uint8_t
check_timer_miss(rtimer_clock_t ref_time, rtimer_clock_t offset, rtimer_clock_t now)
{
  rtimer_clock_t target = ref_time + offset - RTIMER_MIN_DELAY;
  int now_has_overflowed = now < ref_time;
  int target_has_overflowed = target < ref_time;

  if(now_has_overflowed == target_has_overflowed) {
    /* Both or none have overflowed, just compare now to the target */
    return target <= now;
  } else {
    /* Either now or target of overflowed.
     * If it is now, then it has passed the target.
     * If it is target, then we haven't reached it yet.
     *  */
    return now_has_overflowed;
  }
}
/* Wait for a condition with timeout t0+offset. */
#define BUSYWAIT_UNTIL_ABS(cond, t0, offset) \
  do { \
    rtimer_clock_t now = RTIMER_NOW(); \
    if(!check_timer_miss((t0), (offset), now)) { \
      while(!(cond) && RTIMER_CLOCK_LT(RTIMER_NOW(), (t0) + (offset))) ; \
    } \
  } while(0)

/* Schedule a wakeup at a specified offset from a reference time.
 * Provides basic protection against missed deadlines and timer overflows
 * A non-zero return value signals to tsch_link_operation a missed deadline.
 * If conditional: schedule only if the deadline is not missed.
 * Otherwise: schedule regardless of deadline miss. */
static uint8_t
tsch_schedule_link_operation(struct rtimer *tm, rtimer_clock_t ref_time, rtimer_clock_t offset, int conditional)
{
  rtimer_clock_t now = RTIMER_NOW();
  int r;
  int missed = check_timer_miss(ref_time, offset, now);

  if(missed) {
    TSCH_LOG_ADD(tsch_log_message,
                snprintf(log->message, sizeof(log->message),
                    "!dl-miss-%d %d %d",
                        conditional,
                        (int)(now - ref_time), (int)offset);
    );

    if(conditional) {
      return 0;
    }
  }
  ref_time += offset;
  r = rtimer_set(tm, ref_time, 1, (void (*)(struct rtimer *, void *))tsch_link_operation, NULL /*(void*)&status*/);
  if(r != RTIMER_OK) {
    return 0;
  }
  return 1;
}
/* Schedule link operation conditionally, and YIELD if success only */
#define TSCH_SCHEDULE_AND_YIELD(pt, tm, ref_time, offset) \
  do { \
    if(tsch_schedule_link_operation(tm, ref_time, offset, 1)) { \
      PT_YIELD(pt); \
    } \
  } while(0);

/*
 * Channel hopping
 */

/* Return channel from ASN and channel offset */
uint8_t
tsch_calculate_channel(struct asn_t *asn, uint8_t channel_offset)
{
  uint16_t index_of_0 = ASN_MOD(*asn, hopping_sequence_length);
  uint16_t index_of_offset = (index_of_0 + channel_offset) % hopping_sequence_length.val;
  return hopping_sequence_list[index_of_offset];
}
/* Select the current channel from ASN and channel offset, hop to it */
static void
hop_channel(struct asn_t *asn, uint8_t offset)
{
  current_channel = -1;
  uint8_t channel = tsch_calculate_channel(asn, offset);
  if(current_channel != channel) {
    NETSTACK_RADIO_set_channel(channel);
    current_channel = channel;
  }
}
/*---------------------------------------------------------------------------*/
/* Function send for TSCH-MAC, puts the packet in packetbuf in the MAC queue */
static void
send_packet(mac_callback_t sent, void *ptr)
{
  int ret = MAC_TX_DEFERRED;
  int packet_count_before;
  const linkaddr_t *addr = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

  /*
  if(!associated) {
    LOGP("TSCH:! not associated");
    ret = MAC_TX_ERR;
    mac_call_sent_callback(sent, ptr, ret, 1);
    return;
  }
  */

  /* PACKETBUF_ATTR_MAC_SEQNO cannot be zero, due to a pecuilarity
         in framer-802154.c. */
  if(++tsch_packet_seqno == 0) {
    tsch_packet_seqno++;
  }

  /* Ask for ACK if we are sending anything other than broadcast */
  if(!linkaddr_cmp(addr, &linkaddr_null)) {
    packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, 1);
  } else {
    /* Broadcast packets shall be added to broadcast queue
     * The broadcast address in Contiki is linkaddr_null which is equal
     * to tsch_eb_address */
    addr = &tsch_broadcast_address;
  }
  packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, tsch_packet_seqno);

  packet_count_before = tsch_queue_packet_count(addr);

  if(NETSTACK_FRAMER.create() < 0) {
    //LOGP("TSCH:! can't send packet due to framer error");
    ret = MAC_TX_ERR;
  } else {
    /* Enqueue packet */
    if(!tsch_queue_add_packet(addr, sent, ptr)) {
      LOGP("TSCH:! can't send packet !tsch_queue_add_packet");
      ret = MAC_TX_ERR;
    } else {
      LOGP("TSCH: send packet to %u with seqno %u, queue %u %u",
            LOG_NODEID_FROM_LINKADDR(addr), tsch_packet_seqno,
            packet_count_before,
            tsch_queue_packet_count(addr));
    }
  }
  if(ret != MAC_TX_DEFERRED) {
    mac_call_sent_callback(sent, ptr, ret, 1);
  }
}
/*---------------------------------------------------------------------------*/
static void
packet_input(void)
{
#ifdef NETSTACK_DECRYPT
  NETSTACK_DECRYPT();
#endif /* NETSTACK_DECRYPT */

  int frame_parsed = 1;

#if !RADIO_PARSE_MAC_HW
    frame_parsed = NETSTACK_FRAMER.parse();
#endif

    if(frame_parsed < 0) {
      //LOGP("TSCH:! failed to parse %u", packetbuf_datalen());
#if TSCH_ADDRESS_FILTER
  } else if(!linkaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                          &linkaddr_node_addr)
            && !linkaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                             &linkaddr_null)) {
    LOGP("TSCH:! not for us");
#endif /* TSCH_ADDRESS_FILTER */
  } else {
    int duplicate = 0;

#if TSCH_802154_DUPLICATE_DETECTION
    /* Check for duplicate packet by comparing the sequence number
       of the incoming packet with the last few ones we saw. */
    int i;
    for(i = 0; i < MAX_SEQNOS; ++i) {
      if(packetbuf_attr(PACKETBUF_ATTR_PACKET_ID) == received_seqnos[i].seqno &&
         linkaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_SENDER),
                      &received_seqnos[i].sender)) {
        /* Drop the packet. */
        LOGP("TSCH:! drop dup ll from %u seqno %u",
               LOG_NODEID_FROM_LINKADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER)),
               packetbuf_attr(PACKETBUF_ATTR_PACKET_ID));
        duplicate = 1;
      }
    }
    if(!duplicate) {
      for(i = MAX_SEQNOS - 1; i > 0; --i) {
        memcpy(&received_seqnos[i], &received_seqnos[i - 1],
               sizeof(struct seqno));
      }
      received_seqnos[0].seqno = packetbuf_attr(PACKETBUF_ATTR_PACKET_ID);
      linkaddr_copy(&received_seqnos[0].sender,
                    packetbuf_addr(PACKETBUF_ADDR_SENDER));
    }
#endif /* TSCH_802154_DUPLICATE_DETECTION */

    if(packetbuf_datalen() == 0) {
//    LOG("TSCH: KA received from %u\n",
//          LOG_NODEID_FROM_LINKADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER)));
    } else {
      if(!duplicate) {
        LOGP("TSCH: received from %u with seqno %u",
                       LOG_NODEID_FROM_LINKADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER)),
                       packetbuf_attr(PACKETBUF_ATTR_PACKET_ID));
        NETSTACK_NETWORK.input();
      }
    }
  }
}
/* Tx callback for keepalive messages */
static void
keepalive_packet_sent(void *ptr, int status, int transmissions)
{
  uip_ds6_link_neighbor_callback(status, transmissions);
//  LOG("TSCH: KA sent to %u, st %d %d\n",
//      LOG_NODEID_FROM_LINKADDR(packetbuf_addr(PACKETBUF_ADDR_RECEIVER)), status, transmissions);
  tsch_schedule_keepalive();
}
/* Prepare and send a keepalive message */
static void
keepalive_send()
{
  if(associated) {
    struct tsch_neighbor *n = tsch_queue_get_time_source();
    /* Simply send an empty packet */
    /* TODO filter keep alive messages based on packet type
     * (MAC_COMMAND) not data length*/
    packetbuf_clear();
    packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &n->addr);
    send_packet(keepalive_packet_sent, NULL);
//    LOG("TSCH: sending KA to %u\n",
//        LOG_NODEID_FROM_LINKADDR(&n->addr));
  }
}
/* Set ctimer to send a keepalive message after expiration of TSCH_KEEPALIVE_TIMEOUT */
static void
tsch_schedule_keepalive()
{
  /* Pick a delay in the range [TSCH_KEEPALIVE_TIMEOUT*0.9, TSCH_KEEPALIVE_TIMEOUT[ */
  if(!tsch_is_coordinator && associated) {
    unsigned long delay = (TSCH_KEEPALIVE_TIMEOUT - TSCH_KEEPALIVE_TIMEOUT/10)
                            + random_rand() % (TSCH_KEEPALIVE_TIMEOUT/10);
    ctimer_set(&keepalive_timer, delay, keepalive_send, NULL);
  }
}

/* Get EB, broadcast or unicast packet to be sent, and target neighbor. */
static struct tsch_packet *
get_packet_and_neighbor_for_link(struct tsch_link *link, struct tsch_neighbor **target_neighbor)
{
  struct tsch_packet *p = NULL;
  struct tsch_neighbor *n = NULL;

  /* Is this a Tx link? */
  if(link->link_options & LINK_OPTION_TX) {
    int is_shared_link = link->link_options & LINK_OPTION_SHARED;
    /* is it for advertisement of EB? */
    if(link->link_type == LINK_TYPE_ADVERTISING || link->link_type == LINK_TYPE_ADVERTISING_ONLY) {
      /* fetch EB packets */
      n = n_eb;
      p = tsch_queue_get_packet_for_nbr(n, 0);
    }
    if(link->link_type != LINK_TYPE_ADVERTISING_ONLY) {
      /* NORMAL link or no EB to send, pick a data packet */
      if(p == NULL) {
        /* Get neighbor queue associated to the link and get packet from it */
        n = tsch_queue_get_nbr(&link->addr);
        p = tsch_queue_get_packet_for_nbr(n, is_shared_link);
        /* if it is a broadcast slot and there were no broadcast packets, pick any unicast packet */
        if(p == NULL && n == n_broadcast) {
          p = tsch_queue_get_unicast_packet_for_any(&n, is_shared_link);
        }
      }
    }
  }
  /* return nbr (by reference) */
  if(target_neighbor != NULL) {
    *target_neighbor = n;
  }

  return p;
}


/* Post TX: Update neighbor state after a transmission */
static int
update_neighbor_state(struct tsch_neighbor *n, struct tsch_packet *p,
                      struct tsch_link *link, uint8_t mac_tx_status)
{
  int in_queue = 1;
  int is_shared_link = link->link_options & LINK_OPTION_SHARED;
  int is_unicast = !n->is_broadcast;

  t0post_tx = RTIMER_NOW();

  if(mac_tx_status == MAC_TX_OK) {
    /* Successful transmission */
    tsch_queue_remove_packet_from_queue(n);
    in_queue = 0;

    /* Update CSMA state in the unicast case */
    if(is_unicast) {
      if(is_shared_link || tsch_queue_is_empty(n)) {
        /* If this is a shared link, reset backoff on success.
         * Otherwise, do so only is the queue is empty */
        tsch_queue_backoff_reset(n);
      }
    }
  } else {
    /* Failed transmission */
    if(p->transmissions >= MAC_MAX_FRAME_RETRIES + 1) {
      /* Drop packet */
      tsch_queue_remove_packet_from_queue(n);
      in_queue = 0;
    }
    /* Update CSMA state in the unicast case */
    if(is_unicast) {
      /* Failures on dedicated (== non-shared) leave the backoff
       * window nor exponent unchanged */
      if(is_shared_link) {
        /* Shared link: increment backoff exponent, pick a new window */
        tsch_queue_backoff_inc(n);
      }
    }
  }

  t0post_tx = RTIMER_NOW() - t0post_tx;

  return in_queue;
}
static
PT_THREAD(tsch_tx_link(struct pt *pt, struct rtimer *t))
{
  /**
   * TX link:
   * 1. Copy packet to radio buffer
   * 2. Perform CCA if enabled
   * 3. Sleep until it is time to transmit
   * 4. Wait for ACK if it is a unicast packet
   * 5. Extract drift if we received an E-ACK from a time source neighbor
   * 6. Update CSMA parameters according to TX status
   * 7. Schedule mac_call_sent_callback
   **/

  /* tx status */
  static uint8_t mac_tx_status;
  /* is the packet in its neighbor's queue? */
  uint8_t in_queue;
  static int dequeued_index;
  int packet_ready = 1;

  PT_BEGIN(pt);

  /* First check if we have space to store a newly dequeued packet (in case of
   * successful Tx or Drop) */
  dequeued_index = ringbufindex_peek_put(&dequeued_ringbuf);
  if(dequeued_index != -1) {

    /* TODO There are small timing variations visible in cooja, which needs tuning */
    t0prepare = RTIMER_NOW();

    if(current_packet == NULL || current_packet->qb == NULL) {
      mac_tx_status = MAC_TX_ERR_FATAL;
    } else {
      /* packet payload */
      static void *payload;
      /* packet payload length */
      static uint8_t payload_len;
      /* packet seqno */
      static uint8_t seqno;
      /* is this a broadcast packet? (wait for ack?) */
      static uint8_t is_broadcast;
      static rtimer_clock_t tx_start_time;

#if CCA_ENABLED
      static uint8_t cca_status;
#endif

      /* get payload */
      payload = queuebuf_dataptr(current_packet->qb);
      payload_len = queuebuf_datalen(current_packet->qb);
      /* is this a broadcast packet? (wait for ack?) */
      is_broadcast = current_neighbor->is_broadcast;
      /* read seqno from payload */
      seqno = ((uint8_t *)(payload))[2];
      /* if this is an EB, then update its Sync-IE */
      if(current_neighbor == n_eb) {
        packet_ready = tsch_packet_update_eb(payload, payload_len);
      }
      /* prepare packet to send: copy to radio buffer */
      if(packet_ready && NETSTACK_RADIO.prepare(payload, payload_len) == 0) { /* 0 means success */
        static rtimer_clock_t tx_duration;

        t0prepare = RTIMER_NOW() - t0prepare;

#if CCA_ENABLED
        cca_status = 1;
        /* delay before CCA */
        TSCH_SCHEDULE_AND_YIELD(pt, t, current_link_start, TsCCAOffset);
        on();
        /* CCA */
        BUSYWAIT_UNTIL_ABS(!(cca_status |= NETSTACK_RADIO.channel_clear()),
            current_link_start, TsCCAOffset + TsCCA);
        /* there is not enough time to turn radio off */
        /*  off(); */
        if(cca_status == 0) {
          mac_tx_status = MAC_TX_COLLISION;
        } else
#endif /* CCA_ENABLED */
        {
          /* delay before TX */
          TSCH_SCHEDULE_AND_YIELD(pt, t, current_link_start, TsTxOffset - delayTx);
          t0tx = RTIMER_NOW();
          /* send packet already in radio tx buffer */
          mac_tx_status = NETSTACK_RADIO.transmit(payload_len);
          /* Save tx timestamp */
#if TSCH_USE_SFD_FOR_SYNC
          tx_start_time = current_link_start + TsTxOffset;
          /* TODO test this on H/W */
          /*      tx_start_time = NETSTACK_RADIO_read_sfd_timer(); */
#else
          tx_start_time = current_link_start + TsTxOffset;
#endif
          /* calculate TX duration based on sent packet len */
          tx_duration = TSCH_PACKET_DURATION(payload_len);
          /* limit tx_time to its max value */
          tx_duration = MIN(tx_duration, TSCH_DATA_MAX_DURATION);
          /* turn tadio off -- will turn on again to wait for ACK if needed */
          off();
          t0tx = RTIMER_NOW() - t0tx;

          t0txack = RTIMER_NOW();
          if(mac_tx_status == RADIO_TX_OK) {
            if(!is_broadcast) {
              uint8_t ackbuf[TSCH_ACK_LEN];
              int ack_len;
              int is_nack;
              int ret;
              int32_t received_drift;
              rtimer_clock_t ack_start_time;
              int is_time_source;

              /* Disabling address decoding so the radio accepts the enhanced ACK */
              NETSTACK_RADIO_address_decode(0);
              /* Unicast: wait for ack after tx: sleep until ack time */
              TSCH_SCHEDULE_AND_YIELD(pt, t, tx_start_time,
                  tx_duration + TsTxAckDelay - TsShortGT - delayRx);
              on();
              /* Wait for ACK to come */
              BUSYWAIT_UNTIL_ABS(NETSTACK_RADIO.receiving_packet(),
                  tx_start_time, tx_duration + TsTxAckDelay + TsShortGT);

#if TSCH_USE_SFD_FOR_SYNC
              ack_start_time = RTIMER_NOW();
              /* TODO test this on H/W */
              /*          ack_start_time = NETSTACK_RADIO_read_sfd_timer(); */
#else
              ack_start_time = RTIMER_NOW();
#endif
              /* Wait for ACK to finish */
              BUSYWAIT_UNTIL_ABS(!NETSTACK_RADIO.receiving_packet(),
                  ack_start_time, TSCH_ACK_MAX_DURATION);
              off();
              /* Enabling address decoding again so the radio filters data packets */
              NETSTACK_RADIO_address_decode(1);

              /* Read ack frame */
              ack_len = NETSTACK_RADIO.read((void *)ackbuf, TSCH_ACK_LEN);

              is_time_source = current_neighbor != NULL && current_neighbor->is_time_source;
              received_drift = 0;
              ret = tsch_packet_parse_sync_ack(&received_drift, &is_nack,
                  ackbuf, ack_len, seqno, is_time_source);

              if(ret & TSCH_ACK_OK) {
                if(is_time_source && (ret & TSCH_ACK_HAS_SYNC_IE)) {
#if TRUNCATE_SYNC_IE
                  if(received_drift > TRUNCATE_SYNC_IE_BOUND) {
                    drift_correction = TRUNCATE_SYNC_IE_BOUND;
                  } else if(received_drift < -TRUNCATE_SYNC_IE_BOUND) {
                    drift_correction = -TRUNCATE_SYNC_IE_BOUND;
                  } else {
                    drift_correction = received_drift;
                  }
                  if(drift_correction != received_drift) {
                    TSCH_LOG_ADD(tsch_log_message,
                        snprintf(log->message, sizeof(log->message),
                            "!truncated dr %d %d", (int)received_drift, (int)drift_correction);
                    );
                  }
#else /* TRUNCATE_SYNC_IE */
                  drift_correction = received_drift;
#endif /* TRUNCATE_SYNC_IE */
                  drift_neighbor = current_neighbor;
                  /* Keep track of sync time */
                  last_sync_asn = current_asn;
                  tsch_schedule_keepalive();
                }
                mac_tx_status = MAC_TX_OK;
              } else {
                mac_tx_status = MAC_TX_NOACK;
              }
            } else {
              mac_tx_status = MAC_TX_OK;
            }
          } else {
            mac_tx_status = MAC_TX_ERR;
          }
        }
      }
    }
    t0txack = RTIMER_NOW() - t0txack;

    current_packet->transmissions++;
    current_packet->ret = mac_tx_status;

    /* Post TX: Update neighbor state */
    in_queue = update_neighbor_state(current_neighbor, current_packet, current_link, mac_tx_status);

    /* The packet was dequeued, i.e. successfully sent or dropped.
     * Call upper layer callback. */
    if(in_queue == 0) {
      dequeued_array[dequeued_index] = current_packet;
      ringbufindex_put(&dequeued_ringbuf);
      process_poll(&tsch_pending_events_process);
    }

    /* Log every tx attempt */
    TSCH_LOG_ADD(tsch_log_tx,
        log->tx.mac_tx_status = mac_tx_status;
    log->tx.num_tx = current_packet->transmissions;
    log->tx.datalen = queuebuf_datalen(current_packet->qb);
    log->tx.drift = drift_correction;
    log->tx.drift_used = drift_neighbor != NULL;
    log->tx.is_data =
        (tsch_packet_parse_frame_type_from_fcf_lsb(((uint8_t*)queuebuf_dataptr(current_packet->qb))[0])
            & IS_DATA) != 0;
    log->tx.dest = LOG_NODEID_FROM_LINKADDR(queuebuf_addr(current_packet->qb, PACKETBUF_ADDR_RECEIVER));
    appdata_copy(&log->tx.appdata, LOG_APPDATAPTR_FROM_BUFFER(queuebuf_dataptr(current_packet->qb), queuebuf_datalen(current_packet->qb)));
    );
  }

  PT_END(pt);
}

static
PT_THREAD(tsch_rx_link(struct pt *pt, struct rtimer *t))
{
  /**
   * RX link:
   * 1. Check if it is used for TIME_KEEPING
   * 2. Sleep and wake up just before expected RX time (with a guard time: TsLongGT)
   * 3. Check for radio activity for the guard time: TsLongGT
   * 4. Prepare and send ACK if needed
   * 5. Drift calculated in the ACK callback registered with the radio driver. Use it if receiving from a time source neighbor.
   **/

  struct tsch_neighbor *n;
  static linkaddr_t source_address;
  static linkaddr_t destination_address;
  static int16_t input_index;
  static int input_queue_drop = 0;

  PT_BEGIN(pt);

  //TODO receive the packet and send NACK if we don't have buffer space!
  input_index = ringbufindex_peek_put(&input_ringbuf);
  if(input_index == -1) {
    input_queue_drop++;
  } else {
    static struct input_packet *current_input;
    /* Estimated drift based on RX time */
    static int32_t estimated_drift;
    /* Rx timestamps */
    static rtimer_clock_t rx_start_time;
    static rtimer_clock_t rx_end_time;
    static rtimer_clock_t expected_rx_time;

    expected_rx_time = current_link_start + TsTxOffset;
    /* Default start time: expected Rx time */
    rx_start_time = expected_rx_time;

    t0rx = RTIMER_NOW();

    current_input = &input_array[input_index];

    /* Wait before starting to listen */
    TSCH_SCHEDULE_AND_YIELD(pt, t, current_link_start, TsTxOffset - TsLongGT - delayRx);

    /* Start radio for at least guard time */
    on();
    if(!NETSTACK_RADIO.receiving_packet()) {
      /* Check if receiving within guard time */
      BUSYWAIT_UNTIL_ABS(NETSTACK_RADIO.receiving_packet(),
          current_link_start, TsTxOffset + TsLongGT);
      /* Save packet timestamp,
       * XXX it seems that RTIMER gives better sync than SFD timer both on NXP and SKY */
      rx_start_time = RTIMER_NOW();
    }
    if(!NETSTACK_RADIO.receiving_packet() && !NETSTACK_RADIO.pending_packet()) {
      off();
      t0rx = RTIMER_NOW() - t0rx;
      /* no packets on air */
    } else {
      uint8_t seqno;

      /* Wait until packet is received, turn radio off */
      BUSYWAIT_UNTIL_ABS(!NETSTACK_RADIO.receiving_packet(),
          current_link_start, TsTxOffset + TsLongGT + TSCH_DATA_MAX_DURATION);
      /* XXX it seems that RTIMER gives better sync than SFD timer both on NXP and SKY */
#if TSCH_USE_SFD_FOR_SYNC
      /* Save packet timestamp */
      rx_start_time = NETSTACK_RADIO_read_sfd_timer();
#endif /* TSCH_USE_SFD_FOR_SYNC */

      off();

      if(NETSTACK_RADIO.pending_packet()) {
        static int ack_needed;
        static int frame_valid;
        /* Values provided by the radio */
        extern signed char radio_last_rssi;
        extern uint8_t radio_last_correlation;
        /* Read packet */
        current_input->len = NETSTACK_RADIO.read((void *)current_input->payload, TSCH_MAX_PACKET_LEN);
        current_input->rx_asn = current_asn;
        current_input->rssi = radio_last_rssi + RSSI_CORRECTION_CONSTANT;
        ack_needed = tsch_packet_parse_frame_type((uint8_t *)current_input->payload, current_input->len, &seqno) & DO_ACK;
        frame_valid = tsch_packet_extract_addresses((uint8_t*)current_input->payload,
            current_input->len, &source_address, &destination_address);
        rx_end_time = rx_start_time + TSCH_PACKET_DURATION(current_input->len);

        t0rx = RTIMER_NOW() - t0rx;
        t0rxack = RTIMER_NOW();

        if(frame_valid) {
          if(linkaddr_cmp(&destination_address, &linkaddr_node_addr)
              || linkaddr_cmp(&destination_address, &linkaddr_null)) {
            int do_nack = 0;
            estimated_drift = ((int32_t)expected_rx_time - (int32_t)rx_start_time);

#ifdef TSCH_CALLBACK_DO_NACK
            if(ack_needed) {
              do_nack = TSCH_CALLBACK_DO_NACK(current_link,
                  &source_address, &destination_address);
            }
#endif

            if(ack_needed) {
              static uint8_t ack_buf[TSCH_ACK_LEN];
              static int ack_len;

              /* Build ACK frame */
              ack_len = tsch_packet_make_sync_ack(
                  estimated_drift, do_nack,
                  ack_buf, sizeof(ack_buf), &source_address, seqno);
              /* Copy to radio buffer */
              NETSTACK_RADIO.prepare((const void *)ack_buf, ack_len);

              /* Wait for time to ACK and transmit ACK */
              TSCH_SCHEDULE_AND_YIELD(pt, t, rx_end_time, TsTxAckDelay - delayTx);
              NETSTACK_RADIO.transmit(ack_len);
            }

            /* If the sender is a time source, proceed to clock drift compensation */
            n = tsch_queue_get_nbr(&source_address);
            if(n != NULL && n->is_time_source) {
              /* Keep track of last sync time */
              last_sync_asn = current_asn;
              /* Save estimated drift */
              drift_correction = -estimated_drift;
              drift_neighbor = n;
              tsch_schedule_keepalive();
            }

#if WITH_APP_PROBING
            app_probing_received(LOG_APPDATAPTR_FROM_BUFFER(current_input->payload, current_input->len));
#else
            /* Add current input to ringbuf and set ctimer for later processing */
            ringbufindex_put(&input_ringbuf);
            process_poll(&tsch_pending_events_process);
#endif /* WITH_APP_PROBING */

            /* Log every reception */
            TSCH_LOG_ADD(tsch_log_rx,
              log->rx.src = LOG_NODEID_FROM_LINKADDR(&source_address);
              log->rx.is_unicast = ack_needed;
              log->rx.datalen = current_input->len;
              log->rx.drift = drift_correction;
              log->rx.drift_used = drift_neighbor != NULL;
              log->rx.is_data =
                  (tsch_packet_parse_frame_type(current_input->payload, current_input->len, NULL)
                  & IS_DATA) != 0;
              log->rx.estimated_drift = estimated_drift;
              appdata_copy(&log->rx.appdata, LOG_APPDATAPTR_FROM_BUFFER(current_input->payload, current_input->len));
            );
          } else {
            TSCH_LOG_ADD(tsch_log_message,
                  snprintf(log->message, sizeof(log->message),
                      "!not for us %x:%x:%x:%x",
                      destination_address.u8[4], destination_address.u8[5],
                      destination_address.u8[6], destination_address.u8[7]);
            );
          }
        }
      }
    }

    t0rxack = RTIMER_NOW() - t0rxack;
    if(input_queue_drop != 0) {
      TSCH_LOG_ADD(tsch_log_message,
          snprintf(log->message, sizeof(log->message),
              "!queue full skipped %u", input_queue_drop);
      );
      input_queue_drop = 0;
    }
  }

  PT_END(pt);
}

/* Protothread for link operation, called from rtimer interrupt
 * and scheduled from tsch_schedule_link_operation */
static
PT_THREAD(tsch_link_operation(struct rtimer *t, void *ptr))
{
  PT_BEGIN(&link_operation_pt);

  /* Loop over all active links */
  while(associated) {

    if(current_link == NULL || tsch_lock_requested) { /* Skip link operation if there is no link
                                                          or if there is a pending request for getting the lock */
      /* Issue a log whenever skipping a link */
      TSCH_LOG_ADD(tsch_log_message,
                      snprintf(log->message, sizeof(log->message),
                          "!skipped link %u %u %u",
                            tsch_locked,
                            tsch_lock_requested,
                            current_link == NULL);
      );

    } else {
      tsch_in_link_operation = 1;
      /* Get a packet ready to be sent */
      current_packet = get_packet_and_neighbor_for_link(current_link, &current_neighbor);
      /* Hop channel */
      hop_channel(&current_asn, current_link->channel_offset);
      /* Reset drift correction */
      drift_correction = 0;
      drift_neighbor = NULL;
      /* Decide whether it is a TX/RX/IDLE or OFF link */
      /* Actual slot operation */
      if(current_packet != NULL) {
        /* We have something to transmit, do the following:
         * 1. send
         * 2. update_backoff_state(current_neighbor)
         * 3. post tx callback
         **/
        static struct pt link_tx_pt;
        PT_SPAWN(&link_operation_pt, &link_tx_pt, tsch_tx_link(&link_tx_pt, t));
      } else if((current_link->link_options & LINK_OPTION_RX)) {
        /* Listen */
        static struct pt link_rx_pt;
        PT_SPAWN(&link_operation_pt, &link_rx_pt, tsch_rx_link(&link_rx_pt, t));
      }
    }

    /* End of slot operation, schedule next slot or resynchronize */

    /* Do we need to resynchronize? i.e., wait for EB again */
    if(!tsch_is_coordinator && (ASN_DIFF(current_asn, last_sync_asn) > TSCH_CLOCK_TO_SLOTS(TSCH_DESYNC_THRESHOLD))) {
      TSCH_LOG_ADD(tsch_log_message,
            snprintf(log->message, sizeof(log->message),
                "! leaving the network, last sync %u\n",
                          (unsigned)ASN_DIFF(current_asn, last_sync_asn));
      );
      associated = 0;
      process_post(&tsch_process, PROCESS_EVENT_POLL, NULL);
    } else {
      /* backup of drift correction for printing debug messages */
      /* int32_t drift_correction_backup = drift_correction; */
      uint16_t timeslot_diff = 0;
      rtimer_clock_t prev_link_start;
      /* Schedule next wakeup skipping slots if missed deadline */
      do {
        if(current_link != NULL
            && current_link->link_options & LINK_OPTION_TX
            && current_link->link_options & LINK_OPTION_SHARED) {
          /* Decrement the backoff window for all neighbors able to transmit over
           * this Tx, Shared link. */
          tsch_queue_update_all_backoff_windows(&current_link->addr);
        }

        /* Get next active link */
        current_link = tsch_schedule_get_next_active_link(&current_asn, &timeslot_diff);
        if(current_link == NULL) {
          /* There is no next link. Fall back to default
           * behavior: wake up at the next timeslot. */
          timeslot_diff = 1;
        }
        /* Update ASN */
        ASN_INC(current_asn, timeslot_diff);
        /* Time to next wake up */
        tsch_time_until_next_active_link = timeslot_diff * TsSlotDuration + drift_correction;
        drift_correction = 0;
        drift_neighbor = NULL;
        /* Update current link start */
        prev_link_start = current_link_start;
        current_link_start += tsch_time_until_next_active_link;
      } while(!tsch_schedule_link_operation(t, prev_link_start, tsch_time_until_next_active_link, 1));

      /* Drift correction monitoring */
      //PRINTF("TSCH: end of cell, drift correction: %d ticks, next wake up: %u slots\n", (int16_t)drift_correction_backup, timeslot_diff);
      /* Timing profiling of various parts of TSCH link operation */
      // PRINTF("TSCH: timing: TX_prepare %u, TX %u, TX_ack %u, post_TX %u, RX %u, RX_ack %u\n", t0prepare, t0tx, t0txack, t0post_tx, t0rx, t0rxack);
      /* Reset time-profiling variables for next wake up */
      t0prepare=0; t0tx=0; t0txack=0; t0post_tx=0; t0rx=0; t0rxack=0;
      #if DEBUG_INJECT_DRIFT
      #include "node-id.h"
      /* inject drift to test drift correction */
      current_link_start -= node_id & 0x7;
      #endif /* INJECT_DRIFT */
    }

    tsch_in_link_operation = 0;
    PT_YIELD(&link_operation_pt);
  }

  PT_END(&link_operation_pt);
}

/* Associate:
 * If we are a master, start right away.
 * Otherwise, wait for EBs to associate with a master
 */
static
PT_THREAD(tsch_associate(struct pt *pt))
{
  PT_BEGIN(pt);

  ASN_INIT(current_asn, 0, 0);

  if(tsch_is_coordinator) {
    /* We are coordinator, start operating now */

    associated = 1;
    tsch_join_priority = 0;

    //LOG("TSCH: starting, asn-%x.%lx\n",
      //                current_asn.ms1b, current_asn.ls4b);

    current_link_start = RTIMER_NOW() + 20*RTIMER_MIN_DELAY;
    /* Actually start only after a short time, as required by rtimer
     * TODO: why 20*RTIMER_MIN_DELAY? -> Only initial delay needs to be big */
    tsch_time_until_next_active_link = 20*RTIMER_MIN_DELAY;
  } else {
    static struct etimer associate_timer;
    static uint32_t base_channel;
    base_channel = random_rand();
    etimer_set(&associate_timer, CLOCK_SECOND / 100);

    while(!associated) {
      /* We are not coordinator, try to associate */
      rtimer_clock_t t0;
      int is_packet_pending = 0;

      /* Hop to any channel offset */
      hop_channel(&current_asn, base_channel + clock_seconds());

      /* Turn radio on and wait for EB */
      NETSTACK_RADIO_radio_raw_rx_on();

      /* Busy wait for a packet for 1 second */
      t0 = RTIMER_NOW();

      is_packet_pending = NETSTACK_RADIO.pending_packet();
      if(!is_packet_pending && NETSTACK_RADIO.receiving_packet()) {
        /* If we are currently receiving a packet, wait until end of reception */
        //TODO the wait time should be a conf parameter
        BUSYWAIT_UNTIL_ABS((is_packet_pending = NETSTACK_RADIO.pending_packet()), t0, RTIMER_SECOND / 100);
      }

      if(is_packet_pending) {
        linkaddr_t source_address;
        int eb_parsed = 0;

        /* Save packet timestamp */
        t0 = NETSTACK_RADIO_read_sfd_timer();

        /* Wait until reception is finished
         * TODO the following line shouldn't be there. Check this. */
        // BUSYWAIT_UNTIL_ABS(!NETSTACK_RADIO.receiving_packet(), t0, TSCH_DATA_MAX_DURATION);

        /* Read packet */
        input_eb.len = NETSTACK_RADIO.read(input_eb.payload, TSCH_MAX_PACKET_LEN);

        if(input_eb.len != 0) {
          /* Parse EB and extract ASN and join priority */
          eb_parsed = tsch_parse_eb(input_eb.payload, input_eb.len,
              &source_address, &current_asn, &tsch_join_priority);
        }

#if TSCH_CHECK_TIME_AT_ASSOCIATION > 0
        if(eb_parsed != 0) {
          /* Divide by 4k and multiply again to avoid integer overflow */
          uint32_t expected_asn = 4096*TSCH_CLOCK_TO_SLOTS(clock_time()/4096); /* Expected ASN based on our current time*/
          int32_t asn_threshold = TSCH_CHECK_TIME_AT_ASSOCIATION*60ul*TSCH_CLOCK_TO_SLOTS(CLOCK_SECOND);
          int32_t asn_diff = (int32_t)current_asn.ls4b-expected_asn;
          if(asn_diff > asn_threshold) {
            //LOG("TSCH:! EB ASN rejected %lx %lx %ld\n",
              //current_asn.ls4b, expected_asn, asn_diff);
            eb_parsed = 0;
          }
        }
#endif

        if(eb_parsed != 0 && tsch_join_priority < TSCH_MAX_JOIN_PRIORITY) {
          struct tsch_neighbor *n;

          /* Add coordinator to list of neighbors, lock the entry */
          n = tsch_queue_add_nbr(&source_address);

          if(n != NULL) {
            tsch_queue_update_time_source(&source_address);

            /* Use this ASN as "last synchronization ASN" */
            last_sync_asn = current_asn;
            tsch_schedule_keepalive();

            /* Calculate TSCH link start from packet timestamp */
            current_link_start = t0 - TsTxOffset;

            /* Make our join priority 1 plus what we received.
             * TODO: add a hook for the upper layer (e.g. TSCH) to set the priority */
            tsch_join_priority++;

            /* Update global flags */
            associated = 1;

#ifdef TSCH_CALLBACK_JOINING_NETWORK
            TSCH_CALLBACK_JOINING_NETWORK();
#endif

            /* TODO: Verify if tsch_nbrs are created and timesources are set */
//            LOG("TSCH: association done, asn-%x.%lx, jp %u, from %u, time source %u\n",
//                current_asn.ms1b, current_asn.ls4b, tsch_join_priority,
//                LOG_NODEID_FROM_LINKADDR(&source_address),
//                LOG_NODEID_FROM_LINKADDR(&tsch_queue_get_time_source()->addr));
          }
        }
      }

      if(associated) {
        /* End of association turn the radio off */
        off();
      } else {
        etimer_reset(&associate_timer);
        PT_WAIT_UNTIL(pt, etimer_expired(&associate_timer));
      }
    }
  }

  /* Association done, schedule keepalive messages */
  tsch_schedule_keepalive();

  PT_END(pt);
}

/* The main TSCH process */
PROCESS_THREAD(tsch_process, ev, data)
{
  static struct pt associate_pt;
  static struct rtimer link_operation_timer;
  rtimer_clock_t prev_link_start;

  PROCESS_BEGIN();

  while(1) {
    /* Associate:
     * Try to associate to a network or start one if node is TSCH coordinator */
    while(!associated) {
      PROCESS_PT_SPAWN(&associate_pt, tsch_associate(&associate_pt));
    }

    association_time = clock_seconds();
    tsch_current_eb_period = TSCH_MIN_EB_PERIOD;

    /* TODO: make queues and data structures
     * from received EB */

    PRINTF("TSCH: scheduling initial link operation: asn-%x.%lx, start: %u, now: %u\n", current_asn.ms1b, current_asn.ls4b, current_link_start, RTIMER_NOW());

    /* Schedule next slot */
    do {
      uint16_t timeslot_diff;
      /* Get next active link */
      current_link = tsch_schedule_get_next_active_link(&current_asn, &timeslot_diff);
      /* Update ASN */
      ASN_INC(current_asn, timeslot_diff);
      /* Time to next wake up */
      tsch_time_until_next_active_link = timeslot_diff * TsSlotDuration;
      /* Update current link start */
      prev_link_start = current_link_start;
      current_link_start += tsch_time_until_next_active_link;
    } while(!tsch_schedule_link_operation(&link_operation_timer, prev_link_start, tsch_time_until_next_active_link, 1));

    PROCESS_YIELD_UNTIL(!associated);

    /* Resynchronize */
    LOG("TSCH: will re-synchronize\n");
    off();
    tsch_reset();
  }

  PROCESS_END();
}

/* A process that is polled from interrupt and calls tx/rx input
 * callbacks, outputs pending logs. */
PROCESS_THREAD(tsch_pending_events_process, ev, data)
{
  PROCESS_BEGIN();
  while(1) {
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    tsch_rx_process_pending();
    tsch_tx_process_pending();
    tsch_log_process_pending();
  }
  PROCESS_END();
}

/* Pass sent packets to upper layer */
static void
tsch_tx_process_pending()
{
  int16_t dequeued_index;
  /* Loop on accessing (without removing) a pending input packet */
  while((dequeued_index = ringbufindex_peek_get(&dequeued_ringbuf)) != -1) {
    struct tsch_packet *p = dequeued_array[dequeued_index];
    /* Put packet into packetbuf for packet_sent callback */
    queuebuf_to_packetbuf(p->qb);
    /* Call packet_sent callback */
    mac_call_sent_callback(p->sent, p->ptr, p->ret, p->transmissions);
    /* Free packet queuebuf */
    tsch_queue_free_packet(p);
    /* Free all unused neighbors */
    tsch_queue_free_unused_neighbors();
    /* Remove dequeued packet from ringbuf */
    ringbufindex_get(&dequeued_ringbuf);
  }
}

/* Process pending input packet(s) */
static void
tsch_rx_process_pending()
{
  int16_t input_index;
  /* Loop on accessing (without removing) a pending input packet */
  while((input_index = ringbufindex_peek_get(&input_ringbuf)) != -1) {
    struct input_packet *current_input = &input_array[input_index];
    int is_data = (tsch_packet_parse_frame_type(current_input->payload, current_input->len, NULL) & IS_DATA) != 0;
    if(is_data) {
      /* Skip EBs and other control messages */
      /* Copy to packetbuf for processing by upper layers */

      /* if radio parses 15.4 header in HW,
       * use its specific function to copy to packetbuf
       * (and skip SW parser) */
#if RADIO_PARSE_MAC_HW
      micromac_copy_mac_frame_to_packetbuf(current_input->payload);
#else
      packetbuf_copyfrom(current_input->payload, current_input->len);
#endif
      packetbuf_set_attr(PACKETBUF_ATTR_RSSI, current_input->rssi);
    }

    /* Remove input from ringbuf */
    ringbufindex_get(&input_ringbuf);

    if(is_data) {
      /* Pass to upper layers */
      packet_input();
    } else {
      /* LOG("TSCH: EB received\n"); */
      linkaddr_t source_address;
      struct asn_t eb_asn;
      uint8_t eb_join_priority;
      /* Verify incoming EB (does its ASN match our Rx time?),
       * and update our join priority. */

      if(tsch_parse_eb(current_input->payload, current_input->len,
                    &source_address, &eb_asn, &eb_join_priority)) {

#if TSCH_EB_AUTOSELECT
        if(!tsch_is_coordinator) {
          /* Maintain EB received counter for every neighbor */
          struct eb_stat *stat = (struct eb_stat *)nbr_table_get_from_lladdr(eb_stats, &source_address);
          if(stat == NULL) {
            stat = (struct eb_stat *)nbr_table_add_lladdr(eb_stats, &source_address);
          }
          if(stat != NULL) {
            stat->rx_count++;
            stat->jp = eb_join_priority;
            best_neighbor_eb_count = MAX(best_neighbor_eb_count, stat->rx_count);
          }
          /* Select best time source */
          struct eb_stat *best_stat = NULL;
          stat = nbr_table_head(eb_stats);
          while(stat != NULL) {
            /* Is neighbor eligible as a time source? */
            if(stat->rx_count > best_neighbor_eb_count / 2) {
              if(best_stat == NULL ||
                  stat->jp < best_stat->jp) {
                best_stat = stat;
              }
            }
            stat = nbr_table_next(eb_stats, stat);
          }
          /* Update time source */
          if(best_stat != NULL) {
            tsch_queue_update_time_source(nbr_table_get_lladdr(eb_stats, best_stat));
            tsch_join_priority = best_stat->jp + 1;
          }
        }
#endif

        struct tsch_neighbor *n = tsch_queue_get_time_source();
        /* Did the EB come from our time source? */
        if(n != NULL && linkaddr_cmp(&source_address, &n->addr)) {
          /* Check for ASN drift */
          int32_t asn_diff = ASN_DIFF(current_input->rx_asn, eb_asn);
          if(asn_diff != 0) {
            /* We first need to take the lock, i.e. make sure no link operation inteferes with us */
            if(tsch_get_lock()) {
              /* Abort next link operation; it was scheduled as per a drifted ASN */
              current_link = NULL;
            }
            /* Update ASN */
            if(asn_diff > 0) {
              /* The diff is positive, i.e. our ASN is too high */
              ASN_DEC(current_asn, asn_diff);
            } else {
              /* The diff is negative, i.e. our ASN is too low */
              ASN_INC(current_asn, -asn_diff);
            }
            last_sync_asn = current_asn;
            tsch_release_lock();
            LOG("TSCH: corrected ASN by %ld\n", asn_diff);
          }

          /* Update join priority */
          if(eb_join_priority < TSCH_MAX_JOIN_PRIORITY) {
            if(tsch_join_priority != eb_join_priority + 1) {
              LOG("TSCH: update JP from EB %u -> %u\n",
                  tsch_join_priority, eb_join_priority + 1);
              tsch_join_priority = eb_join_priority + 1;
            }
          } else {
            /* Join priority unacceptable. Leave network. */
            LOG("TSCH:! EB JP too high %u, leaving the network\n",
                eb_join_priority);
            associated = 0;
            process_post(&tsch_process, PROCESS_EVENT_POLL, NULL);
          }
        }
      }
    }
  }
}

void
tsch_set_eb_period(uint32_t period)
{
  /* Stick to the minimum period in the first minute after association */
  if(clock_seconds() > association_time + 60) {
    /* Keep the period within boundaries */
    period = MAX(period, TSCH_MIN_EB_PERIOD);
    period = MIN(period, TSCH_MAX_EB_PERIOD);
    /* Update EB period */
    tsch_current_eb_period = period;
  } else {
    tsch_current_eb_period = TSCH_MIN_EB_PERIOD;
  }
}

/* A periodic process to send TSCH Enhanced Beacons (EB) */
PROCESS_THREAD(tsch_send_eb_process, ev, data)
{
  static struct etimer eb_timer;

  PROCESS_BEGIN();

  /* Wait until association */
  etimer_set(&eb_timer, CLOCK_SECOND / 10);
  while(!associated) {
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
    etimer_reset(&eb_timer);
  }

  /* Set an initial delay except for coordinator, which should send an EB asap */
  if(!tsch_is_coordinator) {
    etimer_set(&eb_timer, random_rand() % tsch_current_eb_period);
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
  }

  while(1) {
    unsigned long delay;

    if(associated) {
      /* Enqueue EB only if there isn't already one in queue */
      if(tsch_queue_packet_count(&tsch_eb_address) == 0) {
        int eb_len;
        /* Prepare the EB packet and schedule it to be sent */
        packetbuf_clear();
        /* We don't use seqno 0 */
        if(++tsch_packet_seqno == 0) {
          tsch_packet_seqno++;
        }
        packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, tsch_packet_seqno);
        eb_len = tsch_packet_make_eb(packetbuf_dataptr(), PACKETBUF_SIZE, tsch_packet_seqno);
        if(eb_len != 0) {
          packetbuf_set_datalen(eb_len);
          /* Enqueue EB packet */
          if(!tsch_queue_add_packet(&tsch_eb_address, NULL, NULL)) {
//            LOG("TSCH:! could not enqueue EB packet\n");
          } else {
//            LOG("TSCH: enqueue EB packet\n");
          }
        }
      }
    }
    /* Next EB transmission with a random delay
     * within [tsch_current_eb_period*0.9, tsch_current_eb_period[ */
    delay = (tsch_current_eb_period - tsch_current_eb_period/10)
        + random_rand() % (tsch_current_eb_period/10);
    etimer_set(&eb_timer, delay);
    PROCESS_WAIT_UNTIL(etimer_expired(&eb_timer));
  }
  PROCESS_END();
}
/* Brief dump of the TSCH state */
void
tsch_dump_status()
{
  printf("TSCH-dump %lx %u %u %u %u %u\n",
      current_asn.ls4b,
      tsch_locked, tsch_lock_requested, tsch_in_link_operation,
      current_link != NULL ? current_link->slotframe_handle : 0xffff,
          current_link != NULL ? current_link->channel_offset : 0xffff
  );
  tsch_log_process_pending();
}
/*---------------------------------------------------------------------------*/
static void
tsch_reset(void)
{
  /* First make sure pending packet callbacks are sent etc */
  process_post_synch(&tsch_pending_events_process, PROCESS_EVENT_POLL, NULL);
  /* Empty all neighbor queues */
  /* tsch_queue_flush_all(); */
  /* Remove unused neighbors */
  tsch_queue_free_unused_neighbors();
  tsch_queue_update_time_source(NULL);
  /* Initialize global variables */
  tsch_join_priority = 0xff;
  ASN_INIT(current_asn, 0, 0);
  current_link = NULL;
  current_packet = NULL;
  current_neighbor = NULL;
#ifdef TSCH_CALLBACK_LEAVING_NETWORK
  TSCH_CALLBACK_LEAVING_NETWORK();
#endif
#if TSCH_EB_AUTOSELECT
  best_neighbor_eb_count = 0;
  nbr_table_register(eb_stats, NULL);
#endif
  /* Reset time-profiling variables for next wake up */
  t0prepare=0; t0tx=0; t0txack=0; t0post_tx=0; t0rx=0; t0rxack=0;
}
/*---------------------------------------------------------------------------*/
static void
tsch_init(void)
{
  /* Disable radio interrupts so they do not interfere with RTIMER interrupts
   * Radio will be polled instead */
  NETSTACK_RADIO_set_interrupt_enable(0);

  leds_blink();

  /* save start sfd only */
  NETSTACK_RADIO_sfd_sync(1, 0);
  /* Init TSCH sub-modules */
  tsch_reset();
  tsch_queue_init();
  tsch_schedule_init();
  tsch_log_init();
  ringbufindex_init(&input_ringbuf, TSCH_MAX_INCOMING_PACKETS);
  ringbufindex_init(&dequeued_ringbuf, DEQUEUED_ARRAY_SIZE);
  ASN_DIVISOR_INIT(hopping_sequence_length, TSCH_N_CHANNELS);
  /* Process tx/rx callback and log messages whenever polled */
  process_start(&tsch_pending_events_process, NULL);
}
/*---------------------------------------------------------------------------*/
static int
turn_on(void)
{
  /* tsch_is_coordinator must be set (or unset) before calling this */

  /* periodically send TSCH EBs */
  process_start(&tsch_send_eb_process, NULL);
  /* try to associate to a network or start one if setup as RPL root */
  process_start(&tsch_process, NULL);
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
turn_off(int keep_radio_on)
{
  PRINTF("TSCH: turn_off not supported\n");
  return 1;
}
/*---------------------------------------------------------------------------*/
const struct mac_driver tschmac_driver = {
  "TSCH",
  tsch_init,
  send_packet,
  packet_input,
  turn_on,
  turn_off,
  channel_check_interval,
};
/*---------------------------------------------------------------------------*/
