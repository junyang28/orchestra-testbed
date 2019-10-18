/*
 * Copyright (c) 2007, Swedish Institute of Computer Science.
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
 *         Header file for NXP jn5168-specific rtimer code
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 */

#ifndef RTIMER_ARCH_H_
#define RTIMER_ARCH_H_

#include "sys/rtimer.h"

#ifdef RTIMER_CONF_SECOND
#define RTIMER_ARCH_SECOND RTIMER_CONF_SECOND
#else
//32MHz CPU clock => 16MHz timer
#define RTIMER_ARCH_SECOND (F_CPU/2)

//32MHz CPU clock => 16MHz timer / 2^9 ==> 31.25 KHz/*
//32MHz CPU clock => 16MHz timer / 2^6 ==> 250 KHz ==> 1s = 250000*
//32MHz CPU clock => 16MHz timer / 2^2 ==> 4 MHz ==> 1s = 4000000*
//#define RTIMER_PRESCALE     2
//#define RTIMER_ARCH_SECOND (4000000)
#endif

rtimer_clock_t rtimer_arch_now(void);

rtimer_clock_t rtimer_arch_get_time_until_next_wakeup(void);

#endif /* RTIMER_ARCH_H_ */
