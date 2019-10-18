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
 *         IEEE 802.15.4 TSCH MAC schedule manager.
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 *         Simon Duquennoy <simonduq@sics.se>
 */

#include "contiki.h"
#include "dev/leds.h"
#include "lib/memb.h"
#include "net/nbr-table.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/frame802154.h"
#include "sys/process.h"
#include "sys/rtimer.h"
#include <string.h>

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

/* Do we prioritize links with Tx option or do we only look
 * at slotframe handle? The standard stipulatez the former.
 * We make it default. */
#ifdef TSCH_SCHEDULE_CONF_PRIORITIZE_TX
#define TSCH_SCHEDULE_PRIORITIZE_TX TSCH_SCHEDULE_CONF_PRIORITIZE_TX
#else
#define TSCH_SCHEDULE_PRIORITIZE_TX 1
#endif

/* 6TiSCH Minimal schedule-related defines */
#ifdef TSCH_SCHEDULE_CONF_DEFAULT_LENGTH
#define TSCH_SCHEDULE_DEFAULT_LENGTH TSCH_SCHEDULE_CONF_DEFAULT_LENGTH
#else
#define TSCH_SCHEDULE_DEFAULT_LENGTH 17 /* 17x15ms => 255ms */
#endif

/* Max number of TSCH slotframes */
#ifdef TSCH_CONF_MAX_SLOTFRAMES
#define TSCH_MAX_SLOTFRAMES TSCH_CONF_MAX_SLOTFRAMES
#else
#define TSCH_MAX_SLOTFRAMES 4
#endif

/* Pre-allocated space for links */
MEMB(link_memb, struct tsch_link, TSCH_MAX_LINKS);
/* Pre-allocated space for slotframes */
MEMB(slotframe_memb, struct tsch_slotframe, TSCH_MAX_SLOTFRAMES);
/* List of slotframes (each slotframe holds its own list of links) */
LIST(slotframe_list);

/* Adds and returns a slotframe (NULL if failure) */
struct tsch_slotframe *
tsch_schedule_add_slotframe(uint16_t handle, uint16_t size)
{
  if(tsch_schedule_get_slotframe_from_handle(handle)) {
    /* A slotframe with this handle already exists */
    return NULL;
  } else {
    if(tsch_get_lock()) {
      struct tsch_slotframe *sf = memb_alloc(&slotframe_memb);
      if(sf != NULL) {
        /* Initialize the slotframe */
        sf->handle = handle;
        ASN_DIVISOR_INIT(sf->size, size);
        LIST_STRUCT_INIT(sf, links_list);
        /* Add the slotframe to the global list */
        list_add(slotframe_list, sf);
      }
      tsch_release_lock();
      return sf;
    }
  }
  return NULL;
}
/* Removes a slotframe Return 1 if success, 0 if failure */
int
tsch_schedule_remove_slotframe(struct tsch_slotframe *slotframe)
{
  if(slotframe != NULL) {
    /* Remove all links belonging to this slotframe */
    struct tsch_link *l;
    while((l = list_head(slotframe->links_list))) {
      tsch_schedule_remove_link(slotframe, l);
    }

    /* Now that the slotframe has no links, remove it. */
    if(tsch_get_lock()) {
      memb_free(&slotframe_memb, slotframe);
      list_remove(slotframe_list, slotframe);
      tsch_release_lock();
      return 1;
    }
  }
  return 0;
}
/* Looks for a slotframe from a handle */
struct tsch_slotframe *
tsch_schedule_get_slotframe_from_handle(uint16_t handle)
{
  if(!tsch_is_locked()) {
    struct tsch_slotframe *sf = list_head(slotframe_list);
    while(sf != NULL) {
      if(sf->handle == handle) {
        return sf;
      }
      sf = list_item_next(sf);
    }
  }
  return NULL;
}
/* Looks for a link from a handle */
struct tsch_link *
tsch_schedule_get_link_from_handle(uint16_t handle)
{
  if(!tsch_is_locked()) {
    struct tsch_slotframe *sf = list_head(slotframe_list);
    while(sf != NULL) {
      struct tsch_link *l = list_head(sf->links_list);
      /* Loop over all items. Assume there is max one link per timeslot */
      while(l != NULL) {
        if(l->handle == handle) {
          return l;
        }
        l = list_item_next(l);
      }
      sf = list_item_next(sf);
    }
  }
  return NULL;
}
/* Adds a link to a slotframe, return a pointer to it (NULL if failure) */
struct tsch_link *
tsch_schedule_add_link(struct tsch_slotframe *slotframe,
                       uint8_t link_options, enum link_type link_type, const linkaddr_t *address,
                       uint16_t timeslot, uint16_t channel_offset)
{
  struct tsch_link *l = NULL;
  if(slotframe != NULL) {
    /* We currently support only one link per timeslot in a given slotframe. */
    /* Start with removing the link currently installed at this timeslot (needed
     * to keep neighbor state in sync with link options etc.) */
    tsch_schedule_remove_link_from_timeslot(slotframe, timeslot);
    if(!tsch_get_lock()) {
      PRINTF("TSCH-schedule:! add_link memb_alloc couldn't take lock\n");
    } else {
      l = memb_alloc(&link_memb);
      if(l == NULL) {
        PRINTF("TSCH-schedule:! add_link memb_alloc failed\n");
      } else {
        static int current_link_handle = 0;
        struct tsch_neighbor *n;
        /* Add the link to the slotframe */
        list_add(slotframe->links_list, l);
        /* Initialize link */
        l->handle = current_link_handle++;
        l->link_options = link_options;
        l->link_type = link_type;
        l->slotframe_handle = slotframe->handle;
        l->timeslot = timeslot;
        l->channel_offset = channel_offset;
        l->data = NULL;
        if(address == NULL) {
          address = &linkaddr_null;
        }
        linkaddr_copy(&l->addr, address);

        PRINTF("TSCH-schedule: add_link %u %u %u %u %u\n",
            slotframe->handle, link_options, timeslot, channel_offset, LOG_NODEID_FROM_LINKADDR(address));

        /* Release the lock before we update the neighbor (will take the lock) */
        tsch_release_lock();

        if(l->link_options & LINK_OPTION_TX) {
          n = tsch_queue_add_nbr(&l->addr);
          /* We have a tx link to this neighbor, update counters */
          if(n != NULL) {
            n->tx_links_count++;
            if(!(l->link_options & LINK_OPTION_SHARED)) {
              n->dedicated_tx_links_count++;
            }
          }
        }
      }
    }
  }
  return l;
}
/* Removes a link from slotframe. Return 1 if success, 0 if failure */
int
tsch_schedule_remove_link(struct tsch_slotframe *slotframe, struct tsch_link *l)
{
  if(slotframe != NULL && l != NULL && l->slotframe_handle == slotframe->handle) {
    if(tsch_get_lock()) {
      uint8_t link_options;
      linkaddr_t addr;

      /* Save link option and addr in local variables as we need them
       * after freeing the link */
      link_options = l->link_options;
      linkaddr_copy(&addr, &l->addr);

      /* The link to be removed is the scheduled as next, set it to NULL
       * to abort the next link operation */
      if(l == current_link) {
        current_link = NULL;
      }

      PRINTF("TSCH-schedule: remove_link %u %u %u %u %u\n",
                  slotframe->handle, l->link_options, l->timeslot, l->channel_offset,
                  LOG_NODEID_FROM_LINKADDR(&l->addr));

      list_remove(slotframe->links_list, l);
      memb_free(&link_memb, l);

      /* Release the lock before we update the neighbor (will take the lock) */
      tsch_release_lock();

      /* This was a tx link to this neighbor, update counters */
      if(link_options & LINK_OPTION_TX) {
        struct tsch_neighbor *n = tsch_queue_add_nbr(&addr);
        if(n != NULL) {
          n->tx_links_count--;
          if(!(link_options & LINK_OPTION_SHARED)) {
            n->dedicated_tx_links_count--;
          }
        }
      }

      return 1;
    } else {
      PRINTF("TSCH-schedule:! remove_link memb_alloc couldn't take lock\n");
    }
  }
  return 0;
}
/* Removes a link from slotframe and timeslot. Return a 1 if success, 0 if failure */
int
tsch_schedule_remove_link_from_timeslot(struct tsch_slotframe *slotframe, uint16_t timeslot)
{
  return slotframe != NULL &&
      tsch_schedule_remove_link(slotframe, tsch_schedule_get_link_from_timeslot(slotframe, timeslot));
}
/* Looks within a slotframe for a link with a given timeslot */
struct tsch_link *
tsch_schedule_get_link_from_timeslot(struct tsch_slotframe *slotframe, uint16_t timeslot)
{
  if(!tsch_is_locked()) {
    if(slotframe != NULL) {
      struct tsch_link *l = list_head(slotframe->links_list);
      /* Loop over all items. Assume there is max one link per timeslot */
      while(l != NULL) {
        if(l->timeslot == timeslot) {
          return l;
        }
        l = list_item_next(l);
      }
      return l;
    }
  }
  return NULL;
}
/* Returns the link to be used at a given ASN */
struct tsch_link *
tsch_schedule_get_link_from_asn(struct asn_t *asn)
{
  struct tsch_link *curr_best = NULL;
  struct tsch_slotframe *sf = list_head(slotframe_list);
  /* For each slotframe, looks for a link matching the asn.
   * Tx links have priority, then lower handle have priority. */
  while(sf != NULL) {
    /* Get timeslot from ASN, given the slotframe length */
    uint16_t timeslot = ASN_MOD(*asn, sf->size);
    struct tsch_link *l = tsch_schedule_get_link_from_timeslot(sf, timeslot);
    /* We have a match */
    if(l != NULL) {
      if(curr_best == NULL) {
        curr_best = l;
      } else {
#if TSCH_SCHEDULE_PRIORITIZE_TX
        /* We already have a current best,
         * we must check Tx flag and handle to find the highest priority link */
        if((curr_best->link_options & LINK_OPTION_TX) == (l->link_options & LINK_OPTION_TX)) {
          /* Both or neither links have Tx, select the one with lowest handle */
          if(l->slotframe_handle < curr_best->slotframe_handle) {
            curr_best = l;
          }
        } else {
          /* Select the link that has the Tx option */
          if(l->link_options & LINK_OPTION_TX) {
            curr_best = l;
          }
        }
#else /* TSCH_SCHEDULE_PRIORITIZE_TX */
        if(curr_best->slotframe_handle > sf->handle) {
          /* We have a lower handle */
          curr_best = l;
        }
#endif /* TSCH_SCHEDULE_PRIORITIZE_TX */
      }
    }
    sf = list_item_next(sf);
  }
  return curr_best;
}
/* Returns the next active link after a given ASN */
struct tsch_link *
tsch_schedule_get_next_active_link(struct asn_t *asn, uint16_t *time_offset)
{
  uint16_t curr_earliest = 0;
  struct tsch_link *curr_earliest_link = NULL;
  if(!tsch_is_locked()) {
    struct tsch_slotframe *sf = list_head(slotframe_list);
    /* For each slotframe, look for the earliest occurring link */
    while(sf != NULL) {
      /* Get timeslot from ASN, given the slotframe length */
      uint16_t timeslot = ASN_MOD(*asn, sf->size);
      struct tsch_link *l = list_head(sf->links_list);
      while(l != NULL) {
        uint16_t time_to_timeslot =
          l->timeslot > timeslot ?
          l->timeslot - timeslot :
          sf->size.val + l->timeslot - timeslot;
        if(curr_earliest == 0 || time_to_timeslot < curr_earliest) {
          curr_earliest = time_to_timeslot;
          curr_earliest_link = l;
        }
        l = list_item_next(l);
      }
      sf = list_item_next(sf);
    }
    if(time_offset != NULL) {
      *time_offset = curr_earliest;
    }
  }
  return curr_earliest_link;
}
void
tsch_schedule_print()
{
  if(!tsch_is_locked()) {
    struct tsch_slotframe *sf = list_head(slotframe_list);

    printf("Schedule: slotframe list\n");

    while(sf != NULL) {
      struct tsch_link *l = list_head(sf->links_list);

      printf("[Slotframe] Handle %u, size %u\n", sf->handle, sf->size.val);
      printf("List of links:\n");

      while(l != NULL) {
        printf("[Link] Options %02x, type %u, timeslot %u, channel offset %u, address %u\n",
               l->link_options, l->link_type, l->timeslot, l->channel_offset, l->addr.u8[7]);
        l = list_item_next(l);
      }

      sf = list_item_next(sf);
    }

    printf("Schedule: end of slotframe list\n");
  }
}
void
tsch_schedule_test()
{
  static linkaddr_t link_broadcast_address = { { 0, 0, 0, 0, 0, 0, 0, 0 } };
  static linkaddr_t address1 = { { 0x00, 0x12, 0x74, 01, 00, 01, 01, 01 } };
  static linkaddr_t address2 = { { 0x00, 0x12, 0x74, 02, 00, 02, 02, 02 } };

  struct tsch_slotframe *sf1 = tsch_schedule_add_slotframe(20, 5);
  struct tsch_slotframe *sf2 = tsch_schedule_add_slotframe(21, 3);

  tsch_schedule_add_link(sf1,
                         LINK_OPTION_RX | LINK_OPTION_TX | LINK_OPTION_SHARED | LINK_OPTION_TIME_KEEPING,
                         LINK_TYPE_ADVERTISING, &link_broadcast_address,
                         0, 1);

  tsch_schedule_add_link(sf1,
                         LINK_OPTION_RX,
                         LINK_TYPE_NORMAL, &address1,
                         1, 1);

  tsch_schedule_add_link(sf1,
                         LINK_OPTION_RX,
                         LINK_TYPE_NORMAL, &address1,
                         4, 10);

  tsch_schedule_add_link(sf2,
                         LINK_OPTION_TX,
                         LINK_TYPE_NORMAL, &address2,
                         0, 2);

  tsch_schedule_print();

  unsigned asn_val;
  for(asn_val = 0; asn_val < 20; asn_val++) {
    struct asn_t asn;
    ASN_INIT(asn, 0, asn_val);
    struct tsch_link *l = tsch_schedule_get_link_from_asn(&asn);
    if(l != NULL) {
      printf("asn %u: timeslot %u, channel offset %u (schedule handle %u)\n",
          asn_val, l->timeslot, l->channel_offset, l->slotframe_handle);
    } else {
      printf("asn %u: no link\n", asn_val);
    }
  }
}
/* Initialization. Return 1 is success, 0 if failure. */
int
tsch_schedule_init()
{
  if(tsch_get_lock()) {
    memb_init(&link_memb);
    memb_init(&slotframe_memb);
    list_init(slotframe_list);
    tsch_release_lock();
    return 1;
  } else {
    return 0;
  }
}

/* Create a 6TiSCH minimal schedule */
void
tsch_schedule_create_minimal()
{
  static struct tsch_slotframe *sf_min;
  /* Build 6TiSCH minimal schedule.
   * We pick a slotframe length of TSCH_SCHEDULE_DEFAULT_LENGTH */
  sf_min = tsch_schedule_add_slotframe(0, TSCH_SCHEDULE_DEFAULT_LENGTH);
  /* Add a single Tx|Rx|Shared slot using broadcast address (i.e. usable for unicast and broadcast).
   * We set the link type to advertising, which is not compliant with 6TiSCH minimal schedule
   * but is required according to 802.15.4e if also used for EB transmission.
   * Timeslot: 0, channel offset: 0. */
  tsch_schedule_add_link(sf_min,
      LINK_OPTION_RX | LINK_OPTION_TX | LINK_OPTION_SHARED,
      LINK_TYPE_ADVERTISING, &tsch_broadcast_address,
      0, 0);

  /* Example of a dedicated Tx unicast link. Timeslot: 1, channel offset: 0. */
  /* static linkaddr_t dest_addr = { { 0x00, 0x12, 0x74, 01, 00, 01, 01, 01 } }; */
  /* tsch_schedule_add_link(sf,
            LINK_OPTION_RX,
            LINK_TYPE_NORMAL, &dest_addr,
            1, 0); */
}
