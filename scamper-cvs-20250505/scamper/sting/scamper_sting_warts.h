/*
 * scamper_sting_warts.h
 *
 * $Id: scamper_sting_warts.h,v 1.3 2024/09/23 10:34:27 mjl Exp $
 *
 * Copyright (C) 2010 University of Waikato
 * Copyright (C) 2022 Matthew Luckie
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

#ifndef __SCAMPER_STING_WARTS_H
#define __SCAMPER_STING_WARTS_H

int scamper_file_warts_sting_write(const scamper_file_t *sf,
				   const scamper_sting_t *sting, void *p);

int scamper_file_warts_sting_read(scamper_file_t *sf, const warts_hdr_t *hdr,
				  scamper_sting_t **sting_out);

#endif
