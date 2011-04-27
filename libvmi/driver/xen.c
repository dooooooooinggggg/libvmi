/* The LibVMI Library is an introspection library that simplifies access to 
 * memory in a target virtual machine or in a file containing a dump of 
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright (C) 2011 Sandia National Laboratories
 * Author: Bryan D. Payne (bpayne@sandia.gov)
 *
 * This file is part of LibVMI.
 *
 * LibVMI is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * LibVMI is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libvmi.h"
#include "private.h"
#include "driver/xen.h"
#include "driver/interface.h"

#if ENABLE_XEN == 1
#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <xs.h>

//----------------------------------------------------------------------------
// Helper functions

static char *xen_get_vmpath (unsigned long domainid)
{
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;
    char *tmp = NULL;
    char *vmpath = NULL;

    /* get the vm path */
    tmp = safe_malloc(100);
    memset(tmp, 0, 100);
    snprintf(tmp, 100, "/local/domain/%d/vm", domainid);
    xsh = xs_domain_open();
    vmpath = xs_read(xsh, xth, tmp, NULL);

error_exit:
    /* cleanup memory here */
    if (tmp) free(tmp);
    if (xsh) xs_daemon_close(xsh);

    return vmpath;
}

// formerly vmi_get_kernel_name
//char *xen_get_kernel_name (unsigned long domainid)
//{
//    struct xs_handle *xsh = NULL;
//    xs_transaction_t xth = XBT_NULL;
//    char *vmpath = NULL;
//    char *kernel = NULL;
//    char *tmp = NULL;
//
//    vmpath = xen_get_vmpath(domainid);
//
//    /* get the kernel name */
//    tmp = safe_malloc(100);
//    memset(tmp, 0, 100);
//    snprintf(tmp, 100, "%s/image/kernel", vmpath);
//    xsh = xs_domain_open();
//    kernel = xs_read(xsh, xth, tmp, NULL);
//
//error_exit:
//    /* cleanup memory here */
//    if (tmp) free(tmp);
//    if (vmpath) free(vmpath);
//    if (xsh) xs_daemon_close(xsh);
//
//    return kernel;
//}

static int xen_ishvm (unsigned long domainid)
{
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;
    char *vmpath = NULL;
    char *ostype = NULL;
    char *tmp = NULL;
    unsigned int len = 0;
    int ret = 0;

    /* setup initial values */
    vmpath = xen_get_vmpath(domainid);
    xsh = xs_domain_open();
    tmp = safe_malloc(100);

    /* check the value for xen 3.2.x and earlier */
    memset(tmp, 0, 100);
    snprintf(tmp, 100, "%s/image/kernel", vmpath);
    ostype = xs_read(xsh, xth, tmp, &len);
    if (NULL == ostype){
        /* no action */
    }
    else if (fnmatch("*hvmloader", ostype, 0) == 0){
        ret = 1;
        goto exit;
    }

    /* try again using different path for 3.3.x */
    if (ostype) free(ostype);
    memset(tmp, 0, 100);
    snprintf(tmp, 100, "%s/image/ostype", vmpath);
    ostype = xs_read(xsh, xth, tmp, &len);

    if (NULL == ostype){
        /* no action */
    }
    else if (fnmatch("*hvm", ostype, 0) == 0){
        ret = 1;
        goto exit;
    }

exit:
    /* cleanup memory here */
    if (tmp) free(tmp);
    if (vmpath) free(vmpath);
    if (ostype) free(ostype);
    if (xsh) xs_daemon_close(xsh);

    return ret;
}


//----------------------------------------------------------------------------
// Xen-Specific Interface Functions (no direct mapping to driver_*)

static xen_instance_t *xen_get_instance (vmi_instance_t vmi)
{
    return ((xen_instance_t *)vmi->driver);
}

static unsigned long xen_get_xchandle (vmi_instance_t vmi)
{
    return xen_get_instance(vmi)->xchandle;
}

//TODO assuming length == page size is safe for now, but isn't the most clean approach
void *xen_get_memory_mfn (vmi_instance_t vmi, addr_t mfn, int prot)
{
    void *memory = xc_map_foreign_range(
        xen_get_xchandle(vmi),
        xen_get_domainid(vmi),
        1,
        prot,
        mfn
    );
    return memory;
}

void *xen_get_memory (vmi_instance_t vmi, uint32_t paddr, uint32_t length)
{
    addr_t pfn = paddr >> vmi->page_shift;
    addr_t mfn = xen_pfn_to_mfn(vmi, pfn);
//TODO assuming length == page size is safe for now, but isn't the most clean approach
    return xen_get_memory_mfn(vmi, mfn, PROT_READ);
}

void xen_release_memory (void *memory, size_t length)
{
    munmap(memory, length);
}

status_t xen_put_memory (vmi_instance_t vmi, addr_t paddr, uint32_t count, void *buf)
{
    unsigned char *memory = NULL;
    addr_t phys_address = 0;
    addr_t pfn = 0;
    addr_t mfn = 0;
    addr_t offset = 0;
    size_t buf_offset = 0;

    while (count > 0){
        size_t write_len = 0;

        /* access the memory */
        phys_address = paddr + buf_offset;
        pfn = phys_address >> vmi->page_shift;
        mfn = xen_pfn_to_mfn(vmi, pfn);
        offset = (vmi->page_size - 1) & phys_address;
        memory = xen_get_memory_mfn(vmi, mfn, PROT_WRITE);
        if (NULL == memory){
            return VMI_FAILURE;
        }

        /* determine how much we can write */
        if ((offset + count) > vmi->page_size){
            write_len = vmi->page_size - offset;
        }
        else{
            write_len = count;
        }

        /* do the write */
        memcpy(memory + offset, ((char *) buf) + buf_offset, write_len);

        /* set variables for next loop */
        count -= write_len;
        buf_offset += write_len;
        xen_release_memory(memory, vmi->page_size);
    }

    return VMI_SUCCESS;
}

//----------------------------------------------------------------------------
// General Interface Functions (1-1 mapping to driver_* function)

// formerly vmi_get_domain_id
unsigned long xen_get_domainid_from_name (vmi_instance_t vmi, char *name)
{
    char **domains = NULL;
    int size = 0;
    int i = 0;
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;
    unsigned long domainid = 0;

    xsh = xs_domain_open();
    domains = xs_directory(xsh, xth, "/local/domain", &size);
    for (i = 0; i < size; ++i){
        /* read in name */
        char *tmp = safe_malloc(100);
        char *idStr = domains[i];
        snprintf(tmp, 100, "/local/domain/%s/name", idStr);
        char *nameCandidate = xs_read(xsh, xth, tmp, NULL);

        // if name matches, then return number
        if (strncmp(name, nameCandidate, 100) == 0){
            int idNum = atoi(idStr);
            domainid = (unsigned long) idNum;
            break;
        }

        /* free memory as we go */
        if (tmp) free(tmp);
        if (nameCandidate) free(nameCandidate);
    }

error_exit:
    if (domains) free(domains);
    if (xsh) xs_daemon_close(xsh);
    return domainid;
}

unsigned long xen_get_domainid (vmi_instance_t vmi)
{
    return xen_get_instance(vmi)->domainid;
}

void xen_set_domainid (vmi_instance_t vmi, unsigned long domainid)
{
    xen_get_instance(vmi)->domainid = domainid;
}

status_t xen_init (vmi_instance_t vmi)
{
    status_t ret = VMI_FAILURE;
    int xchandle;

    /* open handle to the libxc interface */
    if ((xchandle = xc_interface_open()) == -1){
        errprint("Failed to open libxc interface.\n");
        goto error_exit;
    }
    xen_get_instance(vmi)->xchandle = xchandle;

    /* initialize other xen-specific values */
    xen_get_instance(vmi)->live_pfn_to_mfn_table = NULL;
    xen_get_instance(vmi)->nr_pfns = 0;

    /* setup the info struct */
    if (xc_domain_getinfo(xchandle, xen_get_domainid(vmi), 1, &(xen_get_instance(vmi)->info)) != 1){
        errprint("Failed to get domain info for Xen.\n");
        goto error_exit;
    }

    /* determine if target is hvm or pv */
    xen_get_instance(vmi)->hvm = xen_ishvm(xen_get_domainid(vmi));
#ifdef VMI_DEBUG
    if (xen_get_instance(vmi)->hvm){
        dbprint("**set hvm to true (HVM).\n");
    }
    else{
        dbprint("**set hvm to false (PV).\n");
    }
#endif /* VMI_DEBUG */

    memory_cache_init(xen_get_memory, xen_release_memory, 0);
    ret = VMI_SUCCESS;

error_exit:
    return ret;
}

void xen_destroy (vmi_instance_t vmi)
{
    if (xen_get_instance(vmi)->live_pfn_to_mfn_table){
        xen_release_memory(
            xen_get_instance(vmi)->live_pfn_to_mfn_table,
            xen_get_instance(vmi)->nr_pfns * 4
        );
    }

    xen_get_instance(vmi)->domainid = 0;
    xc_interface_close(xen_get_xchandle(vmi));
}

status_t xen_get_domainname (vmi_instance_t vmi, char **name)
{
    status_t ret = VMI_FAILURE;
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;
    char *tmp = safe_malloc(100);

    memset(tmp, 0, 100);
    snprintf(tmp, 100, "/local/domain/%d/name", xen_get_domainid(vmi));
    xsh = xs_domain_open();
    *name = xs_read(xsh, xth, tmp, NULL);
    if (NULL == name){
        errprint("Domain ID %d is not running.\n", xen_get_domainid(vmi));
        goto error_exit;
    }
    ret = VMI_SUCCESS;

error_exit:
    return ret;
}

status_t xen_get_memsize (vmi_instance_t vmi, unsigned long *size)
{
    status_t ret = VMI_FAILURE;
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;

    char *tmp = safe_malloc(100);
    memset(tmp, 0, 100);

    /* get the memory size from the xenstore */
    snprintf(tmp, 100, "/local/domain/%d/memory/target", xen_get_domainid(vmi));
    xsh = xs_domain_open();
    *size = strtol(xs_read(xsh, xth, tmp, NULL), NULL, 10) * 1024;
    if (!size){
        errprint("failed to get memory size for Xen domain.\n");
        goto error_exit;
    }
    ret = VMI_SUCCESS;

error_exit:
    if (xsh) xs_daemon_close(xsh);
    return ret;
}

status_t xen_get_vcpureg (vmi_instance_t vmi, reg_t *value, registers_t reg, unsigned long vcpu)
{
    status_t ret = VMI_SUCCESS;
#ifdef HAVE_CONTEXT_ANY
    vcpu_guest_context_any_t ctxt_any;
#endif /* HAVE_CONTEXT_ANY */
    vcpu_guest_context_t ctxt;

#ifdef HAVE_CONTEXT_ANY
    if ((ret = xc_vcpu_getcontext(
                xen_get_xchandle(vmi),
                xen_get_domainid(vmi),
                vcpu,
                &ctxt_any)) != 0){
#else
    if ((ret = xc_vcpu_getcontext(
                xen_get_xchandle(vmi),
                xen_get_domainid(vmi),
                vcpu,
                &ctxt)) != 0){
#endif /* HAVE_CONTEXT_ANY */
        errprint("Failed to get context information.\n");
        ret = VMI_FAILURE;
        goto error_exit;
    }

#ifdef HAVE_CONTEXT_ANY
    ctxt = ctxt_any.c;
#endif /* HAVE_CONTEXT_ANY */

    switch (reg){
        case CR0:
            *value = ctxt.ctrlreg[0];
            break;
        case CR1:
            *value = ctxt.ctrlreg[1];
            break;
        case CR2:
            *value = ctxt.ctrlreg[2];
            break;
        case CR3:
            *value = ctxt.ctrlreg[3];
            break;
        case CR4:
            *value = ctxt.ctrlreg[4];
            break;
        case EAX:
            *value = ctxt.user_regs.eax;
            break;
        case EBX:
            *value = ctxt.user_regs.ebx;
            break;
        case ECX:
            *value = ctxt.user_regs.ecx;
            break;
        case EDX:
            *value = ctxt.user_regs.edx;
            break;
        case ESI:
            *value = ctxt.user_regs.esi;
            break;
        case EDI:
            *value = ctxt.user_regs.edi;
            break;
        case EBP:
            *value = ctxt.user_regs.ebp;
            break;
        case ESP:
            *value = ctxt.user_regs.esp;
            break;
        case EIP:
            *value = ctxt.user_regs.eip;
            break;
        case EFL:
            *value = ctxt.user_regs.eflags;
            break;
        default:
            ret = VMI_FAILURE;
            break;
    }

error_exit:
    return ret;
}

unsigned long xen_pfn_to_mfn (vmi_instance_t vmi, unsigned long pfn)
{
    shared_info_t *live_shinfo = NULL;
    unsigned long *live_pfn_to_mfn_frame_list_list = NULL;
    unsigned long *live_pfn_to_mfn_frame_list = NULL;

    /* Live mapping of the table mapping each PFN to its current MFN. */
    unsigned long *live_pfn_to_mfn_table = NULL;
    unsigned long nr_pfns = 0;
    unsigned long ret = 0;

    if (xen_get_instance(vmi)->hvm){
        return pfn;
    }

    if (NULL == xen_get_instance(vmi)->live_pfn_to_mfn_table){
        live_shinfo = xen_get_memory_mfn(vmi, xen_get_instance(vmi)->info.shared_info_frame, PROT_READ);
        if (live_shinfo == NULL){
            errprint("Failed to init live_shinfo.\n");
            goto error_exit;
        }
        nr_pfns = live_shinfo->arch.max_pfn;

        live_pfn_to_mfn_frame_list_list = xen_get_memory_mfn(vmi, live_shinfo->arch.pfn_to_mfn_frame_list_list, PROT_READ);
        if (live_pfn_to_mfn_frame_list_list == NULL){
            errprint("Failed to init live_pfn_to_mfn_frame_list_list.\n");
            goto error_exit;
        }

        live_pfn_to_mfn_frame_list = xc_map_foreign_batch(
            xen_get_xchandle(vmi),
            xen_get_domainid(vmi),
            PROT_READ,
            live_pfn_to_mfn_frame_list_list,
            (nr_pfns+(fpp*fpp)-1)/(fpp*fpp) );
        if (live_pfn_to_mfn_frame_list == NULL){
            errprint("Failed to init live_pfn_to_mfn_frame_list.\n");
            goto error_exit;
        }
        live_pfn_to_mfn_table = xc_map_foreign_batch(
            xen_get_xchandle(vmi),
            xen_get_domainid(vmi),
            PROT_READ,
            live_pfn_to_mfn_frame_list, (nr_pfns+fpp-1)/fpp );
        if (live_pfn_to_mfn_table  == NULL){
            errprint("Failed to init live_pfn_to_mfn_table.\n");
            goto error_exit;
        }

        /* save mappings for later use */
        xen_get_instance(vmi)->live_pfn_to_mfn_table = live_pfn_to_mfn_table;
        xen_get_instance(vmi)->nr_pfns = nr_pfns;
    }

    ret = xen_get_instance(vmi)->live_pfn_to_mfn_table[pfn];

error_exit:
    if (live_shinfo) xen_release_memory(live_shinfo, XC_PAGE_SIZE);
    if (live_pfn_to_mfn_frame_list_list)
        xen_release_memory(live_pfn_to_mfn_frame_list_list, XC_PAGE_SIZE);
    if (live_pfn_to_mfn_frame_list)
        xen_release_memory(live_pfn_to_mfn_frame_list, XC_PAGE_SIZE);

    return ret;
}

void *xen_map_page (vmi_instance_t vmi, int prot, unsigned long page)
{
    uint32_t paddr = page << vmi->page_shift;
    uint32_t offset = 0;
    return memory_cache_insert(vmi, paddr, &offset);
}

status_t xen_write (vmi_instance_t vmi, addr_t paddr, void *buf, uint32_t length)
{
    return xen_put_memory(vmi, paddr, length, buf);
}

int xen_is_pv (vmi_instance_t vmi)
{
    return !xen_get_instance(vmi)->hvm;
}

status_t xen_test (unsigned long id, char *name)
{
    status_t ret = VMI_FAILURE;
    struct xs_handle *xsh = NULL;
    xs_transaction_t xth = XBT_NULL;
    char *tmp = NULL;

    xsh = xs_domain_open();
    if (NULL == xsh){
        goto error_exit;
    }
    tmp = xs_read(xsh, xth, "/local/domain/0/name", NULL);
    if (NULL == tmp){
        goto error_exit;
    }
    free(tmp);
    ret = VMI_SUCCESS;

error_exit:
    return ret;
}

status_t xen_pause_vm (vmi_instance_t vmi)
{
    if (-1 == xc_domain_pause(xen_get_xchandle(vmi), xen_get_domainid(vmi))){
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

status_t xen_resume_vm (vmi_instance_t vmi)
{
    if (-1 == xc_domain_unpause(xen_get_xchandle(vmi), xen_get_domainid(vmi))){
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////////
#else

status_t xen_init (vmi_instance_t vmi) { return VMI_FAILURE; }
void xen_destroy (vmi_instance_t vmi) { return; }
unsigned long xen_get_domainid_from_name (vmi_instance_t vmi, char *name) { return 0; }
unsigned long xen_get_domainid (vmi_instance_t vmi) { return 0; }
void xen_set_domainid (vmi_instance_t vmi, unsigned long domainid) { return; }
status_t xen_get_domainname (vmi_instance_t vmi, char **name) { return VMI_FAILURE; }
status_t xen_get_memsize (vmi_instance_t vmi, unsigned long *size) { return VMI_FAILURE; }
status_t xen_get_vcpureg (vmi_instance_t vmi, reg_t *value, registers_t reg, unsigned long vcpu) { return VMI_FAILURE; }
unsigned long xen_pfn_to_mfn (vmi_instance_t vmi, unsigned long pfn) { return 0; }
void *xen_map_page (vmi_instance_t vmi, int prot, unsigned long page) { return NULL; }
status_t xen_write (vmi_instance_t vmi, addr_t paddr, void *buf, uint32_t length) { return VMI_FAILURE; }
int xen_is_pv (vmi_instance_t vmi) { return 0; }
status_t xen_test (unsigned long id, char *name) { return VMI_FAILURE; }
status_t xen_pause_vm (vmi_instance_t vmi) { return VMI_FAILURE; }
status_t xen_resume_vm (vmi_instance_t vmi) { return VMI_FAILURE; }

#endif /* ENABLE_XEN */
