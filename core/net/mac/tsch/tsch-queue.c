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
 *         Per-neighbor packet queues for TSCH MAC.
 *         The list of neighbor uses the TSCH lock lock, but per-neighbor packet array are lockfree.
 *				 Read-only operation on neighbor and packets are allowed from interrupts and outside of them.
 *				 *Other operations are allowed outside of interrupt only.*
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 *         Simon Duquennoy <simonduq@sics.se>
 */

#include "contiki.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "net/queuebuf.h"
#include "net/mac/rdc.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#include <string.h>

#ifdef TSCH_CALLBACK_NEW_TIME_SOURCE
void TSCH_CALLBACK_NEW_TIME_SOURCE(struct tsch_neighbor *old, struct tsch_neighbor *new);
#endif

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

/* We have as many packets are there are queuebuf in the system */
MEMB(packet_memb, struct tsch_packet, QUEUEBUF_NUM);
MEMB(neighbor_memb, struct tsch_neighbor, TSCH_QUEUE_MAX_NEIGHBOR_QUEUES);
LIST(neighbor_list);

/* Broadcast and EB virtual neighbors */
struct tsch_neighbor *n_broadcast;
struct tsch_neighbor *n_eb;

/**
 *  A pseudo-random generator with better properties than msp430-libc's default
 **/

static uint32_t tsch_random_seed;

static void
tsch_random_init(uint32_t x)
{
  tsch_random_seed = x;
}
static uint8_t
tsch_random_byte(uint8_t window)
{
  tsch_random_seed = tsch_random_seed * 1103515245 + 12345;
  return ((uint32_t)(tsch_random_seed / 65536) % 32768) & window;
}
/* Add a TSCH neighbor */
struct tsch_neighbor *
tsch_queue_add_nbr(const linkaddr_t *addr)
{
  struct tsch_neighbor *n = NULL;
  /* If we have an entry for this neighbor already, we simply update it */
  n = tsch_queue_get_nbr(addr);
  if(n == NULL) {
    if(tsch_get_lock()) {
      /* Allocate a neighbor */
      n = memb_alloc(&neighbor_memb);
      if(n != NULL) {
        /* Initialize neighbor entry */
        memset(n, 0, sizeof(struct tsch_neighbor));
        ringbufindex_init(&n->tx_ringbuf, TSCH_QUEUE_NUM_PER_NEIGHBOR);
        linkaddr_copy(&n->addr, addr);
        n->is_broadcast = linkaddr_cmp(addr, &tsch_eb_address)
                    || linkaddr_cmp(addr, &tsch_broadcast_address);
        tsch_queue_backoff_reset(n);
        /* Add neighbor to the list */
        list_add(neighbor_list, n);
      }
      tsch_release_lock();
    }
    if(n == NULL) {
      PRINTF("TSCH-queue:! add nbr failed: %p %u ", n, tsch_is_locked());
      PRINTLLADDR((const void *)addr);
      PRINTF("\n");
    } else {
      PRINTF("TSCH-queue: added nbr %p ", n);
      PRINTLLADDR((const void *)addr);
      PRINTF("\n");
    }
  }
  return n;
}
/* Get a TSCH neighbor */
struct tsch_neighbor *
tsch_queue_get_nbr(const linkaddr_t *addr)
{
  if(!tsch_is_locked()) {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while(n != NULL) {
      if(linkaddr_cmp(&n->addr, addr)) {
        return n;
      }
      n = list_item_next(n);
    }
  }
  return NULL;
}
/* Get a TSCH time source (we currently assume there is only one) */
struct tsch_neighbor *
tsch_queue_get_time_source()
{
  if(!tsch_is_locked()) {
    struct tsch_neighbor *curr_nbr = list_head(neighbor_list);
    while(curr_nbr != NULL) {
      if(curr_nbr->is_time_source) {
        return curr_nbr;
      }
      curr_nbr = list_item_next(curr_nbr);
    }
  }
  return NULL;
}

/* Update TSCH time source */
int
tsch_queue_update_time_source(const linkaddr_t *new_addr)
{
  if(!tsch_is_locked()) {
    if(!tsch_is_coordinator) {
      struct tsch_neighbor *old_time_src = tsch_queue_get_time_source();
      struct tsch_neighbor *new_time_src = new_addr ? tsch_queue_add_nbr(new_addr) : NULL;

      if(new_time_src != old_time_src) {
        LOG("TSCH: update time source: %u -> %u\n",
            LOG_NODEID_FROM_LINKADDR(old_time_src ? &old_time_src->addr : NULL),
            LOG_NODEID_FROM_LINKADDR(new_time_src ? &new_time_src->addr : NULL));

        /* Update time source */
        if(new_time_src != NULL) {
          new_time_src->is_time_source = 1;
        }

        if(old_time_src != NULL) {
          old_time_src->is_time_source = 0;
        }

#ifdef TSCH_CALLBACK_NEW_TIME_SOURCE
        TSCH_CALLBACK_NEW_TIME_SOURCE(old_time_src, new_time_src);
#endif

        return 1;
      }
    }
  }
  return 0;
}
/* Flush a neighbor queue */
static void
tsch_queue_flush_nbr_queue(struct tsch_neighbor *n)
{
  while(!tsch_queue_is_empty(n)) {
    struct tsch_packet *p = tsch_queue_remove_packet_from_queue(n);
    if(p != NULL) {
      /* Set return status for packet_sent callback */
      p->ret = MAC_TX_ERR;
      /* Call packet_sent callback */
      mac_call_sent_callback(p->sent, p->ptr, p->ret, p->transmissions);
      /* Free packet queuebuf */
      tsch_queue_free_packet(p);
    }
  }
}
/* Remove TSCH neighbor queue */
static void
tsch_queue_remove_nbr(struct tsch_neighbor *n)
{
  if(n != NULL) {
    if(tsch_get_lock()) {

      /* Remove neighbor from list */
      list_remove(neighbor_list, n);

      tsch_release_lock();

      PRINTF("TSCH-queue: removing nbr: ");
      PRINTLLADDR((const void *)(&n->addr));
      PRINTF("\n");

      /* Flush queue */
      tsch_queue_flush_nbr_queue(n);

      /* Free neighbor */
      memb_free(&neighbor_memb, n);
    }
  }
}
/* Add packet to neighbor queue. Use same lockfree implementation as ringbuf.c (put is atomic) */
int
tsch_queue_add_packet(const linkaddr_t *addr, mac_callback_t sent, void *ptr)
{
  struct tsch_neighbor *n = NULL;
  int16_t put_index = -1;
  struct tsch_packet *p = NULL;
  if(!tsch_is_locked()) {
    n = tsch_queue_add_nbr(addr);
    if(n != NULL) {
      put_index = ringbufindex_peek_put(&n->tx_ringbuf);
      if(put_index != -1) {
        p = memb_alloc(&packet_memb);
        if(p != NULL) {
          /* Enqueue packet */
          p->qb = queuebuf_new_from_packetbuf();
          if(p->qb != NULL) {
            p->sent = sent;
            p->ptr = ptr;
            p->ret = MAC_TX_DEFERRED;
            p->transmissions = 0;
            /* Add to ringbuf (actual add committed through atomic operation) */
            n->tx_array[put_index] = p;
            ringbufindex_put(&n->tx_ringbuf);
            return 1;
          } else {
            memb_free(&packet_memb, p);
          }
        }
      }
    }
  }
  PRINTF("TSCH-queue:! add packet failed: %u %p %d %p %p", tsch_is_locked(), n, put_index, p, p ? p->qb : NULL);
  return 0;
}
/* Returns the number of packets currently in the queue */
int
tsch_queue_packet_count(const linkaddr_t *addr)
{
  struct tsch_neighbor *n = NULL;
  if(!tsch_is_locked()) {
    n = tsch_queue_add_nbr(addr);
    if(n != NULL) {
      return ringbufindex_elements(&n->tx_ringbuf);
    }
  }
  return -1;
}
/* Remove first packet from a neighbor queue */
struct tsch_packet *
tsch_queue_remove_packet_from_queue(struct tsch_neighbor *n)
{
  if(!tsch_is_locked()) {
    if(n != NULL) {
      /* Get and remove packet from ringbuf (remove committed through an atomic operation */
      int16_t get_index = ringbufindex_get(&n->tx_ringbuf);
      if(get_index != -1) {
        return n->tx_array[get_index];
      } else {
        return NULL;
      }
    }
  }
  return NULL;
}
/* Free a packet */
void
tsch_queue_free_packet(struct tsch_packet *p)
{
  if(p != NULL) {
    queuebuf_free(p->qb);
    memb_free(&packet_memb, p);
  }
}
/* Flush all neighbor queues */
void
tsch_queue_flush_all()
{
  /* Deallocate unneeded neighbors */
  if(!tsch_is_locked()) {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while(n != NULL) {
      struct tsch_neighbor *next_n = list_item_next(n);
      tsch_queue_flush_nbr_queue(n);
      n = next_n;
    }
  }
}
/* Deallocate neighbors with empty queue */
void
tsch_queue_free_unused_neighbors()
{
  /* Deallocate unneeded neighbors */
  if(!tsch_is_locked()) {
    struct tsch_neighbor *n = list_head(neighbor_list);
    while(n != NULL) {
      struct tsch_neighbor *next_n = list_item_next(n);
      /* Queue is empty, no tx link to this neighbor: deallocate.
       * Always keep time source and virtual broadcast neighbors. */
      if(!n->is_broadcast && !n->is_time_source && !n->tx_links_count
          && tsch_queue_is_empty(n)) {
        tsch_queue_remove_nbr(n);
      }
      n = next_n;
    }
  }
}
/* Is the neighbor queue empty? */
int
tsch_queue_is_empty(const struct tsch_neighbor *n)
{
  return !tsch_is_locked() && n != NULL && ringbufindex_empty(&n->tx_ringbuf);
}
/* Returns the first packet from a neighbor queue */
struct tsch_packet *
tsch_queue_get_packet_for_nbr(const struct tsch_neighbor *n, int is_shared_link)
{
  if(!tsch_is_locked()) {
    if(n != NULL) {
      int16_t get_index = ringbufindex_peek_get(&n->tx_ringbuf);
      if(get_index != -1 &&
          !(is_shared_link && !tsch_queue_backoff_expired(n))) {    /* If this is a shared link,
                                                                    make sure the backoff has expired */
        return n->tx_array[get_index];
      }
    }
  }
  return NULL;
}
/* Returns the head packet from a neighbor queue (from neighbor address) */
struct tsch_packet *
tsch_queue_get_packet_for_dest_addr(const linkaddr_t *addr, int is_shared_link)
{
  if(!tsch_is_locked()) {
    return tsch_queue_get_packet_for_nbr(tsch_queue_get_nbr(addr), is_shared_link);
  }
  return NULL;
}
/* Returns the head packet of any neighbor queue with zero backoff counter.
 * Writes pointer to the neighbor in *n */
struct tsch_packet *
tsch_queue_get_unicast_packet_for_any(struct tsch_neighbor **n, int is_shared_link)
{
  if(!tsch_is_locked()) {
    /* TODO: round-robin among neighbors */
    struct tsch_neighbor *curr_nbr = list_head(neighbor_list);
    struct tsch_packet *p = NULL;
    while(curr_nbr != NULL) {
      if(!curr_nbr->is_broadcast && curr_nbr->tx_links_count == 0) {
        /* Only look up for non-broadcast neighbors we do not have a tx link to */
        p = tsch_queue_get_packet_for_nbr(curr_nbr, is_shared_link);
        if(p != NULL) {
          if(n != NULL) {
            *n = curr_nbr;
          }
          return p;
        }
      }
      curr_nbr = list_item_next(curr_nbr);
    }
  }
  return NULL;
}
/* May the neighbor transmit over a share link? */
int
tsch_queue_backoff_expired(const struct tsch_neighbor *n)
{
  return n->backoff_window == 0;
}
/* Reset neighbor backoff */
void
tsch_queue_backoff_reset(struct tsch_neighbor *n)
{
  n->backoff_window = 0;
  n->backoff_exponent = MAC_MIN_BE;
}
/* Increment backoff exponent, pick a new window */
void
tsch_queue_backoff_inc(struct tsch_neighbor *n)
{
  /* Increment exponent */
  n->backoff_exponent = MIN(n->backoff_exponent + 1, MAC_MAX_BE);
  /* Pick a window (number of shared slots to skip) */
  n->backoff_window = tsch_random_byte((1 << n->backoff_exponent) - 1);
  /* Add one to the window as we will decrement it at the end of the current slot
   * through tsch_queue_update_all_backoff_windows */
  n->backoff_window++;
}
/* Decrement backoff window for all queues directed at dest_addr */
void
tsch_queue_update_all_backoff_windows(const linkaddr_t *dest_addr)
{
  if(!tsch_is_locked()) {
    int is_broadcast = linkaddr_cmp(dest_addr, &tsch_broadcast_address);
    struct tsch_neighbor *n = list_head(neighbor_list);
    while(n != NULL) {
      if(n->backoff_window != 0 /* Is the queue in backoff state? */
          && (  (n->tx_links_count == 0  && is_broadcast)
             || (n->tx_links_count  > 0 && linkaddr_cmp(dest_addr, &n->addr)))) {
        n->backoff_window--;
      }
      n = list_item_next(n);
    }
  }
}
/* Initialize TSCH queue module */
void
tsch_queue_init(void)
{
  list_init(neighbor_list);
  tsch_random_init(*((uint32_t *)&linkaddr_node_addr) +
      *((uint32_t *)&linkaddr_node_addr + 1));
  memb_init(&neighbor_memb);
  memb_init(&packet_memb);
  /* Add virtual EB and the broadcast neighbors */
  n_eb = tsch_queue_add_nbr(&tsch_eb_address);
  n_broadcast = tsch_queue_add_nbr(&tsch_broadcast_address);
}
/* Testing the module */
int
tsch_queue_test(int num_nbr)
{
#define REPEAT 3

#define TEST_NUM_NBR 7
  const linkaddr_t node_addr[TEST_NUM_NBR] = {
      { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
      { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
      { { 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02 } },
      { { 0x00, 0x15, 0x8d, 0x00, 0x00, 0x46, 0x5f, 0x85 } },
      { { 0x00, 0x15, 0x8d, 0x00, 0x00, 0x46, 0x5f, 0x12 } },
      { { 0x00, 0x12, 0x74, 0x00, 0x11, 0x60, 0xfd, 0xbd } },
      { { 0x00, 0x12, 0x74, 0x00, 0x11, 0x5e, 0xbf, 0xcf } },
  };
  struct tsch_neighbor *n_arr[TEST_NUM_NBR], *n;

  uint8_t fail = 0;
  int b;
  /* limit num_nbr to TEST_NUM_NBR */
  num_nbr = (num_nbr > TEST_NUM_NBR) ? TEST_NUM_NBR : num_nbr;
  /* Add Nbrs */
  for(b = 0; b < num_nbr; b++) {
    n_arr[b] = tsch_queue_add_nbr(&node_addr[b]);
    if(n_arr == NULL) {
      printf("TSCH-queue test: Add nbr %d failed\n", b);
      return fail |= 128;
    }
  }
  /* Test packet add/get/remove */
  for(b = 0; b < num_nbr; b++) {
    /* Add packets to nbr n */
    int q, len = 51;
    n = n_arr[b];
    for(q = 0; q < REPEAT; q++) {
      /* Prepare the packet and schedule it to be sent */
      packetbuf_clear();
      uint8_t *qb = packetbuf_dataptr();
      int i;
      for(i = 0; i < len; i++) {
        qb[i] = i;
      }
      packetbuf_set_datalen(len);

      /* Enqueue packet */
      if(!tsch_queue_add_packet(&node_addr[b], NULL, NULL)) {
        printf("TSCH-queue test: Add packet %d FAILED\n", q + 1 );
        break;
      }
    }
    /* Get packets from nbr n, then remove them */
    int k;
    for(k = 0; k < q; k++) {
      int is_shared_link = 0;
      struct tsch_packet * current_packet = tsch_queue_get_packet_for_nbr(n, is_shared_link);
      if(current_packet == NULL) {
        printf("TSCH-queue test: Get packet FAILED\n");
        fail |= 32;
      }
      uint8_t *payload = queuebuf_dataptr(current_packet->qb);
      uint8_t payload_len = queuebuf_datalen(current_packet->qb);
      if(payload == NULL || payload_len != len) {
        printf("TSCH-queue test: Get queuebuf_dataptr failed ptr %p, len %d\n", payload, payload_len);
        fail |= 16;
      } else {
        int j;
        for(j = 0; j < payload_len; j++) {
          if(j != payload[j]) {
            printf("%03d @ %03d ", payload[j], j);
            fail |= 8;
          }
        }
        printf("\n");
        if(!tsch_queue_remove_packet_from_queue(n)) {
          printf("TSCH-queue test: Remove packet FAILED\n");
          fail |= 4;
        }
      }
    }
    /* Remove nbr */
    tsch_queue_remove_nbr(n);
    printf("TSCH-queue test: Nbr %d: %d packets added, %d removed. Len: %d\n", b, q, k, len);
  }
  return fail;
}
/* TEST - repeat tsch_queue_test n times
 * returns: number of successful attempts */
int
tsch_queue_aggressive_test(int repeat)
{
  static int i, test = 0;
  const int num_nbr = 7;
  for(i = 0; i < repeat; i++) {
    test += (tsch_queue_test(num_nbr) == 0);
  }
  printf("TSCH Queue Test: Success %d out of %d runs\n", test, repeat);
  return test;
}

/* Print nbr table entries (for debugging) */
void
tsch_queue_dump_nbrs()
{
  if(!tsch_is_locked()) {
    struct tsch_neighbor *curr_nbr = list_head(neighbor_list);
    printf("TSCH Queue dump-nbrs: Begin: ---->\n");
    while(curr_nbr != NULL) {
      void uip_debug_lladdr_print(const uip_lladdr_t *addr);
      uip_debug_lladdr_print((uip_lladdr_t *)&curr_nbr->addr);
      printf(" %u %u %u %u\n", !curr_nbr->is_broadcast, !curr_nbr->is_time_source,
          tsch_queue_is_empty(curr_nbr), tsch_queue_backoff_expired(curr_nbr));
      /* Get next in list */
      curr_nbr = list_item_next(curr_nbr);
    }
    printf("TSCH Queue dump-nbrs: Done. <----\n");
  } else {
    printf("TSCH Queue dump-nbrs: LOCKED\n");
  }
}
