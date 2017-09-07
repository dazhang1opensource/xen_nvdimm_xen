/*
 * tools/libxl/libxl_vnvdimm.c
 *
 * Copyright (C) 2017,  Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License, version 2.1, as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xenctrl.h>

#include "libxl_internal.h"
#include "libxl_vnvdimm.h"

int libxl_vnvdimm_copy_config(libxl_ctx *ctx,
                              libxl_domain_config *dst,
                              const libxl_domain_config *src)
{
    GC_INIT(ctx);
    unsigned int nr = src->num_vnvdimms;
    libxl_device_vnvdimm *vnvdimms;
    int rc = 0;

    if (!nr)
        goto out;

    vnvdimms = libxl__calloc(NOGC, nr, sizeof(*vnvdimms));
    if (!vnvdimms) {
        rc = ERROR_NOMEM;
        goto out;
    }

    dst->num_vnvdimms = nr;
    while (nr--)
        libxl_device_vnvdimm_copy(ctx, &vnvdimms[nr], &src->vnvdimms[nr]);
    dst->vnvdimms = vnvdimms;

 out:
    GC_FREE;
    return rc;
}

#if defined(__linux__)

int libxl_vnvdimm_add_pages(libxl__gc *gc, uint32_t domid,
                            xen_pfn_t mfn, xen_pfn_t gpfn, xen_pfn_t nr_pages)
{
    unsigned int nr;
    int ret;

    while (nr_pages) {
        nr = min(nr_pages, (unsigned long)UINT_MAX);

        ret = xc_domain_populate_pmem_map(CTX->xch, domid, mfn, gpfn, nr);
        if (ret && ret != -ERESTART) {
            LOG(ERROR, "failed to map PMEM pages, mfn 0x%" PRI_xen_pfn ", "
                "gpfn 0x%" PRI_xen_pfn ", nr_pages %u, err %d",
                mfn, gpfn, nr, ret);
            break;
        }

        nr_pages -= nr;
        mfn += nr;
        gpfn += nr;
    }

    return ret;
}

#endif /* __linux__ */
