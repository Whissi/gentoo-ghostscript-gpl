/* Copyright (C) 2001-2020 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/


/* Definitions for precompiled halftone resources */

#ifndef gxdhtres_INCLUDED
#  define gxdhtres_INCLUDED

#include "stdpre.h"

/*
 * Precompiled halftones generated by genht #include this file.
 */
typedef struct gx_device_halftone_resource_s gx_device_halftone_resource_t;

struct gx_device_halftone_resource_s {
    const char *rname;
    int HalftoneType;
    int Width;
    int Height;
    int num_levels;
    const unsigned int *levels;
    const void *bit_data;
    int elt_size;
};

#define DEVICE_HALFTONE_RESOURCE_PROC(proc)\
  const gx_device_halftone_resource_t *const *proc(void)

#endif /* gxdhtres_INCLUDED */
