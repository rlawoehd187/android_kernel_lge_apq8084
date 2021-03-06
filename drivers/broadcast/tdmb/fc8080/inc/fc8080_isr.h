/*****************************************************************************
	Copyright(c) 2013 FCI Inc. All Rights Reserved

	File name : fc8080_isr.h

	Description : fc8080 interrupt service routine header file

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA



	History :
	----------------------------------------------------------------------
*******************************************************************************/

#ifndef __FC8080_ISR__
#define __FC8080_ISR__

#ifdef __cplusplus
extern "C" {
#endif

#include "fci_types.h"

extern fci_u32 fic_user_data;
extern fci_u32 msc_user_data;

extern fci_s32 (*fic_callback)(fci_u32 userdata, fci_u8 *data, fci_s32 length);
extern fci_s32 (*msc_callback)(fci_u32 userdata, fci_u8 subch_id, fci_u8 *data,
							fci_s32 length);
extern void fc8080_isr(HANDLE handle);

#ifdef __cplusplus
}
#endif

#endif /* __FC8080_ISR__ */

