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


/* Lab and ICCBased color space writing */
#include "math_.h"
#include "memory_.h"
#include "gx.h"
#include "gxcspace.h"
#include "stream.h"
#include "gsicc.h"
#include "gserrors.h"
#include "gxcie.h"
#include "gdevpdfx.h"
#include "gdevpdfg.h"
#include "gdevpdfc.h"
#include "gdevpdfo.h"
#include "strimpl.h"
#include "gsicc_create.h"
#include "gsicc_manage.h"

/* ------ CIE space synthesis ------ */

/* Add a /Range entry to a CIE-based color space dictionary. */
static int
pdf_cie_add_ranges(gx_device_pdf *pdev, cos_dict_t *pcd, const gs_range *prange, int n, bool clamp)
{
    cos_array_t *pca = cos_array_alloc(pdev, "pdf_cie_add_ranges");
    int code = 0, i;

    if (pca == 0)
        return_error(gs_error_VMerror);
    for (i = 0; i < n; ++i) {
        double rmin = prange[i].rmin, rmax = prange[i].rmax;

        if (clamp) {
            if (rmin < 0) rmin = 0;
            if (rmax > 1) rmax = 1;
        }
        if ((code = cos_array_add_real(pca, rmin)) < 0 ||
            (code = cos_array_add_real(pca, rmax)) < 0
            )
            break;
    }
    if (code >= 0)
        code = cos_dict_put_c_key_object(pcd, "/Range", COS_OBJECT(pca));
    if (code < 0)
        COS_FREE(pca, "pdf_cie_add_ranges");
    return code;
}

/* Transform a CIEBased color to XYZ. */
static int
cie_to_xyz(const double *in, double out[3], const gs_color_space *pcs,
           const gs_gstate *pgs, const gs_cie_common *pciec)
{
    gs_client_color cc;
    frac xyz[3];
    int ncomp = gs_color_space_num_components(pcs);
    int i;

    gs_color_space_index cs_index;
    const gs_vector3 *const pWhitePoint = &pciec->points.WhitePoint;
    float xyz_float[3];

    cs_index = gs_color_space_get_index(pcs);

    for (i = 0; i < ncomp; ++i)
        cc.paint.values[i] = in[i];

    /* The standard concretization makes use of the equivalent ICC profile
       to ensure that all color management is handled by the CMM.
       Unfortunately, we can't do that here since we have no access to the
       icc manager.  Also the PDF write outputs have restrictions on the
       ICC profiles that can be embedded so we must use this older form.
       Need to add an ICC version number into the icc creator to enable
       creation to and from various versions */

    switch (cs_index) {
        case gs_color_space_index_CIEA:
            gx_psconcretize_CIEA(&cc, pcs, xyz, xyz_float, pgs);
            break;
        case gs_color_space_index_CIEABC:
            gx_psconcretize_CIEABC(&cc, pcs, xyz, xyz_float, pgs);
            break;
        case gs_color_space_index_CIEDEF:
            gx_psconcretize_CIEDEF(&cc, pcs, xyz, xyz_float, pgs);
            break;
        case gs_color_space_index_CIEDEFG:
           gx_psconcretize_CIEDEFG(&cc, pcs, xyz, xyz_float, pgs);
           break;
        default:
            /* Only to silence a Coverity uninitialised variable warning */
            memset(&xyz_float, 0x00, sizeof(xyz_float));
            break;
    }
    if (cs_index == gs_color_space_index_CIEA) {
        /* AR forces this case to always be achromatic.  We will
        do the same even though it does not match the PS
        specification */
        /* Use the resulting Y value to scale the wp Illumination.
        note that we scale to the whitepoint here.  Matrix out
        handles mapping to CIE D50.  This forces an achromatic result */
        xyz_float[0] = pWhitePoint->u * xyz_float[1];
        xyz_float[2] = pWhitePoint->w * xyz_float[1];
    } 

    /* Do wp mapping to D50 in XYZ for now.  We should do bradford correction.
       Will add that in next release */
    out[0] = xyz_float[0]*0.9642/pWhitePoint->u;
    out[1] = xyz_float[1];
    out[2] = xyz_float[2]*0.8249/pWhitePoint->w;
    return 0;
}

/* ------ Lab space writing and synthesis ------ */

/* Transform XYZ values to Lab. */
static double
lab_g_inverse(double v)
{
    if (v >= (6.0 * 6.0 * 6.0) / (29 * 29 * 29))
        return pow(v, 1.0 / 3);	/* use cbrt if available? */
    else
        return (v * (841.0 / 108) + 4.0 / 29);
}
static void
xyz_to_lab(const double xyz[3], double lab[3], const gs_cie_common *pciec)
{
    const gs_vector3 *const pWhitePoint = &pciec->points.WhitePoint;
    double L, lunit;

    /* Calculate L* first. */
    L = lab_g_inverse(xyz[1] / pWhitePoint->v) * 116 - 16;
    /* Clamp L* to the PDF range [0..100]. */
    if (L < 0)
        L = 0;
    else if (L > 100)
        L = 100;
    lab[1] = L;
    lunit = (L + 16) / 116;

    /* Calculate a* and b*. */
    lab[0] = (lab_g_inverse(xyz[0] / pWhitePoint->u) - lunit) * 500;
    lab[2] = (lab_g_inverse(xyz[2] / pWhitePoint->w) - lunit) * -200;
}

/* Create a PDF Lab color space corresponding to a CIEBased color space. */
static int
lab_range(gs_range range_out[3] /* only [1] and [2] used */,
          const gs_color_space *pcs, const gs_cie_common *pciec,
          const gs_range *ranges, gs_memory_t *mem)
{
    /*
     * Determine the range of a* and b* by evaluating the color space
     * mapping at all of its extrema.
     */
    int ncomp = gs_color_space_num_components(pcs);
    gs_gstate *pgs;
    int code = gx_cie_to_xyz_alloc(&pgs, pcs, mem);
    int i, j;

    if (code < 0)
        return code;
    for (j = 1; j < 3; ++j)
        range_out[j].rmin = 1000.0, range_out[j].rmax = -1000.0;
    for (i = 0; i < 1 << ncomp; ++i) {
        double in[4], xyz[3];

        for (j = 0; j < ncomp; ++j)
            in[j] = (i & (1 << j) ? ranges[j].rmax : ranges[j].rmin);
        if (cie_to_xyz(in, xyz, pcs, pgs, pciec) >= 0) {
            double lab[3];

            xyz_to_lab(xyz, lab, pciec);
            for (j = 1; j < 3; ++j) {
                range_out[j].rmin = min(range_out[j].rmin, lab[j]);
                range_out[j].rmax = max(range_out[j].rmax, lab[j]);
            }
        }
    }
    gx_cie_to_xyz_free(pgs);
    return 0;
}
/*
 * Create a Lab color space object.
 * This procedure is exported for Lab color spaces in gdevpdfc.c.
 */
int
pdf_put_lab_color_space(gx_device_pdf *pdev, cos_array_t *pca, cos_dict_t *pcd,
                        const gs_range ranges[3] /* only [1] and [2] used */)
{
    int code;
    cos_value_t v;

    if ((code = cos_array_add(pca, cos_c_string_value(&v, "/Lab"))) >= 0)
        code = pdf_cie_add_ranges(pdev, pcd, ranges + 1, 2, false);
    return code;
}

/*
 * Create a Lab color space for a CIEBased space that can't be represented
 * directly as a Calxxx or Lab space.
 */
static int
pdf_convert_cie_to_lab(gx_device_pdf *pdev, cos_array_t *pca,
                       const gs_color_space *pcs,
                       const gs_cie_common *pciec, const gs_range *prange)
{
    cos_dict_t *pcd;
    gs_range ranges[3];
    int code;

    /****** NOT IMPLEMENTED YET, REQUIRES TRANSFORMING VALUES ******/
    if (1) return_error(gs_error_rangecheck);
    pcd = cos_dict_alloc(pdev, "pdf_convert_cie_to_lab(dict)");
    if (pcd == 0)
        return_error(gs_error_VMerror);
    if ((code = lab_range(ranges, pcs, pciec, prange, pdev->pdf_memory)) < 0 ||
        (code = pdf_put_lab_color_space(pdev, pca, pcd, ranges)) < 0 ||
        (code = pdf_finish_cie_space(pdev, pca, pcd, pciec)) < 0
        )
        COS_FREE(pcd, "pdf_convert_cie_to_lab(dict)");
    return code;
}

/* ------ ICCBased space writing and synthesis ------ */

/*
 * Create an ICCBased color space object (internal).  The client must write
 * the profile data on *ppcstrm.
 */
static int
pdf_make_iccbased(gx_device_pdf *pdev, const gs_gstate * pgs,
                  cos_array_t *pca, int ncomps,
                  const gs_range *prange /*[4]*/,
                  const gs_color_space *pcs_alt,
                  cos_stream_t **ppcstrm,
                  const gs_range_t **pprange /* if scaling is needed */)

{
    cos_value_t v;
    int code;
    cos_stream_t * pcstrm = 0;
    cos_array_t * prngca = 0;

    /* Range values are a bit tricky to check.
       For example, CIELAB ICC profiles have
       a unique range.  I am not convinced
       that a check is needed in the new
       color architecture as I am carefull
       to get them properly set during
       creation of the ICC profile data. */

    /* ICCBased color spaces are essentially copied to the output. */
    if ((code = cos_array_add(pca, cos_c_string_value(&v, "/ICCBased"))) < 0)
        return code;

    /* Create a stream for the output. */
    if ((pcstrm = cos_stream_alloc(pdev, "pdf_make_iccbased(stream)")) == 0) {
        code = gs_note_error(gs_error_VMerror);
        goto fail;
    }

    /* Indicate the number of components. */
    code = cos_dict_put_c_key_int(cos_stream_dict(pcstrm), "/N", ncomps);
    if (code < 0)
        goto fail;

    /* In the new design there may not be a specified alternate color space */
    if (pcs_alt != NULL){

        /* Output the alternate color space, if necessary. */
        switch (gs_color_space_get_index(pcs_alt)) {
        case gs_color_space_index_DeviceGray:
        case gs_color_space_index_DeviceRGB:
        case gs_color_space_index_DeviceCMYK:
            break;			/* implicit (default) */
        default:
            if ((code = pdf_color_space_named(pdev, pgs, &v, NULL, pcs_alt,
                                        &pdf_color_space_names, false, NULL, 0, true)) < 0 ||
                (code = cos_dict_put_c_key(cos_stream_dict(pcstrm), "/Alternate",
                                           &v)) < 0
                )
                goto fail;
        }

    } else {
        if (ncomps != 1 && ncomps != 3 && ncomps != 4) {
            /* We can only use a default for Gray, RGB or CMYK. For anything else we need
             * to convert to the base space, we can't legally preserve the ICC profile.
             */
            code = gs_error_rangecheck;
            goto fail;
        }
    }

    /* Wrap up. */
    if ((code = cos_array_add_object(pca, COS_OBJECT(pcstrm))) < 0)
        goto fail;
    *ppcstrm = pcstrm;
    return code;
 fail:
    if (prngca)
        COS_FREE(prngca, "pdf_make_iccbased(Range)");
    if (pcstrm)
        COS_FREE(pcstrm, "pdf_make_iccbased(stream)");
    return code;
}
/*
 * Finish writing the data stream for an ICCBased color space object.
 */
static int
pdf_finish_iccbased(gx_device_pdf *pdev, cos_stream_t *pcstrm)
{
    /*
     * The stream must be an indirect object.  Assign an ID, and write the
     * object out now.
     */
    pcstrm->id = pdf_obj_ref(pdev);
    return cos_write_object(COS_OBJECT(pcstrm), pdev, resourceICC);
}

/*
 * Create an ICCBased color space for a CIEBased space that can't be
 * represented directly as a Calxxx or Lab space.
 */

typedef struct profile_table_s profile_table_t;
struct profile_table_s {
    const char *tag;
    const byte *data;
    uint length;
    uint data_length;		/* may be < length if write != 0 */
    int (*write)(gx_device_pdf *pdev, cos_stream_t *, const profile_table_t *, gs_memory_t *,
                 const gs_cie_common *pciec);
    const void *write_data;
    const gs_range_t *ranges;
};
static profile_table_t *
add_table(profile_table_t **ppnt, const char *tag, const byte *data,
          uint length)
{
    profile_table_t *pnt = (*ppnt)++;

    pnt->tag = tag, pnt->data = data, pnt->length = length;
    pnt->data_length = length;
    pnt->write = NULL;
    /* write_data not set */
    pnt->ranges = NULL;
    return pnt;
}
static void
set_uint32(byte bytes[4], uint value)
{
    bytes[0] = (byte)(value >> 24);
    bytes[1] = (byte)(value >> 16);
    bytes[2] = (byte)(value >> 8);
    bytes[3] = (byte)value;
}
static void
set_XYZ(byte bytes[4], double value)
{
    set_uint32(bytes, (uint)(int)(value * 65536));
}
static void
add_table_xyz3(profile_table_t **ppnt, const char *tag, byte bytes[20],
               const gs_vector3 *pv)
{
    memcpy(bytes, "XYZ \000\000\000\000", 8);
    set_XYZ(bytes + 8, pv->u);
    set_XYZ(bytes + 12, pv->v);
    set_XYZ(bytes + 16, pv->w);
    DISCARD(add_table(ppnt, tag, bytes, 20));
}
static void
set_sample16(byte *p, double v)
{
    int value = (int)(v * 65535);

    if (value < 0)
        value = 0;
    else if (value > 65535)
        value = 65535;
    p[0] = (byte)(value >> 8);
    p[1] = (byte)value;
}
/* Create and write a TRC curve table. */
static int write_trc_abc(gx_device_pdf *pdev, cos_stream_t *, const profile_table_t *, gs_memory_t *, const gs_cie_common *);
static int write_trc_lmn(gx_device_pdf *pdev, cos_stream_t *, const profile_table_t *, gs_memory_t *, const gs_cie_common *);
static profile_table_t *
add_trc(gx_device_pdf *pdev, profile_table_t **ppnt, const char *tag, byte bytes[12],
        const gs_cie_common *pciec, cie_cache_one_step_t one_step)
{
    const int count = gx_cie_cache_size;
    profile_table_t *pnt;

    memcpy(bytes, "curv\000\000\000\000", 8);
    set_uint32(bytes + 8, count);
    pnt = add_table(ppnt, tag, bytes, 12);
    pnt->length += count * 2;
    pnt->write = (one_step == ONE_STEP_ABC ? write_trc_abc : write_trc_lmn);
    pnt->write_data = (const gs_cie_abc *)pciec;
    return pnt;
}
static int
rgb_to_index(const profile_table_t *pnt)
{
    switch (pnt->tag[0]) {
    case 'r': return 0;
    case 'g': return 1;
    case 'b': default: /* others can't happen */ return 2;
    }
}
static double
cache_arg(int i, int denom, const gs_range_t *range)
{
    double arg = i / (double)denom;

    if (range) {
        /* Sample over the range [range->rmin .. range->rmax]. */
        arg = arg * (range->rmax - range->rmin) + range->rmin;
    }
    return arg;
}

static int
write_trc_abc(gx_device_pdf *pdev, cos_stream_t *pcstrm, const profile_table_t *pnt,
              gs_memory_t *ignore_mem, const gs_cie_common *unused)
{
    /* Write the curve table from DecodeABC. */
    const gs_cie_abc *pabc = pnt->write_data;
    int ci = rgb_to_index(pnt);
    gs_cie_abc_proc proc = pabc->DecodeABC.procs[ci];
    byte samples[gx_cie_cache_size * 2];
    byte *p = samples;
    int i;

    for (i = 0; i < gx_cie_cache_size; ++i, p += 2)
        set_sample16(p, proc(cache_arg(i, gx_cie_cache_size - 1, pnt->ranges),
                             pabc));
    return cos_stream_add_bytes(pdev, pcstrm, samples, gx_cie_cache_size * 2);
}
static int
write_trc_lmn(gx_device_pdf *pdev, cos_stream_t *pcstrm, const profile_table_t *pnt,
              gs_memory_t *ignore_mem, const gs_cie_common *unused)
{
    const gs_cie_common *pciec = pnt->write_data;
    int ci = rgb_to_index(pnt);
    gs_cie_common_proc proc = pciec->DecodeLMN.procs[ci];
    byte samples[gx_cie_cache_size * 2];
    byte *p = samples;
    int i;

    /* Write the curve table from DecodeLMN. */
    for (i = 0; i < gx_cie_cache_size; ++i, p += 2)
        set_sample16(p, proc(cache_arg(i, gx_cie_cache_size - 1, pnt->ranges),
                             pciec));
    return cos_stream_add_bytes(pdev, pcstrm, samples, gx_cie_cache_size * 2);
}
/* Create and write an a2b0 lookup table. */
#define NUM_IN_ENTRIES 2	/* assume linear interpolation */
#define NUM_OUT_ENTRIES 2	/* ibid. */
#define MAX_CLUT_ENTRIES 2500	/* enough for 7^4 */
typedef struct icc_a2b0_s {
    byte header[52];
    const gs_color_space *pcs;
    int num_points;		/* on each axis of LUT */
    int count;			/* total # of entries in LUT */
} icc_a2b0_t;
static int write_a2b0(gx_device_pdf *pdev, cos_stream_t *, const profile_table_t *, gs_memory_t *,
                      const gs_cie_common *pciec);
static profile_table_t *
add_a2b0(profile_table_t **ppnt, icc_a2b0_t *pa2b, int ncomps,
         const gs_color_space *pcs)
{
    static const byte a2b0_data[sizeof(pa2b->header)] = {
        'm', 'f', 't', '2',		/* type signature */
        0, 0, 0, 0,			/* reserved, 0 */
        0,				/* # of input channels **VARIABLE** */
        3,				/* # of output channels */
        0,				/* # of CLUT points **VARIABLE** */
        0,				/* reserved, padding */
        0, 1, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /* matrix column 0 */
        0, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0, /* matrix column 1 */
        0, 0, 0, 0,  0, 0, 0, 0,  0, 1, 0, 0, /* matrix column 2 */
        0, NUM_IN_ENTRIES,		/* # of input table entries */
        0, NUM_OUT_ENTRIES		/* # of output table entries */
    };
    int num_points = (int)floor(pow(MAX_CLUT_ENTRIES, 1.0 / ncomps));
    profile_table_t *pnt;

    num_points = min(num_points, 255);
    memcpy(pa2b->header, a2b0_data, sizeof(a2b0_data));
    pa2b->header[8] = ncomps;
    pa2b->header[10] = num_points;
    pa2b->pcs = pcs;
    pa2b->num_points = num_points;
    pa2b->count = (int)pow(num_points, ncomps);
    pnt = add_table(ppnt, "A2B0", pa2b->header,
                    sizeof(pa2b->header) +
                    ncomps * 2 * NUM_IN_ENTRIES + /* in */
                    pa2b->count * (3 * 2) + /* clut: XYZ, 16-bit values */
                    3 * 2 * NUM_OUT_ENTRIES /* out */
                    );
    pnt->data_length = sizeof(pa2b->header); /* only write fixed part */
    pnt->write = write_a2b0;
    pnt->write_data = pa2b;
    return pnt;
}
static int
write_a2b0(gx_device_pdf *pdev, cos_stream_t *pcstrm, const profile_table_t *pnt,
           gs_memory_t *mem, const gs_cie_common *pciec)
{
    const icc_a2b0_t *pa2b = pnt->write_data;
    const gs_color_space *pcs = pa2b->pcs;
    int ncomps = pa2b->header[8];
    int num_points = pa2b->num_points;
    int i;
#define MAX_NCOMPS 4		/* CIEBasedDEFG */
    static const byte v01[MAX_NCOMPS * 2 * 2] = {
        0,0, 255,255,   0,0, 255,255,   0,0, 255,255,   0,0, 255,255
    };
    gs_gstate *pgs;
    int code;

    /* Write the input table. */

    if ((code = cos_stream_add_bytes(pdev, pcstrm, v01, ncomps * 4)) < 0
        )
        return code;

    /* Write the lookup table. */

    code = gx_cie_to_xyz_alloc(&pgs, pcs, mem);
    if (code < 0)
        return code;
    for (i = 0; i < pa2b->count; ++i) {
        double in[MAX_NCOMPS], xyz[3];
        byte entry[3 * 2];
        byte *p = entry;
        int n, j;

        for (n = i, j = ncomps - 1; j >= 0; --j, n /= num_points)
            in[j] = cache_arg(n % num_points, num_points - 1,
                              (pnt->ranges ? pnt->ranges + j : NULL));
        cie_to_xyz(in, xyz, pcs, pgs, pciec);
        /*
         * NOTE: Due to an obscure provision of the ICC Profile
         * specification, values in a2b0 lookup tables do *not* represent
         * the range [0 .. 1], but rather the range [0
         * .. MAX_ICC_XYZ_VALUE].  This caused us a lot of grief before we
         * figured it out!
         */
#define MAX_ICC_XYZ_VALUE (1 + 32767.0/32768)
        for (j = 0; j < 3; ++j, p += 2)
            set_sample16(p, xyz[j] / MAX_ICC_XYZ_VALUE);
#undef MAX_ICC_XYZ_VALUE
        if ((code = cos_stream_add_bytes(pdev, pcstrm, entry, sizeof(entry))) < 0)
            break;
    }
    gx_cie_to_xyz_free(pgs);
    if (code < 0)
        return code;

    /* Write the output table. */

    return cos_stream_add_bytes(pdev, pcstrm, v01, 3 * 4);
}

/* XYZ wp mapping for now.  Will replace later with Bradford or other */
static void
adjust_wp(const gs_vector3 *color_in, const gs_vector3 *wp_in,
          gs_vector3 *color_out, const gs_vector3 *wp_out)
{
    color_out->u = color_in->u * wp_out->u / wp_in->u;
    color_out->v = color_in->v * wp_out->v / wp_in->v;
    color_out->w = color_in->w * wp_out->w / wp_in->w;
}

static int
pdf_convert_cie_to_iccbased(gx_device_pdf *pdev, cos_array_t *pca,
                            const gs_color_space *pcs, const char *dcsname,
                            const gs_cie_common *pciec, const gs_range *prange,
                            cie_cache_one_step_t one_step,
                            const gs_matrix3 *pmat, const gs_range_t **pprange)
{
    /*
     * We have two options for creating an ICCBased color space to represent
     * a CIEBased space.  For CIEBasedABC spaces using only a single
     * Decode step followed by a single Matrix step, we can use [rgb]TRC
     * and [rgb]XYZ; for CIEBasedA spaces using only DecodeA, we could use
     * kTRC (but don't); otherwise, we must use a mft2 LUT.
     */
    int code;
    int ncomps = gs_color_space_num_components(pcs);
    gs_color_space *alt_space;
    cos_stream_t *pcstrm;
    gs_vector3 white_d50;
    gs_vector3 temp_xyz;
    /*
     * because it requires random access to the output stream
     * we construct the ICC profile by hand.
     */
    /* Header */
    byte header[128];
    static const byte header_data[] = {
        0, 0, 0, 0,			/* profile size **VARIABLE** */
        0, 0, 0, 0,			/* CMM type signature */
        0x02, 0x20, 0, 0,		/* profile version number */
        's', 'c', 'n', 'r',		/* profile class signature */
        0, 0, 0, 0,			/* data color space **VARIABLE** */
        'X', 'Y', 'Z', ' ',		/* connection color space */
        2002 / 256, 2002 % 256, 0, 1, 0, 1, /* date (1/1/2002) */
        0, 0, 0, 0, 0, 0,		/* time */
        'a', 'c', 's', 'p',		/* profile file signature */
        0, 0, 0, 0,			/* primary platform signature */
        0, 0, 0, 3,			/* profile flags (embedded use only) */
        0, 0, 0, 0, 0, 0, 0, 0,		/* device manufacturer */
        0, 0, 0, 0,			/* device model */
        0, 0, 0, 0, 0, 0, 0, 2		/* device attributes */
        /* Remaining fields are zero or variable. */
        /* [4] */			/* rendering intent */
        /* 3 * [4] */			/* illuminant */
    };
    /* Description */
#define DESC_LENGTH 5		/* "adhoc" */
    byte desc[12 + DESC_LENGTH + 1 + 11 + 67];
    static const byte desc_data[] = {
        'd', 'e', 's', 'c',		/* type signature */
        0, 0, 0, 0,			/* reserved, 0 */
        0, 0, 0, DESC_LENGTH + 1,	/* ASCII description length */
        'a', 'd', 'h', 'o', 'c', 0,	/* ASCII description */
        /* Remaining fields are zero. */
    };
    /* White point */
    byte wtpt[20];
    /* Copyright (useless, but required by icclib) */
    static const byte cprt_data[] = {
        't', 'e', 'x', 't',	/* type signature */
        0, 0, 0, 0,		/* reserved, 0 */
        'n', 'o', 'n', 'e', 0	/* must be null-terminated (!) */
    };
    /* Lookup table */
    icc_a2b0_t a2b0;
    /* [rgb]TRC */
    byte rTRC[12], gTRC[12], bTRC[12];
    /* [rgb]XYZ */
    byte rXYZ[20], gXYZ[20], bXYZ[20];
    /* Table structures */
#define MAX_NUM_TABLES 9	/* desc, [rgb]TRC, [rgb]xYZ, wtpt, cprt */
    profile_table_t tables[MAX_NUM_TABLES];
    profile_table_t *next_table = tables;

    /* White point must be D50 */
    white_d50.u = 0.9642f;
    white_d50.v = 1.0f;
    white_d50.w = 0.8249f;

    pdf_cspace_init_Device(pdev->memory, &alt_space, ncomps);	/* can't fail */
    code = pdf_make_iccbased(pdev, NULL, pca, ncomps, prange, alt_space,
                             &pcstrm, pprange);
    rc_decrement_cs(alt_space, "pdf_convert_cie_to_iccbased");
    if (code < 0)
        return code;

    /* Fill in most of the header, except for the total size. */

    memset(header, 0, sizeof(header));
    memcpy(header, header_data, sizeof(header_data));
    memcpy(header + 16, dcsname, 4);

    /* Construct the tables. */

    /* desc */
    memset(desc, 0, sizeof(desc));
    memcpy(desc, desc_data, sizeof(desc_data));
    DISCARD(add_table(&next_table, "desc", desc, sizeof(desc)));

    /* wtpt. must be D50 */
    add_table_xyz3(&next_table, "wtpt", wtpt, &white_d50);
    memcpy(header + 68, wtpt + 8, 12); /* illuminant = white point */

    /* cprt */
    /* (We have no use for this tag, but icclib requires it.) */
    DISCARD(add_table(&next_table, "cprt", cprt_data, sizeof(cprt_data)));

    /* Use TRC + XYZ if possible, otherwise AToB. */
    if ((one_step == ONE_STEP_ABC || one_step == ONE_STEP_LMN) && pmat != 0) {
        /* Use TRC + XYZ. */
        profile_table_t *tr =
            add_trc(pdev, &next_table, "rTRC", rTRC, pciec, one_step);
        profile_table_t *tg =
            add_trc(pdev, &next_table, "gTRC", gTRC, pciec, one_step);
        profile_table_t *tb =
            add_trc(pdev, &next_table, "bTRC", bTRC, pciec, one_step);

        if (*pprange) {
            tr->ranges = *pprange;
            tg->ranges = *pprange + 1;
            tb->ranges = *pprange + 2;
        }
        /* These values need to be adjusted to D50.  Again
           use XYZ wp mapping for now.  Later we will add in
           the bradford stuff */
        adjust_wp(&(pmat->cu), &(pciec->points.WhitePoint), &temp_xyz, &white_d50);
        add_table_xyz3(&next_table, "rXYZ", rXYZ, &temp_xyz);
        adjust_wp(&(pmat->cv), &(pciec->points.WhitePoint), &temp_xyz, &white_d50);
        add_table_xyz3(&next_table, "gXYZ", gXYZ, &temp_xyz);
        adjust_wp(&(pmat->cw), &(pciec->points.WhitePoint), &temp_xyz, &white_d50);
        add_table_xyz3(&next_table, "bXYZ", bXYZ, &temp_xyz);
    } else {
        /* General case, use a lookup table. */
        /* AToB (mft2) */
        profile_table_t *pnt = add_a2b0(&next_table, &a2b0, ncomps, pcs);

        pnt->ranges = *pprange;
    }

    /* Write the profile. */
    {
        byte bytes[4 + MAX_NUM_TABLES * 12];
        int num_tables = next_table - tables;
        int i;
        byte *p;
        uint table_size = 4 + num_tables * 12;
        uint offset = sizeof(header) + table_size;

        set_uint32(bytes, next_table - tables);
        for (i = 0, p = bytes + 4; i < num_tables; ++i, p += 12) {
            memcpy(p, tables[i].tag, 4);
            set_uint32(p + 4, offset);
            set_uint32(p + 8, tables[i].length);
            offset += round_up(tables[i].length, 4);
        }
        set_uint32(header, offset);
        if ((code = cos_stream_add_bytes(pdev, pcstrm, header, sizeof(header))) < 0 ||
            (code = cos_stream_add_bytes(pdev, pcstrm, bytes, table_size)) < 0
            )
            return code;
        for (i = 0; i < num_tables; ++i) {
            uint len = tables[i].data_length;
            static const byte pad[3] = {0, 0, 0};

            if ((code = cos_stream_add_bytes(pdev, pcstrm, tables[i].data, len)) < 0 ||
                (tables[i].write != 0 &&
                 (code = tables[i].write(pdev, pcstrm, &tables[i], pdev->pdf_memory, pciec)) < 0) ||
                (code = cos_stream_add_bytes(pdev, pcstrm, pad,
                        -(int)(tables[i].length) & 3)) < 0
                )
                return code;
        }
    }

    return pdf_finish_iccbased(pdev, pcstrm);
}

/* ------ Entry points (from gdevpdfc.c) ------ */

/*
 * Create an ICCBased color space.  This is a single-use procedure,
 * broken out only for readability.
 */
int
pdf_iccbased_color_space(gx_device_pdf *pdev, const gs_gstate * pgs, cos_value_t *pvalue,
                         const gs_color_space *pcs, cos_array_t *pca)
{
    cos_stream_t * pcstrm;
    int code = 0, code1 = 0;
    unsigned char major = 0, minor = 0;
    bool downgrade_icc = false;
    pdf_resource_t *pres = NULL;

    /*
     * This would arise only in a pdf ==> pdf translation, but we
     * should allow for it anyway.
     */
    /* Not all ICC profile types are valid for embedding in a PDF file.
     * The code here duplicates a check in zicc.c, .numicc_components()
     * where we check to see if an embedded profile is valid. Because
     * we could be getting input from other sources, we need to do the same
     * check here. If the profile can't be embedded in PDF, then we
     * return gs_error_rangecheck which will cause pdfwrtie to fall back
     * to the device space. At least the PDF file will be valid and have
     * 'correct' colours.
     */
    switch (pcs->cmm_icc_profile_data->data_cs) {
        case gsCIEXYZ:
        case gsCIELAB:
        case gsRGB:
        case gsGRAY:
        case gsCMYK:
            break;
        case gsUNDEFINED:
        case gsNCHANNEL:
        case gsNAMED:
            emprintf(pdev->memory, "\n An ICC profile which is not suitable for use in PDF has been identified.\n All colours using this profile will be converted into device space\n instead and the profile will not be used.\n");
            return gs_error_rangecheck;
            break;
    }

    code =
        pdf_make_iccbased(pdev, pgs, pca, pcs->cmm_icc_profile_data->num_comps,
                          pcs->cmm_icc_profile_data->Range.ranges,
                          pcs->base_space,
                          &pcstrm, NULL);

    if (code < 0)
        return code;

    /* Transfer the buffer data  */

    (void)gsicc_getprofilevers(pcs->cmm_icc_profile_data, &major, &minor);
    minor = minor >> 4;

    /* Determine whether we need to get the CMS to give us an earlier ICC version
     * of the profile.
     */
    if (pdev->CompatibilityLevel < 1.3) {
        return_error(gs_error_rangecheck);
    } else {
        if (pdev->CompatibilityLevel < 1.5) {
            if (major > 2)
                downgrade_icc = true;
        } else {
            if (pdev->CompatibilityLevel == 1.5) {
                if (major > 4 || minor > 0)
                    downgrade_icc = true;
            } else {
                if (pdev->CompatibilityLevel == 1.6) {
                    if (major > 4 || minor > 1)
                        downgrade_icc = true;
                } else {
                    if (major > 4 || minor > 2)
                        downgrade_icc = true;
                }
            }
        }
    }

    if (downgrade_icc) {
        byte *v2_buffer;
        int size;

        if (pgs == NULL)
            return (gs_error_undefined);
        if (pcs->cmm_icc_profile_data->profile_handle == NULL)
            gsicc_initialize_default_profile(pcs->cmm_icc_profile_data);
        v2_buffer = gsicc_create_getv2buffer(pgs, pcs->cmm_icc_profile_data, &size);
        code = cos_stream_add_bytes(pdev, pcstrm, v2_buffer, size);
    }else{
        code = cos_stream_add_bytes(pdev, pcstrm, pcs->cmm_icc_profile_data->buffer,
        pcs->cmm_icc_profile_data->buffer_size);
    }

    /*
     * The stream has been added to the array: However because the stream cos object
     * has an id (it has to be an indirect object), freeing the colour space won't
     * free the ICC profile stream. In order to have the stream freed we must add it to
     * a resource chain; we don't have a resource chain for ICC profiles, so add it to
     * resourceOther instead. This means it will be among the last objects released.
     */
    code1 = pdf_alloc_resource(pdev, resourceOther, pcstrm->id, &pres, -1);
    if (code1 >= 0) {
        COS_FREE(pres->object, "pdf_iccbased_color_space");
        pres->object = (cos_object_t *)pcstrm;
    }

    if (code >= 0)
        code = pdf_finish_iccbased(pdev, pcstrm);

    return code;
}

/* Convert a CIEBased space to Lab or ICCBased. */
int
pdf_convert_cie_space(gx_device_pdf *pdev, cos_array_t *pca,
                      const gs_color_space *pcs, const char *dcsname,
                      const gs_cie_common *pciec, const gs_range *prange,
                      cie_cache_one_step_t one_step, const gs_matrix3 *pmat,
                      const gs_range_t **pprange)
{
    return (pdev->CompatibilityLevel < 1.3 ?
            /* PDF 1.2 or earlier, use a Lab space. */
            pdf_convert_cie_to_lab(pdev, pca, pcs, pciec, prange) :
            /* PDF 1.3 or later, use an ICCBased space. */
            pdf_convert_cie_to_iccbased(pdev, pca, pcs, dcsname, pciec, prange,
                                        one_step, pmat, pprange)
            );
}
