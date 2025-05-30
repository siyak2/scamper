/*
 * scamper_trace_warts.c
 *
 * Copyright (C) 2004-2006 Matthew Luckie
 * Copyright (C) 2006-2011 The University of Waikato
 * Copyright (C) 2014      The Regents of the University of California
 * Copyright (C) 2015      The University of Waikato
 * Copyright (C) 2015-2025 Matthew Luckie
 * Author: Matthew Luckie
 *
 * $Id: scamper_trace_warts.c,v 1.59 2025/05/01 02:58:04 mjl Exp $
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
#include "scamper_icmpext_int.h"
#include "scamper_trace.h"
#include "scamper_trace_int.h"
#include "scamper_file.h"
#include "scamper_file_warts.h"
#include "scamper_trace_warts.h"

#include "mjl_splaytree.h"
#include "mjl_list.h"
#include "utils.h"

/*
 * trace attributes: 2 bytes each.
 * the first 4 bits are the type, the second 12 bits are the length
 */
#define WARTS_TRACE_ATTR_HDR(type, len) ((type << 12) | len)
#define WARTS_TRACE_ATTR_HDR_TYPE(hdr)  ((hdr >> 12) & 0xf)
#define WARTS_TRACE_ATTR_HDR_LEN(hdr)    (hdr & 0x0fff)
#define WARTS_TRACE_ATTR_EOF       0x0000
#define WARTS_TRACE_ATTR_PMTUD     0x1
#define WARTS_TRACE_ATTR_LASTDITCH 0x2
#define WARTS_TRACE_ATTR_DTREE     0x3

/*
 * the optional bits of a trace structure
 */
#define WARTS_TRACE_LIST_ID        1   /* list id assigned by warts */
#define WARTS_TRACE_CYCLE_ID       2   /* cycle id assigned by warts */
#define WARTS_TRACE_ADDR_SRC_GID   3   /* src address key, deprecated */
#define WARTS_TRACE_ADDR_DST_GID   4   /* dst address key, deprecated */
#define WARTS_TRACE_START          5   /* start timestamp */
#define WARTS_TRACE_STOP_R         6   /* stop reason */
#define WARTS_TRACE_STOP_D         7   /* stop data */
#define WARTS_TRACE_FLAGS8         8   /* 8 bits of flags */
#define WARTS_TRACE_ATTEMPTS       9   /* attempts */
#define WARTS_TRACE_HOPLIMIT       10  /* hoplimit */
#define WARTS_TRACE_TYPE           11  /* type */
#define WARTS_TRACE_PROBE_S        12  /* probe size */
#define WARTS_TRACE_PORT_SRC       13  /* source port */
#define WARTS_TRACE_PORT_DST       14  /* destination port */
#define WARTS_TRACE_FIRSTHOP       15  /* first hop */
#define WARTS_TRACE_TOS            16  /* type of service bits */
#define WARTS_TRACE_WAIT           17  /* how long to wait per probe */
#define WARTS_TRACE_LOOPS          18  /* max loops before stopping */
#define WARTS_TRACE_HOPCOUNT       19  /* hop count */
#define WARTS_TRACE_GAPLIMIT       20  /* gap limit */
#define WARTS_TRACE_GAPACTION      21  /* gap action */
#define WARTS_TRACE_LOOPACTION     22  /* loop action */
#define WARTS_TRACE_PROBEC         23  /* probe count */
#define WARTS_TRACE_WAITPROBE      24  /* min wait between probes */
#define WARTS_TRACE_CONFIDENCE     25  /* confidence level to attain */
#define WARTS_TRACE_ADDR_SRC       26  /* source address key */
#define WARTS_TRACE_ADDR_DST       27  /* destination address key */
#define WARTS_TRACE_USERID         28  /* user id */
#define WARTS_TRACE_OFFSET         29  /* IP offset to use in fragments */
#define WARTS_TRACE_ADDR_RTR       30  /* destination address key */
#define WARTS_TRACE_SQUERIES       31  /* squeries */
#define WARTS_TRACE_FLAGS          32  /* 32 bits of flags */
#define WARTS_TRACE_STOP_HOP       33  /* stop hop */

static const warts_var_t trace_vars[] =
{
  {WARTS_TRACE_LIST_ID,      4},
  {WARTS_TRACE_CYCLE_ID,     4},
  {WARTS_TRACE_ADDR_SRC_GID, 4},
  {WARTS_TRACE_ADDR_DST_GID, 4},
  {WARTS_TRACE_START,        8},
  {WARTS_TRACE_STOP_R,       1},
  {WARTS_TRACE_STOP_D,       1},
  {WARTS_TRACE_FLAGS8,       1},
  {WARTS_TRACE_ATTEMPTS,     1},
  {WARTS_TRACE_HOPLIMIT,     1},
  {WARTS_TRACE_TYPE,         1},
  {WARTS_TRACE_PROBE_S,      2},
  {WARTS_TRACE_PORT_SRC,     2},
  {WARTS_TRACE_PORT_DST,     2},
  {WARTS_TRACE_FIRSTHOP,     1},
  {WARTS_TRACE_TOS,          1},
  {WARTS_TRACE_WAIT,         1},
  {WARTS_TRACE_LOOPS,        1},
  {WARTS_TRACE_HOPCOUNT,     2},
  {WARTS_TRACE_GAPLIMIT,     1},
  {WARTS_TRACE_GAPACTION,    1},
  {WARTS_TRACE_LOOPACTION,   1},
  {WARTS_TRACE_PROBEC,       2},
  {WARTS_TRACE_WAITPROBE,    1},
  {WARTS_TRACE_CONFIDENCE,   1},
  {WARTS_TRACE_ADDR_SRC,    -1},
  {WARTS_TRACE_ADDR_DST,    -1},
  {WARTS_TRACE_USERID,       4},
  {WARTS_TRACE_OFFSET,       2},
  {WARTS_TRACE_ADDR_RTR,    -1},
  {WARTS_TRACE_SQUERIES,     1},
  {WARTS_TRACE_FLAGS,        4},
  {WARTS_TRACE_STOP_HOP,     1},
};
#define trace_vars_mfb WARTS_VAR_MFB(trace_vars)

/*
 * the optional bits of a trace pmtud structure
 */
#define WARTS_TRACE_PMTUD_IFMTU  1       /* interface mtu */
#define WARTS_TRACE_PMTUD_PMTU   2       /* path mtu */
#define WARTS_TRACE_PMTUD_OUTMTU 3       /* mtu to gateway */
#define WARTS_TRACE_PMTUD_VER    4       /* version of data collection */
#define WARTS_TRACE_PMTUD_NOTEC  5       /* number of notes attached */
static const warts_var_t pmtud_vars[] =
{
  {WARTS_TRACE_PMTUD_IFMTU,  2},
  {WARTS_TRACE_PMTUD_PMTU,   2},
  {WARTS_TRACE_PMTUD_OUTMTU, 2},
  {WARTS_TRACE_PMTUD_VER,    1},
  {WARTS_TRACE_PMTUD_NOTEC,  1},
};
#define pmtud_vars_mfb WARTS_VAR_MFB(pmtud_vars)

#define WARTS_TRACE_PMTUD_NOTE_TYPE  1      /* type of note */
#define WARTS_TRACE_PMTUD_NOTE_NHMTU 2      /* nhmtu measured */
#define WARTS_TRACE_PMTUD_NOTE_REPLY 3      /* reply record; index to reply */
static const warts_var_t pmtud_note_vars[] =
{
  {WARTS_TRACE_PMTUD_NOTE_TYPE,  1},
  {WARTS_TRACE_PMTUD_NOTE_NHMTU, 2},
  {WARTS_TRACE_PMTUD_NOTE_REPLY, 2},
};
#define pmtud_note_vars_mfb WARTS_VAR_MFB(pmtud_note_vars)

/*
 * the optional bits of a trace dtree structure
 */
#define WARTS_TRACE_DTREE_LSS_STOP_GID 1 /* deprecated */
#define WARTS_TRACE_DTREE_GSS_STOP_GID 2 /* deprecated */
#define WARTS_TRACE_DTREE_FIRSTHOP     3 /* firsthop */
#define WARTS_TRACE_DTREE_LSS_STOP     4 /* lss stop address */
#define WARTS_TRACE_DTREE_GSS_STOP     5 /* gss stop address */
#define WARTS_TRACE_DTREE_LSS_NAME     6 /* lss name */
#define WARTS_TRACE_DTREE_FLAGS        7 /* flags */
static const warts_var_t trace_dtree_vars[] =
{
  {WARTS_TRACE_DTREE_LSS_STOP_GID,  4},
  {WARTS_TRACE_DTREE_GSS_STOP_GID,  4},
  {WARTS_TRACE_DTREE_FIRSTHOP,      1},
  {WARTS_TRACE_DTREE_LSS_STOP,     -1},
  {WARTS_TRACE_DTREE_GSS_STOP,     -1},
  {WARTS_TRACE_DTREE_LSS_NAME,     -1},
  {WARTS_TRACE_DTREE_FLAGS,         1},
};
#define trace_dtree_vars_mfb WARTS_VAR_MFB(trace_dtree_vars)

/*
 * the optional bits of a trace hop structure
 */
#define WARTS_TRACE_HOP_ADDR_GID     1       /* address id, deprecated */
#define WARTS_TRACE_HOP_PROBE_TTL    2       /* probe ttl */
#define WARTS_TRACE_HOP_REPLY_TTL    3       /* reply ttl */
#define WARTS_TRACE_HOP_FLAGS        4       /* flags */
#define WARTS_TRACE_HOP_PROBE_ID     5       /* probe id */
#define WARTS_TRACE_HOP_RTT          6       /* round trip time */
#define WARTS_TRACE_HOP_ICMP_TC      7       /* icmp type / code */
#define WARTS_TRACE_HOP_PROBE_SIZE   8       /* probe size */
#define WARTS_TRACE_HOP_REPLY_SIZE   9       /* reply size */
#define WARTS_TRACE_HOP_REPLY_IPID   10      /* ipid of reply packet */
#define WARTS_TRACE_HOP_REPLY_IPTOS  11      /* tos bits of reply packet */
#define WARTS_TRACE_HOP_NHMTU        12      /* next hop mtu in ptb message */
#define WARTS_TRACE_HOP_Q_IPLEN      13      /* ip->len from inside icmp */
#define WARTS_TRACE_HOP_Q_IPTTL      14      /* ip->ttl from inside icmp */
#define WARTS_TRACE_HOP_TCP_FLAGS    15      /* tcp->flags of reply packet */
#define WARTS_TRACE_HOP_Q_IPTOS      16      /* ip->tos byte inside icmp */
#define WARTS_TRACE_HOP_ICMPEXT      17      /* RFC 4884 icmp extension data */
#define WARTS_TRACE_HOP_ADDR         18      /* address */
#define WARTS_TRACE_HOP_TX           19      /* transmit time */
#define WARTS_TRACE_HOP_NAME         20      /* name for IP address */

static const warts_var_t hop_vars[] =
{
  {WARTS_TRACE_HOP_ADDR_GID,     4},
  {WARTS_TRACE_HOP_PROBE_TTL,    1},
  {WARTS_TRACE_HOP_REPLY_TTL,    1},
  {WARTS_TRACE_HOP_FLAGS,        1},
  {WARTS_TRACE_HOP_PROBE_ID,     1},
  {WARTS_TRACE_HOP_RTT,          4},
  {WARTS_TRACE_HOP_ICMP_TC,      2},
  {WARTS_TRACE_HOP_PROBE_SIZE,   2},
  {WARTS_TRACE_HOP_REPLY_SIZE,   2},
  {WARTS_TRACE_HOP_REPLY_IPID,   2},
  {WARTS_TRACE_HOP_REPLY_IPTOS,  1},
  {WARTS_TRACE_HOP_NHMTU,        2},
  {WARTS_TRACE_HOP_Q_IPLEN,      2},
  {WARTS_TRACE_HOP_Q_IPTTL,      1},
  {WARTS_TRACE_HOP_TCP_FLAGS,    1},
  {WARTS_TRACE_HOP_Q_IPTOS,      1},
  {WARTS_TRACE_HOP_ICMPEXT,     -1},
  {WARTS_TRACE_HOP_ADDR,        -1},
  {WARTS_TRACE_HOP_TX,           8},
  {WARTS_TRACE_HOP_NAME,        -1},
};
#define hop_vars_mfb WARTS_VAR_MFB(hop_vars)

typedef struct warts_trace_hop
{
  scamper_trace_probe_t *probe;
  scamper_trace_reply_t *reply;
  uint8_t                flags[WARTS_VAR_MFB(hop_vars)];
  uint16_t               flags_len;
  uint16_t               params_len;
} warts_trace_hop_t;

typedef struct warts_trace_dtree
{
  uint8_t              flags[WARTS_VAR_MFB(trace_dtree_vars)];
  uint16_t             flags_len;
  uint16_t             params_len;
  uint32_t             len;
} warts_trace_dtree_t;

typedef struct warts_trace_pmtud_note
{
  uint8_t              flags[pmtud_note_vars_mfb];
  uint16_t             flags_len;
  uint16_t             params_len;
  uint16_t             hop;
} warts_trace_pmtud_note_t;

typedef struct warts_trace_pmtud
{
  uint8_t                   flags[pmtud_vars_mfb];
  uint16_t                  flags_len;
  uint16_t                  params_len;
  warts_trace_hop_t        *hops;
  uint16_t                  hopc;
  warts_trace_pmtud_note_t *notes;
  uint32_t                  len;
} warts_trace_pmtud_t;

typedef struct trace_hop
{
  scamper_trace_probe_t    *probe;
  scamper_trace_reply_t    *reply;
} trace_hop_t;

static int warts_trace_params(const scamper_trace_t *trace,
			      warts_addrtable_t *table, uint8_t *flags,
			      uint16_t *flags_len, uint16_t *params_len)
{
  int max_id = 0;
  const warts_var_t *var;
  size_t i;

  /* unset all the flags possible */
  memset(flags, 0, trace_vars_mfb);
  *params_len = 0;

  for(i=0; i<sizeof(trace_vars)/sizeof(warts_var_t); i++)
    {
      var = &trace_vars[i];

      if(var->id == WARTS_TRACE_ADDR_SRC_GID ||
	 var->id == WARTS_TRACE_ADDR_DST_GID ||
	 (var->id == WARTS_TRACE_USERID && trace->userid == 0) ||
	 (var->id == WARTS_TRACE_OFFSET && trace->offset == 0) ||
	 (var->id == WARTS_TRACE_FLAGS8 && (trace->flags & 0xFF) == 0) ||
	 (var->id == WARTS_TRACE_FLAGS && (trace->flags & ~0xFF) == 0) ||
	 (var->id == WARTS_TRACE_ADDR_RTR && trace->rtr == NULL) ||
	 (var->id == WARTS_TRACE_ADDR_SRC && trace->src == NULL) ||
	 (var->id == WARTS_TRACE_SQUERIES && trace->squeries < 2) ||
	 (var->id == WARTS_TRACE_STOP_HOP && trace->stop_hop == 0))
	{
	  continue;
	}

      flag_set(flags, var->id, &max_id);

      if(var->id == WARTS_TRACE_ADDR_SRC)
	{
	  if(warts_addr_size(table, trace->src, params_len) != 0)
	    return -1;
	  continue;
	}
      else if(var->id == WARTS_TRACE_ADDR_DST)
	{
	  if(warts_addr_size(table, trace->dst, params_len) != 0)
	    return -1;
	  continue;
	}
      else if(var->id == WARTS_TRACE_ADDR_RTR)
	{
	  if(warts_addr_size_static(trace->rtr, params_len) != 0)
	    return -1;
	  continue;
	}

      assert(var->size >= 0);
      *params_len += var->size;
    }

  *flags_len = fold_flags(flags, max_id);
  return 0;
}

static int warts_trace_params_read(scamper_trace_t *trace,warts_state_t *state,
				   warts_addrtable_t *table,
				   uint8_t *buf, uint32_t *off, uint32_t len)
{
  uint8_t flags8 = 0;
  uint8_t wait_probe = 0, wait_timeout = 0;
  warts_param_reader_t handlers[] = {
    {&trace->list,        (wpr_t)extract_list,     state},
    {&trace->cycle,       (wpr_t)extract_cycle,    state},
    {&trace->src,         (wpr_t)extract_addr_gid, state},
    {&trace->dst,         (wpr_t)extract_addr_gid, state},
    {&trace->start,       (wpr_t)extract_timeval,  NULL},
    {&trace->stop_reason, (wpr_t)extract_byte,     NULL},
    {&trace->stop_data,   (wpr_t)extract_byte,     NULL},
    {&flags8,             (wpr_t)extract_byte,     NULL},
    {&trace->attempts,    (wpr_t)extract_byte,     NULL},
    {&trace->hoplimit,    (wpr_t)extract_byte,     NULL},
    {&trace->type,        (wpr_t)extract_byte,     NULL},
    {&trace->probe_size,  (wpr_t)extract_uint16,   NULL},
    {&trace->sport,       (wpr_t)extract_uint16,   NULL},
    {&trace->dport,       (wpr_t)extract_uint16,   NULL},
    {&trace->firsthop,    (wpr_t)extract_byte,     NULL},
    {&trace->tos,         (wpr_t)extract_byte,     NULL},
    {&wait_timeout,       (wpr_t)extract_byte,     NULL},
    {&trace->loops,       (wpr_t)extract_byte,     NULL},
    {&trace->hop_count,   (wpr_t)extract_uint16,   NULL},
    {&trace->gaplimit,    (wpr_t)extract_byte,     NULL},
    {&trace->gapaction,   (wpr_t)extract_byte,     NULL},
    {&trace->loopaction,  (wpr_t)extract_byte,     NULL},
    {&trace->probec,      (wpr_t)extract_uint16,   NULL},
    {&wait_probe,         (wpr_t)extract_byte,     NULL},
    {&trace->confidence,  (wpr_t)extract_byte,     NULL},
    {&trace->src,         (wpr_t)extract_addr,     table},
    {&trace->dst,         (wpr_t)extract_addr,     table},
    {&trace->userid,      (wpr_t)extract_uint32,   NULL},
    {&trace->offset,      (wpr_t)extract_uint16,   NULL},
    {&trace->rtr,         (wpr_t)extract_addr_static, NULL},
    {&trace->squeries,    (wpr_t)extract_byte,     NULL},
    {&trace->flags,       (wpr_t)extract_uint32,   NULL},
    {&trace->stop_hop,    (wpr_t)extract_byte,     NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);
  uint32_t o = *off;
  int rc;

  if((rc = warts_params_read(buf, off, len, handlers, handler_cnt)) != 0)
    return rc;
  if(trace->dst == NULL)
    return -1;
  if(trace->firsthop == 0)
    trace->firsthop = 1;
  if(trace->squeries < 2)
    trace->squeries = 1;

  trace->wait_timeout.tv_sec = wait_timeout;
  trace->wait_timeout.tv_usec = 0;
  trace->wait_probe.tv_sec = wait_probe / 100;
  trace->wait_probe.tv_usec = (wait_probe % 100) * 10000;

  if(flag_isset(&buf[o], WARTS_TRACE_FLAGS) == 0 &&
     flag_isset(&buf[o], WARTS_TRACE_FLAGS8) != 0)
    trace->flags = flags8;

  return 0;
}

static int warts_trace_params_write(const scamper_trace_t *trace,
				    const scamper_file_t *sf,
				    warts_addrtable_t *table,
				    uint8_t *buf, uint32_t *off,
				    const uint32_t len,
				    const uint8_t *flags,
				    const uint16_t flags_len,
				    const uint16_t params_len)
{
  uint32_t list_id, cycle_id;
  uint8_t flags8 = trace->flags & 0xFF;
  uint8_t wait_probe, wait_timeout;
  warts_param_writer_t handlers[] = {
    {&list_id,            (wpw_t)insert_uint32,  NULL},
    {&cycle_id,           (wpw_t)insert_uint32,  NULL},
    {NULL,                NULL,                  NULL},
    {NULL,                NULL,                  NULL},
    {&trace->start,       (wpw_t)insert_timeval, NULL},
    {&trace->stop_reason, (wpw_t)insert_byte,    NULL},
    {&trace->stop_data,   (wpw_t)insert_byte,    NULL},
    {&flags8,             (wpw_t)insert_byte,    NULL},
    {&trace->attempts,    (wpw_t)insert_byte,    NULL},
    {&trace->hoplimit,    (wpw_t)insert_byte,    NULL},
    {&trace->type,        (wpw_t)insert_byte,    NULL},
    {&trace->probe_size,  (wpw_t)insert_uint16,  NULL},
    {&trace->sport,       (wpw_t)insert_uint16,  NULL},
    {&trace->dport,       (wpw_t)insert_uint16,  NULL},
    {&trace->firsthop,    (wpw_t)insert_byte,    NULL},
    {&trace->tos,         (wpw_t)insert_byte,    NULL},
    {&wait_timeout,       (wpw_t)insert_byte,    NULL},
    {&trace->loops,       (wpw_t)insert_byte,    NULL},
    {&trace->hop_count,   (wpw_t)insert_uint16,  NULL},
    {&trace->gaplimit,    (wpw_t)insert_byte,    NULL},
    {&trace->gapaction,   (wpw_t)insert_byte,    NULL},
    {&trace->loopaction,  (wpw_t)insert_byte,    NULL},
    {&trace->probec,      (wpw_t)insert_uint16,  NULL},
    {&wait_probe,         (wpw_t)insert_byte,    NULL},
    {&trace->confidence,  (wpw_t)insert_byte,    NULL},
    {trace->src,          (wpw_t)insert_addr,    table},
    {trace->dst,          (wpw_t)insert_addr,    table},
    {&trace->userid,      (wpw_t)insert_uint32,  NULL},
    {&trace->offset,      (wpw_t)insert_uint16,  NULL},
    {trace->rtr,          (wpw_t)insert_addr_static, NULL},
    {&trace->squeries,    (wpw_t)insert_byte,    NULL},
    {&trace->flags,       (wpw_t)insert_uint32,  NULL},
    {&trace->stop_hop,    (wpw_t)insert_byte,    NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);

  if(warts_list_getid(sf,  trace->list,  &list_id)  == -1) return -1;
  if(warts_cycle_getid(sf, trace->cycle, &cycle_id) == -1) return -1;

  wait_timeout = trace->wait_timeout.tv_sec;
  wait_probe =
    (trace->wait_probe.tv_sec * 100) + (trace->wait_probe.tv_usec / 10000);

  warts_params_write(buf, off, len, flags, flags_len, params_len, handlers,
		     handler_cnt);
  return 0;
}

static int warts_trace_hop_read_icmp_tc(const uint8_t *buf, uint32_t *off,
					uint32_t len,
					scamper_trace_reply_t *reply,
					void *param)
{
  if(len - *off < 2)
    return -1;
  reply->reply_icmp_type = buf[(*off)++];
  reply->reply_icmp_code = buf[(*off)++];
  return 0;
}

static void warts_trace_hop_write_icmp_tc(uint8_t *buf, uint32_t *off,
					  uint32_t len,
					  const scamper_trace_reply_t *reply,
					  void *param)
{
  assert(len - *off >= 2);
  buf[(*off)++] = reply->reply_icmp_type;
  buf[(*off)++] = reply->reply_icmp_code;
  return;
}

static int warts_trace_hop_read_probe_id(const uint8_t *buf, uint32_t *off,
					 uint32_t len, uint8_t *out,
					 void *param)
{
  if(len - *off < 1)
    return -1;
  *out = buf[(*off)++] + 1;
  return 0;
}

static void warts_trace_hop_write_probe_id(uint8_t *buf, uint32_t *off,
					   uint32_t len,
					   const uint8_t *in, void *param)
{
  assert(len - *off >= 1);
  buf[(*off)++] = *in - 1;
  return;
}

static int warts_trace_hop_read_icmpext(const uint8_t *buf, uint32_t *off,
					uint32_t len,
					scamper_trace_reply_t *reply,
					void *param)
{
  return warts_icmpexts_read(buf, off, len, &reply->icmp_exts);
}

static void warts_trace_hop_write_icmpext(uint8_t *buf, uint32_t *off,
					  const uint32_t len,
					  const scamper_trace_reply_t *reply,
					  void *param)
{
  warts_icmpexts_write(buf, off, len, reply->icmp_exts);
  return;
}

static int warts_trace_hop_params(const scamper_trace_t *trace,
				  const scamper_trace_probe_t *probe,
				  const scamper_trace_reply_t *reply,
				  warts_addrtable_t *table, uint8_t *flags,
				  uint16_t *flags_len, uint16_t *params_len)
{
  const warts_var_t *var;
  int max_id = 0;
  size_t i;

  /* unset all the flags possible */
  memset(flags, 0, hop_vars_mfb);
  *params_len = 0;

  for(i=0; i<sizeof(hop_vars)/sizeof(warts_var_t); i++)
    {
      var = &hop_vars[i];

      /* not used any more */
      if(var->id == WARTS_TRACE_HOP_ADDR_GID ||
	 (var->id == WARTS_TRACE_HOP_ADDR && reply->addr == NULL) ||
	 (var->id == WARTS_TRACE_HOP_TCP_FLAGS &&
	  SCAMPER_TRACE_REPLY_IS_TCP(reply) == 0) ||
	 (var->id == WARTS_TRACE_HOP_ICMP_TC &&
	  SCAMPER_TRACE_REPLY_IS_ICMP(reply) == 0) ||
	 (var->id == WARTS_TRACE_HOP_Q_IPLEN &&
	  (SCAMPER_TRACE_REPLY_IS_ICMP_Q(reply) == 0 ||
	   reply->reply_icmp_q_ipl == trace->probe_size)) ||
	 (var->id == WARTS_TRACE_HOP_Q_IPTTL &&
	  (SCAMPER_TRACE_REPLY_IS_ICMP_Q(reply) == 0 ||
	   reply->reply_icmp_q_ttl == 1)) ||
	 (var->id == WARTS_TRACE_HOP_Q_IPTOS &&
	  SCAMPER_TRACE_REPLY_IS_ICMP_Q(reply) == 0) ||
	 (var->id == WARTS_TRACE_HOP_NHMTU &&
	  SCAMPER_TRACE_REPLY_IS_ICMP_PTB(reply) == 0) ||
	 (var->id == WARTS_TRACE_HOP_ICMPEXT && reply->icmp_exts == NULL) ||
	 (var->id == WARTS_TRACE_HOP_REPLY_IPID && reply->ipid == 0) ||
	 (var->id == WARTS_TRACE_HOP_TX && probe->tx.tv_sec == 0) ||
	 (var->id == WARTS_TRACE_HOP_NAME && reply->name == NULL))
	continue;

      flag_set(flags, var->id, &max_id);

      if(var->id == WARTS_TRACE_HOP_ADDR)
	{
	  if(warts_addr_size(table, reply->addr, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_TRACE_HOP_ICMPEXT)
	{
	  if(warts_icmpexts_size(reply->icmp_exts, params_len) != 0)
	    return -1;
	}
      else if(var->id == WARTS_TRACE_HOP_NAME)
	{
	  if(warts_str_size(reply->name, params_len) != 0)
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

static int warts_trace_hop_state(const scamper_trace_t *trace,
				 scamper_trace_probe_t *probe,
				 scamper_trace_reply_t *reply,
				 warts_trace_hop_t *state,
				 warts_addrtable_t *table, uint32_t *len)
{
  uint32_t x;

  /* for each hop, figure out how much space it will take up */
  if(warts_trace_hop_params(trace, probe, reply, table, state->flags,
			    &state->flags_len, &state->params_len) != 0)
    return -1;

  /* store the actual hop record with the state structure too */
  state->probe = probe;
  state->reply = reply;

  /* calculate the space required */
  x = state->flags_len + state->params_len;
  if(state->params_len != 0)
    x += 2;

  /* make sure we have sufficient space */
  if(uint32_wouldwrap(*len, x))
    return -1;

  *len += x;
  return 0;
}

static void trace_hop_free(trace_hop_t *hop)
{
  if(hop->reply != NULL)
    scamper_trace_reply_free(hop->reply);
  free(hop);
  return;
}

static int warts_trace_hop_read_int(slist_t *probes, trace_hop_t *hop,
				    warts_state_t *state,
				    warts_addrtable_t *table,
				    const uint8_t *buf,
				    uint32_t *off, uint32_t len)
{
  scamper_trace_reply_t *reply = hop->reply;
  struct timeval probe_tx;
  uint8_t probe_ttl, probe_id;
  uint16_t probe_size;
  warts_param_reader_t handlers[] = {
    {&reply->addr,             (wpr_t)extract_addr_gid,              state},
    {&probe_ttl,               (wpr_t)extract_byte,                  NULL},
    {&reply->ttl,              (wpr_t)extract_byte,                  NULL},
    {&reply->flags,            (wpr_t)extract_byte,                  NULL},
    {&probe_id,                (wpr_t)warts_trace_hop_read_probe_id, NULL},
    {&reply->rtt,              (wpr_t)extract_rtt,                   NULL},
    {reply,                    (wpr_t)warts_trace_hop_read_icmp_tc,  NULL},
    {&probe_size,              (wpr_t)extract_uint16,                NULL},
    {&reply->size,             (wpr_t)extract_uint16,                NULL},
    {&reply->ipid,             (wpr_t)extract_uint16,                NULL},
    {&reply->tos,              (wpr_t)extract_byte,                  NULL},
    {&reply->reply_icmp_nhmtu, (wpr_t)extract_uint16,                NULL},
    {&reply->reply_icmp_q_ipl, (wpr_t)extract_uint16,                NULL},
    {&reply->reply_icmp_q_ttl, (wpr_t)extract_byte,                  NULL},
    {&reply->reply_tcp_flags,  (wpr_t)extract_byte,                  NULL},
    {&reply->reply_icmp_q_tos, (wpr_t)extract_byte,                  NULL},
    {reply,                    (wpr_t)warts_trace_hop_read_icmpext,  NULL},
    {&reply->addr,             (wpr_t)extract_addr,                  table},
    {&probe_tx,                (wpr_t)extract_timeval,               NULL},
    {&reply->name,             (wpr_t)extract_string,                NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);
  scamper_trace_probe_t *probe;
  uint32_t o = *off;
  int rc;

  memset(&probe_tx, 0, sizeof(probe_tx));
  probe_ttl = 0;
  probe_id = 0;
  probe_size = 0;

  if((rc = warts_params_read(buf, off, len, handlers, handler_cnt)) != 0)
    return rc;

  if(reply->addr == NULL)
    return -1;
  if(probe_ttl == 0)
    return -1;

  if((probe = slist_tail_item(probes)) == NULL ||
     probe->ttl != probe_ttl || probe->id != probe_id ||
     probe->size != probe_size)
    {
      if((probe = scamper_trace_probe_alloc()) == NULL ||
	 slist_tail_push(probes, probe) == NULL)
	{
	  if(probe != NULL) scamper_trace_probe_free(probe);
	  return -1;
	}
      probe->ttl = probe_ttl;
      probe->id = probe_id;
      probe->size = probe_size;
      timeval_cpy(&probe->tx, &probe_tx);
    }

  hop->probe = probe;

  if(SCAMPER_TRACE_REPLY_IS_ICMP_Q(reply))
    {
      if(flag_isset(&buf[o], WARTS_TRACE_HOP_Q_IPTTL) == 0)
	reply->reply_icmp_q_ttl = 1;
      if(flag_isset(&buf[o], WARTS_TRACE_HOP_Q_IPLEN) == 0)
	reply->reply_icmp_q_ipl = probe->size;
    }

  return 0;
}

static int warts_trace_hop_read(slist_t *probes, slist_t *hops,
				warts_state_t *state, warts_addrtable_t *table,
				const uint8_t *buf,uint32_t *off,uint32_t len)
{
  trace_hop_t *hop = NULL;
  if((hop = malloc_zero(sizeof(trace_hop_t))) == NULL ||
     (hop->reply = scamper_trace_reply_alloc()) == NULL ||
     slist_tail_push(hops, hop) == NULL)
    {
      if(hop != NULL) trace_hop_free(hop);
      return -1;
    }
  return warts_trace_hop_read_int(probes, hop, state, table, buf, off, len);
}

static void warts_trace_hop_write(const warts_trace_hop_t *state,
				  warts_addrtable_t *table,
				  uint8_t *buf, uint32_t *off, uint32_t len)
{
  scamper_trace_reply_t *reply = state->reply;
  scamper_trace_probe_t *probe = state->probe;
  warts_param_writer_t handlers[] = {
    {NULL,                     NULL,                                  NULL},
    {&probe->ttl,              (wpw_t)insert_byte,                    NULL},
    {&reply->ttl,              (wpw_t)insert_byte,                    NULL},
    {&reply->flags,            (wpw_t)insert_byte,                    NULL},
    {&probe->id,               (wpw_t)warts_trace_hop_write_probe_id, NULL},
    {&reply->rtt,              (wpw_t)insert_rtt,                     NULL},
    {reply,                    (wpw_t)warts_trace_hop_write_icmp_tc,  NULL},
    {&probe->size,             (wpw_t)insert_uint16,                  NULL},
    {&reply->size,             (wpw_t)insert_uint16,                  NULL},
    {&reply->ipid,             (wpw_t)insert_uint16,                  NULL},
    {&reply->tos,              (wpw_t)insert_byte,                    NULL},
    {&reply->reply_icmp_nhmtu, (wpw_t)insert_uint16,                  NULL},
    {&reply->reply_icmp_q_ipl, (wpw_t)insert_uint16,                  NULL},
    {&reply->reply_icmp_q_ttl, (wpw_t)insert_byte,                    NULL},
    {&reply->reply_tcp_flags,  (wpw_t)insert_byte,                    NULL},
    {&reply->reply_icmp_q_tos, (wpw_t)insert_byte,                    NULL},
    {reply,                    (wpw_t)warts_trace_hop_write_icmpext,  NULL},
    {reply->addr,              (wpw_t)insert_addr,                    table},
    {&probe->tx,               (wpw_t)insert_timeval,                 NULL},
    {reply->name,              (wpw_t)insert_string,                  NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);
  warts_params_write(buf, off, len, state->flags, state->flags_len,
		     state->params_len, handlers, handler_cnt);
  return;
}

static slist_t *warts_trace_hops_read(warts_state_t *state,
				      warts_addrtable_t *table,
				      const uint8_t *buf, uint32_t *off,
				      uint32_t len, uint16_t count)
{
  scamper_trace_probe_t *probe;
  slist_t *probes = NULL, *hops = NULL;
  slist_node_t *sn;
  trace_hop_t *hop;
  uint16_t i;
  size_t sz;

  if((probes = slist_alloc()) == NULL || (hops = slist_alloc()) == NULL)
    goto err;

  for(i=0; i<count; i++)
    if(warts_trace_hop_read(probes, hops, state, table, buf, off, len) != 0)
      goto err;

  while((sn = slist_head_node(hops)) != NULL)
    {
      /* identify a batch of replies all involving the same probe */
      hop = slist_node_item(sn);
      probe = hop->probe;
      sz = 1;
      while((sn = slist_node_next(sn)) != NULL)
	{
	  hop = slist_node_item(sn);
	  if(hop->probe != probe)
	    break;
	  sz++;
	}
      if(sz > UINT16_MAX)
	goto err;
      sz *= sizeof(scamper_trace_reply_t *);
      assert(probe->replies == NULL);
      if((probe->replies = malloc_zero(sz)) == NULL)
	goto err;
      while((hop = slist_head_item(hops)) != NULL && hop->probe == probe)
	{
	  slist_head_pop(hops);
	  probe->replies[probe->replyc++] = hop->reply;
	  free(hop);
	}
    }

  slist_free(hops);
  return probes;

 err:
  if(probes != NULL)
    slist_free_cb(probes, (slist_free_t)scamper_trace_probe_free);
  if(hops != NULL)
    slist_free_cb(hops, (slist_free_t)trace_hop_free);
  return NULL;
}

static void warts_trace_pmtud_note_params(const scamper_trace_pmtud_t *pmtud,
					  const scamper_trace_pmtud_note_t *n,
					  warts_trace_pmtud_note_t *state)
{
  scamper_trace_hopiter_t hi;
  const scamper_trace_reply_t *hop;
  const warts_var_t *var;
  uint16_t u16;
  int max_id = 0;
  size_t i;

  memset(state->flags, 0, pmtud_note_vars_mfb);
  state->params_len = 0;

  for(i=0; i<sizeof(pmtud_note_vars)/sizeof(warts_var_t); i++)
    {
      var = &pmtud_note_vars[i];
      if((var->id == WARTS_TRACE_PMTUD_NOTE_TYPE && n->type == 0) ||
	 (var->id == WARTS_TRACE_PMTUD_NOTE_NHMTU && n->nhmtu == 0) ||
	 (var->id == WARTS_TRACE_PMTUD_NOTE_REPLY && n->reply == NULL))
	continue;

      if(var->id == WARTS_TRACE_PMTUD_NOTE_REPLY)
	{
	  scamper_trace_hopiter_reset(&hi);
	  u16 = 0;
	  while((hop = scamper_trace_pmtud_hopiter_next(pmtud, &hi)) != NULL)
	    {
	      if(hop == n->reply)
		break;
	      u16++;
	    }
	  assert(hop == n->reply);
	  state->hop = u16;
	}

      flag_set(state->flags, var->id, &max_id);
      assert(var->size >= 0);
      state->params_len += var->size;
    }

  state->flags_len = fold_flags(state->flags, max_id);
  return;
}

static void warts_trace_pmtud_note_write(const scamper_trace_pmtud_note_t *note,
					 uint8_t *buf, uint32_t *off,
					 uint32_t len,
					 warts_trace_pmtud_note_t *state)
{
  warts_param_writer_t handlers[] = {
    {&note->type,           (wpw_t)insert_byte,   NULL},
    {&note->nhmtu,          (wpw_t)insert_uint16, NULL},
    {&state->hop,           (wpw_t)insert_uint16, NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);
  warts_params_write(buf, off, len, state->flags, state->flags_len,
		     state->params_len, handlers, handler_cnt);
  return;
}

static int warts_trace_pmtud_note_read(const scamper_trace_pmtud_t *pmtud,
				       scamper_trace_pmtud_note_t *note,
				       const uint8_t *buf, uint32_t *off,
				       uint32_t len)
{
  scamper_trace_hopiter_t hi;
  scamper_trace_reply_t *hop;
  uint16_t u16 = 0;
  warts_param_reader_t handlers[] = {
    {&note->type,  (wpr_t)extract_byte,   NULL},
    {&note->nhmtu, (wpr_t)extract_uint16, NULL},
    {&u16,         (wpr_t)extract_uint16, NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);
  uint32_t o = *off;

  if(warts_params_read(buf, off, len, handlers, handler_cnt) != 0)
    return -1;

  if(flag_isset(&buf[o], WARTS_TRACE_PMTUD_NOTE_REPLY))
    {
      scamper_trace_hopiter_reset(&hi);
      while((hop = scamper_trace_pmtud_hopiter_next(pmtud, &hi)) != NULL)
	{
	  if(u16 == 0)
	    break;
	  u16--;
	}
      if(hop == NULL)
	return -1;
      note->probe = scamper_trace_hopiter_probe_get(&hi);
      note->reply = hop;
    }

  return 0;
}

static void warts_trace_pmtud_params(const scamper_trace_t *trace,
				     warts_trace_pmtud_t *state)
{
  const scamper_trace_pmtud_t *pmtud = trace->pmtud;
  const warts_var_t *var;
  int max_id = 0;
  size_t i;

  /* unset all the flags possible */
  memset(state->flags, 0, pmtud_vars_mfb);
  state->params_len = 0;

  for(i=0; i<sizeof(pmtud_vars)/sizeof(warts_var_t); i++)
    {
      var = &pmtud_vars[i];
      if((var->id == WARTS_TRACE_PMTUD_IFMTU && pmtud->ifmtu == 0) ||
	 (var->id == WARTS_TRACE_PMTUD_PMTU && pmtud->pmtu == 0) ||
	 (var->id == WARTS_TRACE_PMTUD_OUTMTU && pmtud->outmtu == 0) ||
	 (var->id == WARTS_TRACE_PMTUD_NOTEC && pmtud->notec == 0))
	continue;

      flag_set(state->flags, var->id, &max_id);
      assert(var->size >= 0);
      state->params_len += var->size;
    }

  state->flags_len = fold_flags(state->flags, max_id);
  return;
}

static int warts_trace_pmtud_state(const scamper_trace_t *trace,
				   warts_trace_pmtud_t *state,
				   warts_addrtable_t *table)
{
  scamper_trace_pmtud_t *pmtud = trace->pmtud;
  scamper_trace_hopiter_t hi;
  warts_trace_pmtud_note_t *note;
  scamper_trace_probe_t *probe;
  scamper_trace_reply_t *hop;
  uint8_t i;
  size_t size;
  int j;

  /* figure out what the structure of the pmtud header looks like */
  warts_trace_pmtud_params(trace, state);

  /* flags + params + number of hop records for pmtud structure */
  state->len = state->flags_len + state->params_len + 2;
  if(state->params_len != 0)
    state->len += 2;

  /* count the number of hop records */
  scamper_trace_hopiter_reset(&hi);
  while(scamper_trace_pmtud_hopiter_next(pmtud, &hi) != NULL)
    state->hopc++;
  if(state->hopc > 0)
    {
      /* allocate an array of address indexes for the pmtud hop addresses */
      size = state->hopc * sizeof(warts_trace_hop_t);
      if((state->hops = (warts_trace_hop_t *)malloc_zero(size)) == NULL)
	return -1;

      /* record hop state for each pmtud hop */
      scamper_trace_hopiter_reset(&hi); j = 0;
      while((hop = scamper_trace_pmtud_hopiter_next(pmtud, &hi)) != NULL)
	{
	  probe = scamper_trace_hopiter_probe_get(&hi);
	  if(warts_trace_hop_state(trace, probe, hop, &state->hops[j++],
				   table, &state->len) != 0)
	    return -1;
	}
    }

  /* record state for each pmtud note */
  if(pmtud->notec > 0)
    {
      size = pmtud->notec * sizeof(warts_trace_pmtud_note_t);
      if((state->notes = (warts_trace_pmtud_note_t *)malloc_zero(size)) == NULL)
	return -1;
      for(i=0; i<pmtud->notec; i++)
	{
	  note = &state->notes[i];
	  warts_trace_pmtud_note_params(pmtud, pmtud->notes[i], note);

	  /* increase length required for the trace record */
	  state->len += note->flags_len + note->params_len;
	  if(note->params_len != 0)
	    state->len += 2;
	}
    }

  return 0;
}

static int warts_trace_pmtud_read(scamper_trace_t *trace, warts_state_t *state,
				  warts_addrtable_t *table, const uint8_t *buf,
				  uint32_t *off, uint32_t len)
{
  uint16_t ifmtu = 0, pmtu = 0, outmtu = 0;
  uint8_t  ver = 1, notec = 0;
  warts_param_reader_t handlers[] = {
    {&ifmtu,  (wpr_t)extract_uint16, NULL},
    {&pmtu,   (wpr_t)extract_uint16, NULL},
    {&outmtu, (wpr_t)extract_uint16, NULL},
    {&ver,    (wpr_t)extract_byte,   NULL},
    {&notec,  (wpr_t)extract_byte,   NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);
  scamper_trace_pmtud_note_t *n = NULL;
  scamper_trace_probe_t *probe;
  slist_t *probes = NULL;
  uint16_t p, count;
  uint8_t u8;
  size_t sz;

  if(trace->pmtud != NULL ||
     (trace->pmtud = scamper_trace_pmtud_alloc()) == NULL)
    goto err;

  if(warts_params_read(buf, off, len, handlers, handler_cnt) != 0)
    goto err;
  trace->pmtud->ifmtu  = ifmtu;
  trace->pmtud->pmtu   = pmtu;
  trace->pmtud->outmtu = outmtu;
  trace->pmtud->ver    = ver;
  trace->pmtud->notec  = notec;

  /* the number of hop records that follow */
  if(extract_uint16(buf, off, len, &count, NULL) != 0)
    goto err;
  if(count != 0)
    {
      probes = warts_trace_hops_read(state, table, buf, off, len, count);
      if(probes == NULL)
	goto err;
      trace->pmtud->probec = slist_count(probes);
      sz = trace->pmtud->probec * sizeof(scamper_trace_probe_t *);
      if((trace->pmtud->probes = malloc_zero(sz)) == NULL)
	goto err;
      p = 0;
      while((probe = slist_head_pop(probes)) != NULL)
	trace->pmtud->probes[p++] = probe;
      slist_free(probes); probes = NULL;
    }

  if(trace->pmtud->notec != 0)
    {
      if(scamper_trace_pmtud_notes_alloc(trace->pmtud,trace->pmtud->notec) != 0)
	goto err;

      for(u8=0; u8<trace->pmtud->notec; u8++)
	{
	  if((n = scamper_trace_pmtud_note_alloc()) == NULL)
	    goto err;
	  if(warts_trace_pmtud_note_read(trace->pmtud, n, buf, off, len) != 0)
	    goto err;
	  trace->pmtud->notes[u8] = n; n = NULL;
	}
    }

  return 0;

 err:
  if(probes != NULL)
    slist_free_cb(probes, (slist_free_t)scamper_trace_probe_free);
  if(n != NULL) scamper_trace_pmtud_note_free(n);
  return -1;
}

static void warts_trace_pmtud_write(const scamper_trace_t *trace,
				    uint8_t *buf, uint32_t *off, uint32_t len,
				    warts_trace_pmtud_t *state,
				    warts_addrtable_t *table)
{
  warts_param_writer_t handlers[] = {
    {&trace->pmtud->ifmtu,  (wpw_t)insert_uint16, NULL},
    {&trace->pmtud->pmtu,   (wpw_t)insert_uint16, NULL},
    {&trace->pmtud->outmtu, (wpw_t)insert_uint16, NULL},
    {&trace->pmtud->ver,    (wpw_t)insert_byte,   NULL},
    {&trace->pmtud->notec,  (wpw_t)insert_byte,   NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);
  uint16_t u16;
  uint8_t u8;

  warts_params_write(buf, off, len, state->flags, state->flags_len,
		     state->params_len, handlers, handler_cnt);

  /* write the number of hop records */
  insert_uint16(buf, off, len, &state->hopc, NULL);

  /* write the hop records */
  for(u16=0; u16<state->hopc; u16++)
    warts_trace_hop_write(&state->hops[u16], table, buf, off, len);

  /* write the notes */
  for(u8=0; u8<trace->pmtud->notec; u8++)
    warts_trace_pmtud_note_write(trace->pmtud->notes[u8], buf, off, len,
				 &state->notes[u8]);

  return;
}

static void warts_trace_pmtud_free(warts_trace_pmtud_t *state)
{
  if(state == NULL)
    return;
  if(state->hops != NULL) free(state->hops);
  if(state->notes != NULL) free(state->notes);
  free(state);
  return;
}

static int warts_trace_lastditch_read(scamper_trace_t *trace,
				      warts_state_t *state,
				      warts_addrtable_t *table,
				      const uint8_t *buf,
				      uint32_t *off, uint32_t len)
{
  scamper_trace_lastditch_t *ld;
  scamper_trace_probe_t *probe;
  slist_t *probes = NULL;
  uint16_t p, count;
  size_t sz;

  if(trace->lastditch != NULL ||
     (trace->lastditch = scamper_trace_lastditch_alloc()) == NULL)
    goto err;

  if(warts_params_read(buf, off, len, NULL, 0) != 0)
    goto err;

  if(extract_uint16(buf, off, len, &count, NULL) != 0)
    goto err;

  if(count != 0)
    {
      probes = warts_trace_hops_read(state, table, buf, off, len, count);
      if(probes == NULL || slist_count(probes) < 1)
	goto err;
      ld = trace->lastditch;
      ld->probec = slist_count(probes);
      sz = ld->probec * sizeof(scamper_trace_probe_t *);
      if((ld->probes = malloc_zero(sz)) == NULL)
	goto err;
      p = 0;
      while((probe = slist_head_pop(probes)) != NULL)
	ld->probes[p++] = probe;
      slist_free(probes); probes = NULL;
    }

  return 0;

 err:
  if(probes != NULL)
    slist_free_cb(probes, (slist_free_t)scamper_trace_probe_free);
  return -1;
}

static int warts_trace_dtree_params(const scamper_file_t *sf,
				    const scamper_trace_t *trace,
				    warts_addrtable_t *table,
				    warts_trace_dtree_t *state)
{
  scamper_trace_dtree_t *dtree = trace->dtree;
  const warts_var_t *var;
  int max_id = 0;
  size_t i;

  /* unset all the flags possible */
  memset(state->flags, 0, trace_dtree_vars_mfb);
  state->params_len = 0;

  for(i=0; i<sizeof(trace_dtree_vars)/sizeof(warts_var_t); i++)
    {
      var = &trace_dtree_vars[i];

      /* not used any more */
      if(var->id == WARTS_TRACE_DTREE_LSS_STOP_GID ||
	 var->id == WARTS_TRACE_DTREE_GSS_STOP_GID)
	continue;

      if((var->id == WARTS_TRACE_DTREE_LSS_STOP && dtree->lss_stop == NULL) ||
	 (var->id == WARTS_TRACE_DTREE_LSS_NAME && dtree->lss == NULL) ||
	 (var->id == WARTS_TRACE_DTREE_GSS_STOP && dtree->gss_stop == NULL) ||
	 (var->id == WARTS_TRACE_DTREE_FLAGS    && dtree->flags == 0))
	continue;

      flag_set(state->flags, var->id, &max_id);

      /* variables that don't have a fixed size */
      if(var->id == WARTS_TRACE_DTREE_LSS_STOP)
	{
	  if(warts_addr_size(table, dtree->lss_stop, &state->params_len) != 0)
	    return -1;
	  continue;
	}
      else if(var->id == WARTS_TRACE_DTREE_LSS_NAME)
	{
	  if(warts_str_size(dtree->lss, &state->params_len) != 0)
	    return -1;
	  continue;
	}
      else if(var->id == WARTS_TRACE_DTREE_GSS_STOP)
	{
	  if(warts_addr_size(table, dtree->gss_stop, &state->params_len) != 0)
	    return -1;
	  continue;
	}

      assert(var->size != -1);
      state->params_len += var->size;
    }

  state->flags_len = fold_flags(state->flags, max_id);

  state->len = state->flags_len + state->params_len;
  if(state->params_len != 0)
    state->len += 2;

  return 0;
}

static void warts_trace_dtree_write(const scamper_trace_t *trace,
				    warts_addrtable_t *table,
				    uint8_t *buf, uint32_t *off, uint32_t len,
				    warts_trace_dtree_t *state)
{
  warts_param_writer_t handlers[] = {
    {NULL,                    NULL,                 NULL},
    {NULL,                    NULL,                 NULL},
    {&trace->dtree->firsthop, (wpw_t)insert_byte,   NULL},
    {trace->dtree->lss_stop,  (wpw_t)insert_addr,   table},
    {trace->dtree->gss_stop,  (wpw_t)insert_addr,   table},
    {trace->dtree->lss,       (wpw_t)insert_string, NULL},
    {&trace->dtree->flags,    (wpw_t)insert_byte,   NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_writer_t);

  warts_params_write(buf, off, len,
		     state->flags, state->flags_len, state->params_len,
		     handlers, handler_cnt);
  return;
}

static int warts_trace_dtree_read(scamper_trace_t *trace, warts_state_t *state,
				  warts_addrtable_t *table, const uint8_t *buf,
				  uint32_t *off, uint32_t len)
{
  scamper_addr_t *lss_stop = NULL, *gss_stop = NULL;
  uint8_t firsthop = 0, flags = 0;
  char *lss = NULL;

  warts_param_reader_t handlers[] = {
    {&lss_stop, (wpr_t)extract_addr_gid, state},
    {&gss_stop, (wpr_t)extract_addr_gid, state},
    {&firsthop, (wpr_t)extract_byte,     NULL},
    {&lss_stop, (wpr_t)extract_addr,     table},
    {&gss_stop, (wpr_t)extract_addr,     table},
    {&lss,      (wpr_t)extract_string,   NULL},
    {&flags,    (wpr_t)extract_byte,     NULL},
  };
  const int handler_cnt = sizeof(handlers)/sizeof(warts_param_reader_t);

  if(trace->dtree != NULL ||
     (trace->dtree = scamper_trace_dtree_alloc()) == NULL ||
     warts_params_read(buf, off, len, handlers, handler_cnt) != 0)
    {
      if(lss_stop != NULL) scamper_addr_free(lss_stop);
      if(gss_stop != NULL) scamper_addr_free(gss_stop);
      if(lss != NULL) free(lss);
      return -1;
    }

  trace->dtree->lss_stop = lss_stop;
  trace->dtree->gss_stop = gss_stop;
  trace->dtree->firsthop = firsthop;
  trace->dtree->lss      = lss;
  trace->dtree->flags    = flags;
  return 0;
}

/*
 * warts_trace_read
 *
 */
int scamper_file_warts_trace_read(scamper_file_t *sf, const warts_hdr_t *hdr,
				  scamper_trace_t **trace_out)
{
  warts_state_t *state = scamper_file_getstate(sf);
  scamper_trace_probettl_t *pttl;
  scamper_trace_probe_t *probe;
  warts_addrtable_t *table = NULL;
  scamper_trace_t *trace = NULL;
  uint8_t *buf = NULL;
  uint32_t i, off = 0;
  uint16_t count, max_ttl, len, u16;
  uint8_t type, ttl;
  slist_t *probes = NULL;
  slist_node_t *sn;
  size_t sz;

  if(warts_read(sf, &buf, hdr->len) != 0)
    {
      goto err;
    }
  if(buf == NULL)
    {
      *trace_out = NULL;
      return 0;
    }

  if((trace = scamper_trace_alloc()) == NULL)
    {
      goto err;
    }

  if((table = warts_addrtable_alloc_byid()) == NULL)
    goto err;

  /* read the trace's parameters */
  if(warts_trace_params_read(trace, state, table, buf, &off, hdr->len) != 0)
    {
      goto err;
    }

  /*
   * the next two bytes tell us how many scamper_hops to read out of trace
   * if we did not get any responses, we are done.
   */
  if(extract_uint16(buf, &off, hdr->len, &count, NULL) != 0)
    {
      goto err;
    }

  /* read all the hop records */
  probes = warts_trace_hops_read(state, table, buf, &off, hdr->len, count);
  if(probes == NULL)
    goto err;

  /* work out the maximum ttl probed that got a response */
  max_ttl = 0;
  for(sn = slist_head_node(probes); sn != NULL; sn = slist_node_next(sn))
    {
      probe = slist_node_item(sn);
      if(probe->ttl > max_ttl)
	max_ttl = probe->ttl;
    }

  /*
   * if the hop_count field was provided in the file, then
   * make sure it makes sense based on the hop data we've just scanned
   */
  if(trace->hop_count != 0)
    {
      if(trace->hop_count < max_ttl)
	goto err;
      if(trace->hop_count > 255)
	goto err;
    }
  else
    {
      trace->hop_count = max_ttl;
    }

  if(trace->hop_count > 0)
    {
      /* allocate enough hops to string the trace together */
      if(scamper_trace_hops_alloc(trace, trace->hop_count) != 0)
	goto err;

      while((sn = slist_head_node(probes)) != NULL)
	{
	  /* identify a batch of probes all involving the same TTL */
	  probe = slist_node_item(sn);
	  ttl = probe->ttl;
	  sz = 1;
	  while((sn = slist_node_next(sn)) != NULL)
	    {
	      probe = slist_node_item(sn);
	      if(probe->ttl < ttl) /* ttls must ascend */
		goto err;
	      if(probe->ttl > ttl) /* got to the end of this TTL batch */
		break;
	      sz++;
	    }

	  /* allocate space for those probes */
	  sz *= sizeof(scamper_trace_probe_t *);
	  if((pttl = scamper_trace_probettl_alloc()) == NULL ||
	     (pttl->probes = malloc_zero(sz)) == NULL)
	    goto err;

	  /* put probes into probettl structure */
	  while((probe = slist_head_item(probes)) != NULL && probe->ttl == ttl)
	    pttl->probes[pttl->probec++] = slist_head_pop(probes);

	  trace->hops[ttl-1] = pttl; pttl = NULL;
	}
    }
  slist_free(probes); probes = NULL;

  for(;;)
    {
      if(extract_uint16(buf, &off, hdr->len, &u16, NULL) != 0)
	goto err;
      if(u16 == WARTS_TRACE_ATTR_EOF)
	break;

      type = WARTS_TRACE_ATTR_HDR_TYPE(u16);
      len  = WARTS_TRACE_ATTR_HDR_LEN(u16);

      if(type == WARTS_TRACE_ATTR_PMTUD)
	{
	  i = off;
	  if(warts_trace_pmtud_read(trace,state,table,buf,&i,hdr->len) != 0)
	    goto err;
	}
      else if(type == WARTS_TRACE_ATTR_LASTDITCH)
	{
	  i = off;
	  if(warts_trace_lastditch_read(trace, state, table,
 					buf, &i, hdr->len) != 0)
	    goto err;
	}
      else if(type == WARTS_TRACE_ATTR_DTREE)
	{
	  i = off;
	  if(warts_trace_dtree_read(trace,state,table,buf,&i,hdr->len) != 0)
	    goto err;
	}

      off += len;
    }

  warts_addrtable_free(table);
  free(buf);
  *trace_out = trace;
  return 0;

 err:
  if(probes != NULL)
    slist_free_cb(probes, (slist_free_t)scamper_trace_probe_free);
  if(table != NULL) warts_addrtable_free(table);
  if(buf != NULL) free(buf);
  if(trace != NULL) scamper_trace_free(trace);
  return -1;
}

int scamper_file_warts_trace_write(const scamper_file_t *sf,
				   const scamper_trace_t *trace, void *p)
{
  scamper_trace_lastditch_t *ld;
  scamper_trace_probe_t *probe;
  scamper_trace_reply_t *hop;
  uint8_t             *buf = NULL;
  uint8_t              trace_flags[trace_vars_mfb];
  uint16_t             trace_flags_len, trace_params_len;
  warts_trace_hop_t   *hop_state = NULL;
  uint16_t             hop_recs = 0;
  warts_trace_pmtud_t *pmtud = NULL;
  warts_trace_hop_t   *ld_state = NULL;
  uint16_t             ld_recs = 0;
  uint32_t             ld_len = 0;
  warts_trace_dtree_t  dtree_state;
  uint16_t             u16;
  uint8_t              u8;
  uint32_t             off = 0, len;
  size_t               size;
  int                  i, j;
  warts_addrtable_t   *table = NULL;
  scamper_trace_hopiter_t hi;

  memset(&dtree_state, 0, sizeof(dtree_state));

  if((table = warts_addrtable_alloc_byaddr()) == NULL)
    goto err;

  /* figure out which trace data items we'll store in this record */
  if(warts_trace_params(trace, table, trace_flags,
			&trace_flags_len, &trace_params_len) != 0)
    goto err;

  /*
   * this represents the length of the trace's flags and parameters, and the
   * 2-byte field that records the number of hop records that follow
   */
  len = 8 + trace_flags_len + trace_params_len + 2;
  if(trace_params_len != 0) len += 2;

  /* count the total number of hop records */
  if(scamper_trace_hopiter_ttl_set(&hi, 1, trace->hop_count) == 0)
    {
      while(scamper_trace_hopiter_next(trace, &hi) != NULL)
	{
	  if(hop_recs == UINT16_MAX)
	    goto err;
	  hop_recs++;
	}
    }

  /* for each hop, figure out what is going to be stored in this record */
  if(hop_recs > 0)
    {
      size = hop_recs * sizeof(warts_trace_hop_t);
      if((hop_state = (warts_trace_hop_t *)malloc_zero(size)) == NULL ||
	 scamper_trace_hopiter_ttl_set(&hi, 1, trace->hop_count) != 0)
	goto err;
      j = 0;
      while((hop = scamper_trace_hopiter_next(trace, &hi)) != NULL)
	{
	  probe = scamper_trace_hopiter_probe_get(&hi);
	  if(warts_trace_hop_state(trace, probe, hop, &hop_state[j++],
				   table, &len) != 0)
	    goto err;
	}
    }

  /* figure out how much space we need for PMTUD data, if we have it */
  if(trace->pmtud != NULL)
    {
      if((pmtud = malloc_zero(sizeof(warts_trace_pmtud_t))) == NULL)
	goto err;

      if(warts_trace_pmtud_state(trace, pmtud, table) != 0)
	goto err;

      len += (2 + pmtud->len); /* 2 = size of attribute header */
    }

  if((ld = trace->lastditch) != NULL)
    {
      /* count the number of last-ditch hop records */
      scamper_trace_hopiter_reset(&hi);
      while(scamper_trace_lastditch_hopiter_next(ld, &hi) != NULL)
	ld_recs++;

      /* allocate an array of hop state structs for the lastditch hops */
      if((ld_state = malloc_zero(ld_recs * sizeof(warts_trace_hop_t))) == NULL)
	goto err;

      /* need to record count of lastditch hops and a single zero flags byte */
      ld_len = 3;

      /* record hop state for each lastditch reply */
      scamper_trace_hopiter_reset(&hi); j = 0;
      while((hop = scamper_trace_lastditch_hopiter_next(ld, &hi)) != NULL)
	{
	  probe = scamper_trace_hopiter_probe_get(&hi);
	  if(warts_trace_hop_state(trace, probe, hop, &ld_state[j++],
				   table, &ld_len) != 0)
	    goto err;
	}

      len += (2 + ld_len); /* 2 = size of attribute header */
    }

  if(trace->dtree != NULL)
    {
      /* figure out what the structure of the dtree header looks like */
      if(warts_trace_dtree_params(sf, trace, table, &dtree_state) != 0)
	goto err;

      /* 2 = size of attribute header */
      len += (2 + dtree_state.len);
    }

  len += 2; /* EOF */

  if((buf = malloc_zero(len)) == NULL)
    {
      goto err;
    }

  insert_wartshdr(buf, &off, len, SCAMPER_FILE_OBJ_TRACE);

  /* write trace parameters */
  if(warts_trace_params_write(trace, sf, table, buf, &off, len, trace_flags,
			      trace_flags_len, trace_params_len) == -1)
    {
      goto err;
    }

  /* hop record count */
  insert_uint16(buf, &off, len, &hop_recs, NULL);

  /* write each traceroute hop record */
  for(i=0; i<hop_recs; i++)
    warts_trace_hop_write(&hop_state[i], table, buf, &off, len);
  if(hop_state != NULL)
    free(hop_state);
  hop_state = NULL;

  /* write the PMTUD data */
  if(pmtud != NULL)
    {
      /* write the attribute header */
      u16 = WARTS_TRACE_ATTR_HDR(WARTS_TRACE_ATTR_PMTUD, pmtud->len);
      insert_uint16(buf, &off, len, &u16, NULL);

      /* write details of the pmtud measurement */
      warts_trace_pmtud_write(trace, buf, &off, len, pmtud, table);

      warts_trace_pmtud_free(pmtud);
      pmtud = NULL;
    }

  /* write the last-ditch data */
  if(trace->lastditch != NULL)
    {
      /* write the attribute header */
      u16 = WARTS_TRACE_ATTR_HDR(WARTS_TRACE_ATTR_LASTDITCH, ld_len);
      insert_uint16(buf, &off, len, &u16, NULL);

      /* write the last-ditch flags: currently zero */
      u8 = 0;
      insert_byte(buf, &off, len, &u8, NULL);

      /* write the number of hop records */
      insert_uint16(buf, &off, len, &ld_recs, NULL);

      for(i=0; i<ld_recs; i++)
	warts_trace_hop_write(&ld_state[i], table, buf, &off, len);

      free(ld_state);
      ld_state = NULL;
    }

  /* write doubletree data */
  if(trace->dtree != NULL)
    {
      u16 = WARTS_TRACE_ATTR_HDR(WARTS_TRACE_ATTR_DTREE, dtree_state.len);
      insert_uint16(buf, &off, len, &u16, NULL);

      /* write details of the pmtud measurement */
      warts_trace_dtree_write(trace, table, buf, &off, len, &dtree_state);
    }

  /* write the end of trace attributes header */
  u16 = WARTS_TRACE_ATTR_EOF;
  insert_uint16(buf, &off, len, &u16, NULL);

  assert(off == len);

  if(warts_write(sf, buf, len, p) == -1)
    {
      goto err;
    }

  warts_addrtable_free(table);
  free(buf);
  return 0;

 err:
  if(table != NULL) warts_addrtable_free(table);
  if(buf != NULL) free(buf);
  if(hop_state != NULL) free(hop_state);
  if(pmtud != NULL) warts_trace_pmtud_free(pmtud);
  if(ld_state != NULL) free(ld_state);
  return -1;
}
