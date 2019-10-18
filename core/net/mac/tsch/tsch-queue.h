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
 *         The list of neighbor uses a lock, but per-neighbor packet array are lockfree.
 *				 Read-only operation on neighbor and packets are allowed from interrupts and outside of them.
 *				 *Other operations are allowed outside of interrupt only.*
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 *         Simon Duquennoy <simonduq@sics.se>
 */

#ifndef __TSCH_QUEUE_H__
#define __TSCH_QUEUE_H__

#include "contiki.h"
#include "lib/ringbufindex.h"
#include "net/linkaddr.h"

/* The maximum number of packets in the system: must be power of two to enable atomic ringbuf operations */
#ifdef TSCH_CONF_QUEUE_NUM_PER_NEIGHBOR
#define TSCH_QUEUE_NUM_PER_NEIGHBOR TSCH_CONF_QUEUE_NUM_PER_NEIGHBOR
#else
#define TSCH_QUEUE_NUM_PER_NEIGHBOR 8
#endif

#if (TSCH_QUEUE_NUM_PER_NEIGHBOR & (TSCH_QUEUE_NUM_PER_NEIGHBOR-1)) != 0
#error TSCH_QUEUE_NUM_PER_NEIGHBOR must be power of two
#endif

#ifdef TSCH_CONF_QUEUE_MAX_NEIGHBOR_QUEUES
#define TSCH_QUEUE_MAX_NEIGHBOR_QUEUES TSCH_CONF_QUEUE_MAX_NEIGHBOR_QUEUES
#else
#define TSCH_QUEUE_MAX_NEIGHBOR_QUEUES 8
#endif

/* TSCH packet information */
struct tsch_packet {
  struct queuebuf *qb;  /* pointer to the queuebuf to be sent */
  mac_callback_t sent; /* callback for this packet */
  void *ptr; /* MAC callback parameter */
  uint8_t transmissions; /* #transmissions performed for this packet */
  uint8_t ret; /* status -- MAC return code */
};

/* TSCH neighbor information */
struct tsch_neighbor {
  /* Neighbors are stored as a list: "next" must be the first field */
  struct tsch_neighbor *next;
  linkaddr_t addr; /* MAC address of the neighbor */
  uint8_t is_broadcast; /* is this neighbor a virtual neighbor used for broadcast (of data packets or EBs) */
  uint8_t is_time_source; /* is this neighbor a time source? */
  uint8_t backoff_exponent; /* CSMA backoff exponent */
  uint8_t backoff_window; /* CSMA backoff window (number of slots to skip) */
  uint8_t last_backoff_window; /* Last CSMA backoff window */
  uint8_t tx_links_count; /* How many links do we have to this neighbor? */
  uint8_t dedicated_tx_links_count; /* How many dedicated links do we have to this neighbor? */
  /* Array for the ringbuf. Contains pointers to packets.
   * Its size must be a power of two to allow for atomic put */
  struct tsch_packet *tx_array[TSCH_QUEUE_NUM_PER_NEIGHBOR];
  /* Circular buffer of pointers to packet. */
  struct ringbufindex tx_ringbuf;
};

/* Broadcast and EB virtual neighbors */
extern struct tsch_neighbor *n_broadcast;
extern struct tsch_neighbor *n_eb;

/* Add a TSCH neighbor */
struct tsch_neighbor *tsch_queue_add_nbr(const linkaddr_t *addr);
/* Get a TSCH neighbor */
struct tsch_neighbor *tsch_queue_get_nbr(const linkaddr_t *addr);
/* Get a TSCH time source (we currently assume there is only one) */
struct tsch_neighbor *tsch_queue_get_time_source();
/* Update TSCH time source */
int tsch_queue_update_time_source(const linkaddr_t *new_addr);
/* Add packet to neighbor queue. Use same lockfree implementation as ringbuf.c (put is atomic) */
int tsch_queue_add_packet(const linkaddr_t *addr, mac_callback_t sent, void *ptr);
/* Returns the number of packets currently in the queue */
int tsch_queue_packet_count(const linkaddr_t *addr);
/* Remove first packet from a neighbor queue. The packet is stored in a seprate
 * dequeued packet list, for later processing. Return the packet. */
struct tsch_packet *tsch_queue_remove_packet_from_queue(struct tsch_neighbor *n);
/* Free a packet */
void tsch_queue_free_packet(struct tsch_packet *p);
/* Flush all neighbor queues */
void tsch_queue_flush_all();
/* Deallocate neighbors with empty queue */
void tsch_queue_free_unused_neighbors();
/* Is the neighbor queue empty? */
int tsch_queue_is_empty(const struct tsch_neighbor *n);
/* Returns the first packet from a neighbor queue */
struct tsch_packet *tsch_queue_get_packet_for_nbr(const struct tsch_neighbor *n, int is_shared_link);
/* Returns the head packet from a neighbor queue (from neighbor address) */
struct tsch_packet *tsch_queue_get_packet_for_dest_addr(const linkaddr_t *addr, int is_shared_link);
/* Returns the head packet of any neighbor queue with zero backoff counter.
 * Writes pointer to the neighbor in *n */
struct tsch_packet *tsch_queue_get_unicast_packet_for_any(struct tsch_neighbor **n, int is_shared_link);
/* May the neighbor transmit over a share link? */
int tsch_queue_backoff_expired(const struct tsch_neighbor *n);
/* Reset neighbor backoff */
void tsch_queue_backoff_reset(struct tsch_neighbor *n);
/* Increment backoff exponent, pick a new window */
void tsch_queue_backoff_inc(struct tsch_neighbor *n);
/* Decrement backoff window for all queues directed at dest_addr */
void tsch_queue_update_all_backoff_windows(const linkaddr_t *dest_addr);
/* Initialize TSCH queue module */
void tsch_queue_init(void);
/* Testing the module */
int tsch_queue_test(int num_nbr);
/* TEST - repeat tsch_queue_test n times
 * returns: number of successful attempts */
int tsch_queue_aggressive_test(int num);
/* Print nbr table entries (for debugging) */
void tsch_queue_dump_nbrs();

#endif /* __TSCH_QUEUE_H__ */
