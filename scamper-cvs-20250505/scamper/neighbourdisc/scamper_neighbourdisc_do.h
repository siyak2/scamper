/*
 * scamper_do_neighbourdisc.h
 *
 * $Id: scamper_neighbourdisc_do.h,v 1.9 2025/04/27 00:49:24 mjl Exp $
 *
 * Copyright (C) 2009-2010 Matthew Luckie
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

#ifndef __SCAMPER_DO_NEIGHBOURDISC_H
#define __SCAMPER_DO_NEIGHBOURDISC_H

typedef struct scamper_neighbourdisc_do scamper_neighbourdisc_do_t;

scamper_task_t *scamper_do_neighbourdisc_alloctask(void *data,
						   scamper_list_t *list,
						   scamper_cycle_t *cycle,
						   char *errbuf, size_t errlen);

void scamper_do_neighbourdisc_free(void *);

/* code to use the neighbourdisc code to do IP->MAC for another scamper task */
scamper_neighbourdisc_do_t *scamper_do_neighbourdisc_do(
  int ifindex, scamper_addr_t *dst,
  void *param, void (*cb)(void *, scamper_addr_t *ip, scamper_addr_t *mac));
void scamper_neighbourdisc_do_free(scamper_neighbourdisc_do_t *nddo);

uint32_t scamper_do_neighbourdisc_userid(void *data);

int scamper_do_neighbourdisc_enabled(void);

void scamper_do_neighbourdisc_cleanup(void);
int scamper_do_neighbourdisc_init(void);

#endif /* __SCAMPER_DO_NEIGHBOURDISC_H */
