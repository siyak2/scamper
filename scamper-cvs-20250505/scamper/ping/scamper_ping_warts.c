/*
 * scamper_ping_warts.c
 *
 * Copyright (C) 2005-2006 Matthew Luckie
 * Copyright (C) 2006-2011 The University of Waikato
 * Copyright (C) 2012-2014 The Regents of the University of California
 * Copyright (C) 2016-2024 Matthew Luckie
 * Author: Matthew Luckie
 *
 * $Id: scamper_ping_warts.c,v 1.41 2025/05/05 03:34:24 mjl Exp $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "internal.h"

#include "scamper_addr.h"
#include "scamper_addr_int.h"
#include "scamper_list.h"
#include "scamper_icmpext.h"
#include "scamper_ping.h"
#include "scamper_file.h"
#include "scamper_ping_int.h"
#include "scamper_file_warts.h"
#include "scamper_ping_warts.h"

#include "mjl_splaytree.h"
#include "utils.h"

/*
 * the optional bits of a ping structure
 */
#define WARTS_PING_LIST_ID         1
#define WARTS_PING_CYCLE_ID        2
#define WARTS_PING_ADDR_SRC_GID    3 /* deprecated */
#define WARTS_PING_ADDR_DST_GID    4 /* deprecated */
#define WARTS_PING_START           5
#define WARTS_PING_STOP_R          6
#define WARTS_PING_STOP_D          7
#define WARTS_PING_DATA_LEN        8
#define WARTS_PING_DATA_BYTES      9
#define WARTS_PING_PROBE_COUNT    10
#define WARTS_PING_PROBE_SIZE     11
#define WARTS_PING_PROBE_WAIT     12
#define WARTS_PING_PROBE_TTL      13
#define WARTS_PING_STOP_COUNT     14
#define WARTS_PING_PING_SENT      15
#define WARTS_PING_PROBE_METHOD   16
#define WARTS_PING_PROBE_SPORT    17
#define WARTS_PING_PROBE_DPORT    18
#define WARTS_PING_USERID         19
#define WARTS_PING_ADDR_SRC       20
#define WARTS_PING_ADDR_DST       21
#define WARTS_PING_FLAGS8         22
#define WARTS_PING_PROBE_TOS      23
#define WARTS_PING_PROBE_TSPS     24
#define WARTS_PING_PROBE_ICMPSUM  25
#define WARTS_PING_REPLY_PMTU     26
#define WARTS_PING_PROBE_TIMEOUT  27
#define WARTS_PING_PROBE_WAIT_US  28
#define WARTS_PING_PROBE_TCPACK   29
#define WARTS_PING_FLAGS          30
#define WARTS_PING_PROBE_TCPSEQ   31
#define WARTS_PING_ADDR_RTR       32
#define WARTS_PING_PROBE_TIMEOUT_US 33

static const warts_var_t ping_vars[] =
{
  {WARTS_PING_LIST_ID,           4},
  {WARTS_PING_CYCLE_ID,          4},
  {WARTS_PING_ADDR_SRC_GID,      4},
  {WARTS_PING_ADDR_DST_GID,      4},
  {WARTS_PING_START,             8},
  {WARTS_PING_STOP_R,            1},
  {WARTS_PING_STOP_D,            1},
  {WARTS_PING_DATA_LEN,          2},
  {WARTS_PING_DATA_BYTES,       -1},
  {WARTS_PING_PROBE_COUNT,       2},
  {WARTS_PING_PROBE_SIZE,        2},
  {WARTS_PING_PROBE_WAIT,        1},
  {WARTS_PING_PROBE_TTL,         1},
  {WARTS_PING_STOP_COUNT,        2},
  {WARTS_PING_PING_SENT,         2},
  {WARTS_PING_PROBE_METHOD,      1},
  {WARTS_PING_PROBE_SPORT,       2},
  {WARTS_PING_PROBE_DPORT,       2},
  {WARTS_PING_USERID,            4},
  {WARTS_PING_ADDR_SRC,         -1},
  {WARTS_PING_ADDR_DST,         -1},
  {WARTS_PING_FLAGS8,            1},
  {WARTS_PING_PROBE_TOS,         1},
  {WARTS_PING_PROBE_TSPS,       -1},
  {WARTS_PING_PROBE_ICMPSUM,     2},
  {WARTS_PING_REPLY_PMTU,        2},
  {WARTS_PING_PROBE_TIMEOUT,     1},
  {WARTS_PING_PROBE_WAIT_US,     4},
  {WARTS_PING_PROBE_TCPACK,      4},
  {WARTS_PING_FLAGS,             4},
  {WARTS_PING_PROBE_TCPSEQ,      4},
  {WARTS_PING_ADDR_RTR,         -1},
  {WARTS_PING_PROBE_TIMEOUT_US,  4},
};
#define ping_vars_mfb WARTS_VAR_MFB(ping_vars)

#define WARTS_PING_REPLY_ADDR_GID        1 /* deprecated */
#define WARTS_PING_REPLY_FLAGS           2
#define WARTS_PING_REPLY_REPLY_TTL       3
#define WARTS_PING_REPLY_REPLY_SIZE      4
#define WARTS_PING_REPLY_ICMP_TC         5
#define WARTS_PING_REPLY_RTT             6
#define WARTS_PING_REPLY_PROBE_ID        7
#define WARTS_PING_REPLY_REPLY_IPID      8
#define WARTS_PING_REPLY_PROBE_IPID      9
#define WARTS_PING_REPLY_REPLY_PROTO     10
#define WARTS_PING_REPLY_TCP_FLAGS       11
#define WARTS_PING_REPLY_ADDR            12
#define WARTS_PING_REPLY_V4RR            13
#define WARTS_PING_REPLY_V4TS            14
#define WARTS_PING_REPLY_REPLY_IPID32    15
#define WARTS_PING_REPLY_TX              16
#define WARTS_PING_REPLY_TSREPLY         17
#define WARTS_PING_REPLY_PROBE_SPORT     18
#define WARTS_PING_REPLY_REPLY_IPTOS     19
#define WARTS_PING_REPLY_IFNAME          20
#define WARTS_PING_REPLY_ICMP_NHMTU      21

static const warts_var_t ping_reply_vars[] =
{
  {WARTS_PING_REPLY_ADDR_GID,        4},
  {WARTS_PING_REPLY_FLAGS,           1},
  {WARTS_PING_REPLY_REPLY_TTL,       1},
  {WARTS_PING_REPLY_REPLY_SIZE,      2},
  {WARTS_PING_REPLY_ICMP_TC,         2},
  {WARTS_PING_REPLY_RTT,             4},
  {WARTS_PING_REPLY_PROBE_ID,        2},
  {WARTS_PING_REPLY_REPLY_IPID,      2},
  {WARTS_PING_REPLY_PROBE_IPID,      2},
  {WARTS_PING_REPLY_REPLY_PROTO,     1},
  {WARTS_PING_REPLY_TCP_FLAGS,       1},
  {WARTS_PING_REPLY_ADDR,           -1},
  {WARTS_PING_REPLY_V4RR,           -1},
  {WARTS_PING_REPLY_V4TS,           -1},
  {WARTS_PING_REPLY_REPLY_IPID32,    4},
  {WARTS_PING_REPLY_TX,              8},
  {WARTS_PING_REPLY_TSREPLY,        12},
  {WARTS_PING_REPLY_PROBE_SPORT,     2},
  {WARTS_PING_REPLY_REPLY_IPTOS,     1},
  {WARTS_PING_REPLY_IFNAME,         -1},
  {WARTS_PING_REPLY_ICMP_NHMTU,      2},
};
#define ping_reply_vars_mfb WARTS_VAR_MFB(ping_reply_vars)

typedef struct warts_ping_reply
{
  scamper_ping_probe_t *probe;
  scamper_ping_reply_t *reply;
  uint8_t               flags[WARTS_VAR_MFB(ping_reply_vars)];
  uint16_t              flags_len;
  uint16_t              params_len;
} warts_ping_reply_t;

static void insert_ping_reply_v4rr(uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   const scamper_ping_reply_v4rr_t *rr,
				   void *param)
{
  uint8_t i;

  assert(len - *off >= 1);
  buf[(*off)++] = rr->ipc;
  for(i=0; i<rr->ipc; i++)
    insert_addr(buf, off, len, rr->ip[i], param);

  return;
}

static int extract_ping_reply_v4rr(const uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   scamper_ping_reply_v4rr_t **out,
				   void *param)
{
  scamper_addr_t *addr;
  uint8_t i, ipc;

  if(*off >= len || len - *off < 1)
    return -1;

  ipc = buf[(*off)++];

  if((*out = scamper_ping_reply_v4rr_alloc(ipc)) == NULL)
    return -1;

  for(i=0; i<ipc; i++)
    {
      if(extract_addr(buf, off, len, &addr, param) != 0)
	return -1;
      (*out)->ip[i] = addr;
    }

  return 0;
}

static void insert_ping_reply_v4ts(uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   const scamper_ping_reply_v4ts_t *ts,
				   void *param)
{
  uint8_t i, ipc;

  ipc = (ts->ips != NULL ? ts->tsc : 0);

  assert(len - *off >= 2);
  buf[(*off)++] = ts->tsc;
  buf[(*off)++] = ipc;

  for(i=0; i<ts->tsc; i++)
    insert_uint32(buf, off, len, &ts->tss[i], NULL);

  for(i=0; i<ipc; i++)
    insert_addr(buf, off, len, ts->ips[i], param);

  return;
}

static int extract_ping_reply_v4ts(const uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   scamper_ping_reply_v4ts_t **out,
				   void *param)
{
  scamper_addr_t *addr;
  uint8_t i, tsc, ipc;
  uint32_t u32;

  if(*off >= len || len - *off < 2)
    return -1;

  /*
   * the v4ts structure will have timestamps, and sometimes IP
   * addresses.  if there are IP addresses, the number must match the
   * number of timestamp records.  the second parameter to
   * scamper_ping_reply_v4ts_alloc is a binary flag that says whether
   * or not to allocate the same number of IP addresses.  this is
   * probably a design oversight in the warts records.
   */
  tsc = buf[(*off)++];
  ipc = buf[(*off)++];
  if(ipc != 0 && ipc != tsc)
    return -1;
  if((*out = scamper_ping_reply_v4ts_alloc(tsc, ipc != 0 ? 1 : 0)) == NULL)
    return -1;

  for(i=0; i<tsc; i++)
    {
      if(extract_uint32(buf, off, len, &u32, NULL) != 0)
	return -1;
      (*out)->tss[i] = u32;
    }

  for(i=0; i<ipc; i++)
    {
      if(extract_addr(buf, off, len, &addr, param) != 0)
	return -1;
      (*out)->ips[i] = addr;
    }

  return 0;
}

static void insert_ping_reply_tsreply(uint8_t *buf, uint32_t *off,
				      const uint32_t len,
				      const scamper_ping_reply_tsreply_t *ts,
				      void *param)
{
  insert_uint32(buf, off, len, &ts->tso, NULL);
  insert_uint32(buf, off, len, &ts->tsr, NULL);
  insert_uint32(buf, off, len, &ts->tst, NULL);
  return;
}

static int extract_ping_reply_tsreply(uint8_t *buf, uint32_t *off,
				      const uint32_t len,
				      scamper_ping_reply_tsreply_t **out,
				      void *param)
{
  scamper_ping_reply_tsreply_t *tsreply;
  if(*off >= len || len - *off < 12)
    return -1;
  if((tsreply = scamper_ping_reply_tsreply_alloc()) == NULL)
    return -1;
  extract_uint32(buf, off, len, &tsreply->tso, NULL);
  extract_uint32(buf, off, len, &tsreply->tsr, NULL);
  extract_uint32(buf, off, len, &tsreply->tst, NULL);
  *out = tsreply;
  return 0;
}

static int warts_ping_reply_params(const scamper_ping_t *ping,
				   const scamper_ping_probe_t *probe,
				   const scamper_ping_reply_t *reply,
				   warts_addrtable_t *table,
				   warts_ifnametable_t *ifntable,
				   uint8_t *flags, uint16_t *flags_len,
				   uint16_t *params_len)
{
  const warts_var_t *var;
  int j, max_id = 0;
  size_t i;

  /* unset all the flags possible */
  memset(flags, 0, ping_reply_vars_mfb);
  *params_len = 0;

  for(i=0; i<sizeof(ping_reply_vars)/sizeof(warts_var_t); i++)
    {
      var = &ping_reply_vars[i];

      if(var->id == WARTS_PING_REPLY_ADDR_GID ||
	 (var->id == WARTS_PING_REPLY_ADDR && reply->addr == NULL) ||
	 (var->id == WARTS_PING_REPLY_FLAGS && reply->flags == 0) ||
	 (var->id == WARTS_PING_REPLY_REPLY_PROTO &&
	  SCAMPER_PING_METHOD_IS_ICMP(ping)) ||
	 (var->id == WARTS_PING_REPLY_REPLY_TTL &&
	  (reply->flags & SCAMPER_PING_REPLY_FLAG_REPLY_TTL) == 0) ||
	 (var->id == WARTS_PING_REPLY_REPLY_IPID &&
	  (SCAMPER_ADDR_TYPE_IS_IPV4(ping->dst) == 0 ||
	   (reply->flags & SCAMPER_PING_REPLY_FLAG_REPLY_IPID) == 0)) ||
	 (var->id == WARTS_PING_REPLY_REPLY_IPID32 &&
	  (SCAMPER_ADDR_TYPE_IS_IPV6(ping->dst) == 0 ||
	   (reply->flags & SCAMPER_PING_REPLY_FLAG_REPLY_IPID) == 0)) ||
	 (var->id == WARTS_PING_REPLY_PROBE_IPID &&
	  (SCAMPER_ADDR_TYPE_IS_IPV4(ping->dst) == 0 ||
	   (reply->flags & SCAMPER_PING_REPLY_FLAG_PROBE_IPID) == 0)) ||
	 (var->id == WARTS_PING_REPLY_ICMP_TC &&
	  SCAMPER_PING_REPLY_IS_ICMP(reply) == 0) ||
	 (var->id == WARTS_PING_REPLY_TCP_FLAGS &&
	  SCAMPER_PING_REPLY_IS_TCP(reply) == 0) ||
	 (var->id == WARTS_PING_REPLY_V4RR && reply->v4rr == NULL) ||
	 (var->id == WARTS_PING_REPLY_V4TS && reply->v4ts == NULL) ||
	 (var->id == WARTS_PING_REPLY_TX && probe->tx.tv_sec == 0) ||
	 (var->id == WARTS_PING_REPLY_TSREPLY && reply->tsreply == NULL) ||
	 (var->id == WARTS_PING_REPLY_PROBE_SPORT && probe->sport == 0) ||
	 (var->id == WARTS_PING_REPLY_REPLY_IPTOS && reply->tos == 0) ||
	 (var->id == WARTS_PING_REPLY_IFNAME && reply->ifname == NULL) ||
	 (var->id == WARTS_PING_REPLY_ICMP_NHMTU &&
	  SCAMPER_PING_REPLY_IS_ICMP_PTB(reply) == 0))
	{
	  continue;
	}

      flag_set(flags, var->id, &max_id);

      if(var->id == WARTS_PING_REPLY_ADDR)
	{
	  if(warts_addr_size(table, reply->addr, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_PING_REPLY_V4RR)
	{
	  *params_len += 1;
	  for(j=0; j<reply->v4rr->ipc; j++)
	    if(warts_addr_size(table, reply->v4rr->ip[j], params_len) != 0)
	      return -1;
	}
      else if(var->id == WARTS_PING_REPLY_V4TS)
	{
	  assert(reply->v4ts != NULL);
	  *params_len += 2; /* one byte tsc, one byte count of v4ts->ips */
	  *params_len += (reply->v4ts->tsc * 4);
	  if(reply->v4ts->ips != NULL)
	    for(j=0; j<reply->v4ts->tsc; j++)
	      if(warts_addr_size(table, reply->v4ts->ips[j], params_len) != 0)
		return -1;
	}
      else if(var->id == WARTS_PING_REPLY_IFNAME)
	{
	  assert(reply->ifname != NULL);
	  if(warts_ifname_size(ifntable, reply->ifname, params_len) != 0)
	    return -1;
	}
      else
	{
	  assert(var->size >= 0);
	  *params_len += var->size;
	}
    }

  *flags_len = fold_flags(flags, max_id);
  return 0;
}

static int warts_ping_reply_state(const scamper_file_t *sf,
				  const scamper_ping_t *ping,
				  scamper_ping_probe_t *probe,
				  scamper_ping_reply_t *reply,
				  warts_ping_reply_t *state,
				  warts_addrtable_t *table,
				  warts_ifnametable_t *ifntable,
				  uint32_t *len)
{
  if(warts_ping_reply_params(ping, probe, reply, table, ifntable, state->flags,
			     &state->flags_len, &state->params_len) != 0)
    return -1;

  state->probe = probe;
  state->reply = reply;

  *len += state->flags_len + state->params_len;
  if(state->params_len != 0) *len += 2;

  return 0;
}

static int extract_ping_reply_icmptc(const uint8_t *buf, uint32_t *off,
				     uint32_t len, scamper_ping_reply_t *reply,
				     void *param)
{
  if(*off >= len || len - *off < 2)
    return -1;

  reply->icmp_type = buf[(*off)++];
  reply->icmp_code = buf[(*off)++];
  return 0;
}

static void insert_ping_reply_icmptc(uint8_t *buf, uint32_t *off,
				     const uint32_t len,
				     const scamper_ping_reply_t *reply,
				     void *param)
{
  assert(len - *off >= 2);

  buf[(*off)++] = reply->icmp_type;
  buf[(*off)++] = reply->icmp_code;

  return;
}

static int warts_ping_reply_read_int(const scamper_ping_t *ping,
				     scamper_ping_reply_t *reply,
				     warts_state_t *state,
				     warts_addrtable_t *table,
				     warts_ifnametable_t *ifntable,
				     const uint8_t *buf,
				     uint32_t *off, uint32_t len)
{
  uint16_t probe_id = 0, probe_ipid = 0, probe_sport = 0, reply_ipid = 0;
  struct timeval probe_tx;
  warts_param_reader_t handlers[] = {
    {&reply->addr,            (wpr_t)extract_addr_gid,             state},
    {&reply->flags,           (wpr_t)extract_byte,                 NULL},
    {&reply->ttl,             (wpr_t)extract_byte,                 NULL},
    {&reply->size,            (wpr_t)extract_uint16,               NULL},
    {reply,                   (wpr_t)extract_ping_reply_icmptc,    NULL},
    {&reply->rtt,             (wpr_t)extract_rtt,                  NULL},
    {&probe_id,               (wpr_t)extract_uint16,               NULL},
    {&reply_ipid,             (wpr_t)extract_uint16,               NULL},
    {&probe_ipid,             (wpr_t)extract_uint16,               NULL},
    {&reply->proto,           (wpr_t)extract_byte,                 NULL},
    {&reply->tcp_flags,       (wpr_t)extract_byte,                 NULL},
    {&reply->addr,            (wpr_t)extract_addr,                 table},
    {&reply->v4rr,            (wpr_t)extract_ping_reply_v4rr,      table},
    {&reply->v4ts,            (wpr_t)extract_ping_reply_v4ts,      table},
    {&reply->ipid32,          (wpr_t)extract_uint32,               NULL},
    {&probe_tx,               (wpr_t)extract_timeval,              NULL},
    {&reply->tsreply,         (wpr_t)extract_ping_reply_tsreply,   NULL},
    {&probe_sport,            (wpr_t)extract_uint16,               NULL},
    {&reply->tos,             (wpr_t)extract_byte,                 NULL},
    {&reply->ifname,          (wpr_t)extract_ifname,               ifntable},
    {&reply->icmp_nhmtu,      (wpr_t)extract_uint16,               NULL},
  };
  const int handler_cnt = sizeof(handlers) / sizeof(warts_param_reader_t);
  scamper_ping_probe_t *probe = NULL;
  uint32_t o = *off;
  int i;

  memset(&probe_tx, 0, sizeof(probe_tx));

  if((i = warts_params_read(buf, off, len, handlers, handler_cnt)) != 0)
    return i;

  if(reply->addr == NULL)
    return -1;

  if(probe_id >= ping->ping_sent)
    return -1;
  if((probe = ping->probes[probe_id]) == NULL)
    {
      if((probe = scamper_ping_probe_alloc()) == NULL)
	return -1;
      probe->id = probe_id;
      probe->sport = probe_sport;
      probe->ipid  = probe_ipid;
      probe->flags = SCAMPER_PING_PROBE_FLAGS_MASK(reply);
      timeval_cpy(&probe->tx, &probe_tx);
      ping->probes[probe_id] = probe;
    }

  /*
   * some earlier versions of the ping reply structure did not include
   * the reply protocol field.  fill it with something valid.
   */
  if(flag_isset(&buf[o], WARTS_PING_REPLY_REPLY_PROTO) == 0)
    {
      if(SCAMPER_ADDR_TYPE_IS_IPV4(ping->dst))
	reply->proto = IPPROTO_ICMP;
      else
	reply->proto = IPPROTO_ICMPV6;
    }

  if(flag_isset(&buf[o], WARTS_PING_REPLY_REPLY_IPID) &&
     SCAMPER_ADDR_TYPE_IS_IPV4(ping->dst))
    reply->ipid32 = reply_ipid;

  if(scamper_ping_probe_reply_append(probe, reply) != 0)
    return -1;

  return 0;
}

static int warts_ping_reply_read(const scamper_ping_t *ping,
				 warts_state_t *state,
				 warts_addrtable_t *table,
				 warts_ifnametable_t *ifntable,
				 const uint8_t *buf,
				 uint32_t *off, uint32_t len)
{
  scamper_ping_reply_t *reply;

  if((reply = scamper_ping_reply_alloc()) == NULL ||
     warts_ping_reply_read_int(ping, reply, state, table, ifntable,
			       buf, off, len) != 0)
    {
      if(reply != NULL)
	scamper_ping_reply_free(reply);
      return -1;
    }

  return 0;
}

static void warts_ping_reply_write(const warts_ping_reply_t *state,
				   warts_addrtable_t *table,
				   warts_ifnametable_t *ifntable,
				   uint8_t *buf, uint32_t *off, uint32_t len)
{
  scamper_ping_probe_t *probe = state->probe;
  scamper_ping_reply_t *reply = state->reply;
  uint16_t reply_ipid = reply->ipid32 & 0xFFFF;

  warts_param_writer_t handlers[] = {
    {NULL,                    NULL,                                 NULL},
    {&reply->flags,           (wpw_t)insert_byte,                   NULL},
    {&reply->ttl,             (wpw_t)insert_byte,                   NULL},
    {&reply->size,            (wpw_t)insert_uint16,                 NULL},
    {reply,                   (wpw_t)insert_ping_reply_icmptc,      NULL},
    {&reply->rtt,             (wpw_t)insert_rtt,                    NULL},
    {&probe->id,              (wpw_t)insert_uint16,                 NULL},
    {&reply_ipid,             (wpw_t)insert_uint16,                 NULL},
    {&probe->ipid,            (wpw_t)insert_uint16,                 NULL},
    {&reply->proto,           (wpw_t)insert_byte,                   NULL},
    {&reply->tcp_flags,       (wpw_t)insert_byte,                   NULL},
    {reply->addr,             (wpw_t)insert_addr,                   table},
    {reply->v4rr,             (wpw_t)insert_ping_reply_v4rr,        table},
    {reply->v4ts,             (wpw_t)insert_ping_reply_v4ts,        table},
    {&reply->ipid32,          (wpw_t)insert_uint32,                 NULL},
    {&probe->tx,              (wpw_t)insert_timeval,                NULL},
    {reply->tsreply,          (wpw_t)insert_ping_reply_tsreply,     NULL},
    {&probe->sport,           (wpw_t)insert_uint16,                 NULL},
    {&reply->tos,             (wpw_t)insert_byte,                   NULL},
    {reply->ifname,           (wpw_t)insert_ifname,                 ifntable},
    {&reply->icmp_nhmtu,      (wpw_t)insert_uint16,                 NULL},
  };
  const int handler_cnt = sizeof(handlers) / sizeof(warts_param_writer_t);

  warts_params_write(buf, off, len, state->flags, state->flags_len,
		     state->params_len, handlers, handler_cnt);
  return;
}

static int warts_ping_params(const scamper_ping_t *ping,
			     warts_addrtable_t *table, uint8_t *flags,
			     uint16_t *flags_len, uint16_t *params_len)
{
  const warts_var_t *var;
  int j, max_id = 0;
  size_t i;

  /* unset all the flags possible */
  memset(flags, 0, ping_vars_mfb);
  *params_len = 0;

  for(i=0; i<sizeof(ping_vars)/sizeof(warts_var_t); i++)
    {
      var = &ping_vars[i];

      if(var->id == WARTS_PING_ADDR_SRC_GID ||
	 var->id == WARTS_PING_ADDR_DST_GID ||
	 (var->id == WARTS_PING_ADDR_SRC      && ping->src == NULL) ||
	 (var->id == WARTS_PING_ADDR_DST      && ping->dst == NULL) ||
	 (var->id == WARTS_PING_ADDR_RTR      && ping->rtr == NULL) ||
	 (var->id == WARTS_PING_LIST_ID       && ping->list == NULL) ||
	 (var->id == WARTS_PING_CYCLE_ID      && ping->cycle == NULL) ||
	 (var->id == WARTS_PING_USERID        && ping->userid == 0) ||
	 (var->id == WARTS_PING_DATA_LEN      && ping->datalen == 0) ||
	 (var->id == WARTS_PING_PROBE_METHOD  && ping->method == 0) ||
	 (var->id == WARTS_PING_PROBE_TOS     && ping->tos == 0) ||
	 (var->id == WARTS_PING_PROBE_SPORT   && ping->sport == 0) ||
	 (var->id == WARTS_PING_PROBE_DPORT   && ping->dport == 0) ||
	 (var->id == WARTS_PING_FLAGS8        && (ping->flags & 0xFF) == 0) ||
	 (var->id == WARTS_PING_FLAGS         && (ping->flags & ~0xFF) == 0) ||
	 (var->id == WARTS_PING_STOP_COUNT    && ping->stop_count == 0) ||
	 (var->id == WARTS_PING_REPLY_PMTU    && ping->pmtu == 0) ||
	 (var->id == WARTS_PING_PROBE_TIMEOUT && ping->wait_timeout.tv_sec == ping->wait_probe.tv_sec) ||
	 (var->id == WARTS_PING_PROBE_WAIT_US && ping->wait_probe.tv_usec == 0) ||
	 (var->id == WARTS_PING_PROBE_TIMEOUT_US && ping->wait_timeout.tv_usec == 0) ||
	 (var->id == WARTS_PING_PROBE_TCPACK  && ping->tcpack == 0) ||
	 (var->id == WARTS_PING_PROBE_TCPSEQ  && ping->tcpseq == 0) ||
	 (var->id == WARTS_PING_PROBE_ICMPSUM &&
	  (ping->icmpsum == 0 || (ping->flags & SCAMPER_PING_FLAG_ICMPSUM) == 0)) ||
	 (var->id == WARTS_PING_DATA_BYTES    && ping->datalen == 0) ||
	 (var->id == WARTS_PING_PROBE_TSPS    && ping->tsps == NULL))
	{
	  continue;
	}

      flag_set(flags, var->id, &max_id);

      if(var->id == WARTS_PING_ADDR_SRC)
	{
	  if(warts_addr_size(table, ping->src, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_PING_ADDR_DST)
	{
	  if(warts_addr_size(table, ping->dst, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_PING_ADDR_RTR)
	{
	  if(warts_addr_size_static(ping->rtr, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_PING_DATA_BYTES)
	{
	  *params_len += ping->datalen;
	}
      else if(var->id == WARTS_PING_PROBE_TSPS)
	{
	  *params_len += 1;
	  for(j=0; j<ping->tsps->ipc; j++)
	    if(warts_addr_size(table, ping->tsps->ips[j], params_len) != 0)
	      return -1;
	}
      else
	{
	  assert(var->size >= 0);
	  *params_len += var->size;
	}
    }

  *flags_len = fold_flags(flags, max_id);

  return 0;
}

static void insert_ping_probe_tsps(uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   const scamper_ping_v4ts_t *ts, void *param)
{
  uint8_t i;

  assert(len - *off >= 1);
  buf[(*off)++] = ts->ipc;
  for(i=0; i<ts->ipc; i++)
    insert_addr(buf, off, len, ts->ips[i], param);

  return;
}

static int extract_ping_probe_tsps(const uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   scamper_ping_v4ts_t **out, void *param)
{
  scamper_addr_t *addr;
  uint8_t i, ipc;

  /* make sure there is room for the ip count */
  if(*off >= len || len - *off < 1)
    return -1;

  ipc = buf[(*off)++];

  if((*out = scamper_ping_v4ts_alloc(ipc)) == NULL)
    return -1;

  for(i=0; i<ipc; i++)
    {
      if(extract_addr(buf, off, len, &addr, param) != 0)
	return -1;
      (*out)->ips[i] = addr;
    }

  return 0;
}

static int warts_ping_params_read(scamper_ping_t *ping, warts_state_t *state,
				  warts_addrtable_t *table,
				  uint8_t *buf, uint32_t *off, uint32_t len)
{
  uint8_t  flags8 = 0;
  uint8_t  wait_probe_sec = 0;
  uint32_t wait_probe_usec = 0;
  uint8_t  wait_timeout_sec = 0;
  uint32_t wait_timeout_usec = 0;

  warts_param_reader_t handlers[] = {
    {&ping->list,          (wpr_t)extract_list,            state},
    {&ping->cycle,         (wpr_t)extract_cycle,           state},
    {&ping->src,           (wpr_t)extract_addr_gid,        state},
    {&ping->dst,           (wpr_t)extract_addr_gid,        state},
    {&ping->start,         (wpr_t)extract_timeval,         NULL},
    {&ping->stop_reason,   (wpr_t)extract_byte,            NULL},
    {&ping->stop_data,     (wpr_t)extract_byte,            NULL},
    {&ping->datalen,       (wpr_t)extract_uint16,          NULL},
    {&ping->data,          (wpr_t)extract_bytes_alloc,     &ping->datalen},
    {&ping->attempts,      (wpr_t)extract_uint16,          NULL},
    {&ping->size,          (wpr_t)extract_uint16,          NULL},
    {&wait_probe_sec,      (wpr_t)extract_byte,            NULL},
    {&ping->ttl,           (wpr_t)extract_byte,            NULL},
    {&ping->stop_count,    (wpr_t)extract_uint16,          NULL},
    {&ping->ping_sent,     (wpr_t)extract_uint16,          NULL},
    {&ping->method,        (wpr_t)extract_byte,            NULL},
    {&ping->sport,         (wpr_t)extract_uint16,          NULL},
    {&ping->dport,         (wpr_t)extract_uint16,          NULL},
    {&ping->userid,        (wpr_t)extract_uint32,          NULL},
    {&ping->src,           (wpr_t)extract_addr,            table},
    {&ping->dst,           (wpr_t)extract_addr,            table},
    {&flags8,              (wpr_t)extract_byte,            NULL},
    {&ping->tos,           (wpr_t)extract_byte,            NULL},
    {&ping->tsps,          (wpr_t)extract_ping_probe_tsps, table},
    {&ping->icmpsum,       (wpr_t)extract_uint16,          NULL},
    {&ping->pmtu,          (wpr_t)extract_uint16,          NULL},
    {&wait_timeout_sec,    (wpr_t)extract_byte,            NULL},
    {&wait_probe_usec,     (wpr_t)extract_uint32,          NULL},
    {&ping->tcpack,        (wpr_t)extract_uint32,          NULL},
    {&ping->flags,         (wpr_t)extract_uint32,          NULL},
    {&ping->tcpseq,        (wpr_t)extract_uint32,          NULL},
    {&ping->rtr,           (wpr_t)extract_addr_static,     NULL},
    {&wait_timeout_usec,   (wpr_t)extract_uint32,          NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);
  uint32_t o = *off;
  int rc;

  if((rc = warts_params_read(buf, off, len, handlers, handler_cnt)) != 0)
    return rc;
  if(ping->dst == NULL)
    return -1;

  ping->wait_probe.tv_sec = wait_probe_sec;
  ping->wait_probe.tv_usec = wait_probe_usec;
  ping->wait_timeout.tv_sec = wait_timeout_sec;
  ping->wait_timeout.tv_usec = wait_timeout_usec;

  if(flag_isset(&buf[o], WARTS_PING_PROBE_TIMEOUT) == 0)
    ping->wait_timeout.tv_sec = ping->wait_probe.tv_sec;
  if(flag_isset(&buf[o], WARTS_PING_FLAGS) == 0 &&
     flag_isset(&buf[o], WARTS_PING_FLAGS8) != 0)
    ping->flags = flags8;
  return 0;
}

static int warts_ping_params_write(const scamper_ping_t *ping,
				   const scamper_file_t *sf,
				   warts_addrtable_t *table,
				   uint8_t *buf, uint32_t *off,
				   const uint32_t len,
				   const uint8_t *flags,
				   const uint16_t flags_len,
				   const uint16_t params_len)
{
  uint32_t list_id, cycle_id;
  uint16_t pad_len = ping->datalen;
  uint8_t flags8 = ping->flags & 0xFF;
  uint8_t wait_probe_sec = ping->wait_probe.tv_sec;
  uint32_t wait_probe_usec = ping->wait_probe.tv_usec;
  uint8_t wait_timeout_sec = ping->wait_timeout.tv_sec;
  uint32_t wait_timeout_usec = ping->wait_timeout.tv_usec;
  warts_param_writer_t handlers[] = {
    {&list_id,             (wpw_t)insert_uint32,          NULL},
    {&cycle_id,            (wpw_t)insert_uint32,          NULL},
    {NULL,                 NULL,                          NULL},
    {NULL,                 NULL,                          NULL},
    {&ping->start,         (wpw_t)insert_timeval,         NULL},
    {&ping->stop_reason,   (wpw_t)insert_byte,            NULL},
    {&ping->stop_data,     (wpw_t)insert_byte,            NULL},
    {&ping->datalen,       (wpw_t)insert_uint16,          NULL},
    {ping->data,           (wpw_t)insert_bytes_uint16,    &pad_len},
    {&ping->attempts,      (wpw_t)insert_uint16,          NULL},
    {&ping->size,          (wpw_t)insert_uint16,          NULL},
    {&wait_probe_sec,      (wpw_t)insert_byte,            NULL},
    {&ping->ttl,           (wpw_t)insert_byte,            NULL},
    {&ping->stop_count,    (wpw_t)insert_uint16,          NULL},
    {&ping->ping_sent,     (wpw_t)insert_uint16,          NULL},
    {&ping->method,        (wpw_t)insert_byte,            NULL},
    {&ping->sport,         (wpw_t)insert_uint16,          NULL},
    {&ping->dport,         (wpw_t)insert_uint16,          NULL},
    {&ping->userid,        (wpw_t)insert_uint32,          NULL},
    {ping->src,            (wpw_t)insert_addr,            table},
    {ping->dst,            (wpw_t)insert_addr,            table},
    {&flags8,              (wpw_t)insert_byte,            NULL},
    {&ping->tos,           (wpw_t)insert_byte,            NULL},
    {ping->tsps,           (wpw_t)insert_ping_probe_tsps, table},
    {&ping->icmpsum,       (wpw_t)insert_uint16,          NULL},
    {&ping->pmtu,          (wpw_t)insert_uint16,          NULL},
    {&wait_timeout_sec,    (wpw_t)insert_byte,            NULL},
    {&wait_probe_usec,     (wpw_t)insert_uint32,          NULL},
    {&ping->tcpack,        (wpw_t)insert_uint32,          NULL},
    {&ping->flags,         (wpw_t)insert_uint32,          NULL},
    {&ping->tcpseq,        (wpw_t)insert_uint32,          NULL},
    {ping->rtr,            (wpw_t)insert_addr_static,     NULL},
    {&wait_timeout_usec,   (wpw_t)insert_uint32,          NULL},
  };

  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);

  if(warts_list_getid(sf,  ping->list,  &list_id)  == -1) return -1;
  if(warts_cycle_getid(sf, ping->cycle, &cycle_id) == -1) return -1;

  warts_params_write(buf, off, len, flags, flags_len, params_len, handlers,
		     handler_cnt);
  return 0;
}

int scamper_file_warts_ping_read(scamper_file_t *sf, const warts_hdr_t *hdr,
				 scamper_ping_t **ping_out)
{
  warts_state_t *state = scamper_file_getstate(sf);
  scamper_ping_t *ping = NULL;
  warts_addrtable_t *table = NULL;
  warts_ifnametable_t *ifntable = NULL;
  uint8_t *buf = NULL;
  uint32_t off = 0;
  uint16_t i, reply_count;

  if(warts_read(sf, &buf, hdr->len) != 0)
    {
      goto err;
    }
  if(buf == NULL)
    {
      *ping_out = NULL;
      return 0;
    }

  if((ping = scamper_ping_alloc()) == NULL)
    {
      goto err;
    }

  if((table = warts_addrtable_alloc_byid()) == NULL ||
     (ifntable = warts_ifnametable_alloc_byid()) == NULL)
    goto err;

  if(warts_ping_params_read(ping, state, table, buf, &off, hdr->len) != 0)
    {
      goto err;
    }

  /* determine how many replies to read */
  if(extract_uint16(buf, &off, hdr->len, &reply_count, NULL) != 0)
    {
      goto err;
    }

  /* allocate the ping->probes array */
  if(scamper_ping_probes_alloc(ping, ping->ping_sent) != 0)
    {
      goto err;
    }

  /* if there are no replies, then we are done */
  if(reply_count == 0)
    {
      goto done;
    }

  /* for each reply, read it and insert it into the ping structure */
  for(i=0; i<reply_count; i++)
    {
      if(warts_ping_reply_read(ping, state, table, ifntable,
			       buf, &off, hdr->len) != 0)
	goto err;
    }

 done:
  warts_addrtable_free(table);
  warts_ifnametable_free(ifntable);
  *ping_out = ping;
  free(buf);
  return 0;

 err:
  if(table != NULL) warts_addrtable_free(table);
  if(ifntable != NULL) warts_ifnametable_free(ifntable);
  if(buf != NULL) free(buf);
  if(ping != NULL) scamper_ping_free(ping);
  return -1;
}

int scamper_file_warts_ping_write(const scamper_file_t *sf,
				  const scamper_ping_t *ping, void *p)
{
  warts_addrtable_t *table = NULL;
  warts_ifnametable_t *ifntable = NULL;
  warts_ping_reply_t *reply_state = NULL;
  scamper_ping_probe_t *probe;
  uint8_t *buf = NULL;
  uint8_t  flags[ping_vars_mfb];
  uint16_t flags_len, params_len;
  uint32_t len, off = 0;
  uint16_t i, j, k, reply_count;
  size_t   size;

  if((table = warts_addrtable_alloc_byaddr()) == NULL ||
     (ifntable = warts_ifnametable_alloc_byname()) == NULL)
    goto err;

  /* figure out which ping data items we'll store in this record */
  if(warts_ping_params(ping, table, flags, &flags_len, &params_len) != 0)
    goto err;

  /* length of the ping's flags, parameters, and number of reply records */
  len = 8 + flags_len + 2 + params_len + 2;

  if((reply_count = scamper_ping_reply_total(ping)) > 0)
    {
      size = reply_count * sizeof(warts_ping_reply_t);
      if((reply_state = (warts_ping_reply_t *)malloc_zero(size)) == NULL)
	{
	  goto err;
	}

      k = 0;
      for(i=0; i<ping->ping_sent; i++)
	{
	  if((probe = ping->probes[i]) == NULL)
	    continue;

	  for(j=0; j<probe->replyc; j++)
	    {
	      if(warts_ping_reply_state(sf, ping, probe, probe->replies[j],
					&reply_state[k++],
					table, ifntable, &len) != 0)
		{
		  goto err;
		}
	    }
	}
    }

  if((buf = malloc_zero(len)) == NULL)
    {
      goto err;
    }

  insert_wartshdr(buf, &off, len, SCAMPER_FILE_OBJ_PING);

  if(warts_ping_params_write(ping, sf, table, buf, &off, len,
			     flags, flags_len, params_len) == -1)
    {
      goto err;
    }

  /* reply record count */
  insert_uint16(buf, &off, len, &reply_count, NULL);

  /* write each ping reply record */
  for(i=0; i<reply_count; i++)
    {
      warts_ping_reply_write(&reply_state[i], table, ifntable, buf, &off, len);
    }
  if(reply_state != NULL)
    {
      free(reply_state);
      reply_state = NULL;
    }

  assert(off == len);

  if(warts_write(sf, buf, len, p) == -1)
    {
      goto err;
    }

  warts_addrtable_free(table);
  warts_ifnametable_free(ifntable);
  free(buf);
  return 0;

 err:
  if(table != NULL) warts_addrtable_free(table);
  if(ifntable != NULL) warts_ifnametable_free(ifntable);
  if(reply_state != NULL) free(reply_state);
  if(buf != NULL) free(buf);
  return -1;
}
