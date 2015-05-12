/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <xio_os.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include "libxio.h"
#include "xio_log.h"
#include "xio_common.h"
#include "xio_observer.h"
#include "xio_transport.h"
#include "xio_protocol.h"
#include "get_clock.h"
#include "xio_mem.h"
#include "xio_usr_transport.h"
#include "xio_mempool.h"
#include "xio_protocol.h"
#include "xio_mbuf.h"
#include "xio_task.h"
#include "xio_rdma_utils.h"
#include "xio_ev_data.h"
#include "xio_workqueue.h"
#include "xio_context.h"
#include "xio_rdma_transport.h"


/*---------------------------------------------------------------------------*/
/* globals								     */
/*---------------------------------------------------------------------------*/
static LIST_HEAD(mr_list);
static spinlock_t mr_list_lock;
static uint32_t mr_num; /* checkpatch doesn't like initializing static vars */


/*---------------------------------------------------------------------------*/
/* ibv_wc_opcode_str	                                                     */
/*---------------------------------------------------------------------------*/
const char *ibv_wc_opcode_str(enum ibv_wc_opcode opcode)
{
	switch (opcode) {
	case IBV_WC_SEND:		return "IBV_WC_SEND";
	case IBV_WC_RDMA_WRITE:		return "IBV_WC_RDMA_WRITE";
	case IBV_WC_RDMA_READ:		return "IBV_WC_RDMA_READ";
	case IBV_WC_COMP_SWAP:		return "IBV_WC_COMP_SWAP";
	case IBV_WC_FETCH_ADD:		return "IBV_WC_FETCH_ADD";
	case IBV_WC_BIND_MW:		return "IBV_WC_BIND_MW";
	/* recv-side: inbound completion */
	case IBV_WC_RECV:		return "IBV_WC_RECV";
	case IBV_WC_RECV_RDMA_WITH_IMM: return "IBV_WC_RECV_RDMA_WITH_IMM";
	default:			return "IBV_WC_UNKNOWN";
	};
}

/*---------------------------------------------------------------------------*/
/* xio_reg_mr_ex_dev							     */
/*---------------------------------------------------------------------------*/
static struct xio_mr_elem *xio_reg_mr_ex_dev(struct xio_device *dev,
					     void **addr, size_t length,
					     uint64_t access)
{
	struct xio_mr_elem *mr_elem;
	struct ibv_mr	   *mr;
	int retval;
	struct ibv_exp_reg_mr_in reg_mr_in;
	int alloc_mr = (*addr == NULL);

	reg_mr_in.pd = dev->pd;
	reg_mr_in.addr = *addr;
	reg_mr_in.length = length;
	reg_mr_in.exp_access = access;
	reg_mr_in.comp_mask = 0;
	mr = ibv_xio_reg_mr(&reg_mr_in);
	if (!mr) {
		xio_set_error(errno);
		if (!alloc_mr)
			ERROR_LOG("ibv_reg_mr failed, %m\n");
		if (errno == ENOMEM)
			xio_validate_ulimit_memlock();
		return NULL;
	}
	mr_elem = (struct xio_mr_elem *)ucalloc(1, sizeof(*mr_elem));
	if (mr_elem == NULL)
		goto  cleanup;

	mr_elem->dev = dev;
	mr_elem->mr = mr;

	return mr_elem;

cleanup:
	retval = ibv_dereg_mr(mr);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("ibv_dereg_mr failed, %m\n");
	}

	return NULL;
}

#define MAX_DEVS 32
/*---------------------------------------------------------------------------*/
/* xio_reg_mr_ex							     */
/*---------------------------------------------------------------------------*/
static struct xio_mr *xio_reg_mr_ex(void **addr, size_t length, uint64_t access)
{
	struct xio_mr			*tmr;
	struct xio_mr_elem		*tmr_elem;
	struct xio_device		*dev;
	int				retval;
	static int			init_transport = 1;
	struct xio_device		*devs_arr[MAX_DEVS];
	int				devs_nr = 0, i;


	/* this may the first call in application so initialize the rdma */
	if (init_transport) {
		struct xio_transport *transport = xio_get_transport("rdma");
		if (transport == NULL) {
			ERROR_LOG("invalid protocol. proto: rdma\n");
			xio_set_error(XIO_E_ADDR_ERROR);
			return NULL;
		}
		init_transport = 0;
	}

	spin_lock(&dev_list_lock);
	if (list_empty(&dev_list)) {
		ERROR_LOG("dev_list is empty\n");
		spin_unlock(&dev_list_lock);
		goto cleanup2;
	}
	list_for_each_entry(dev, &dev_list, dev_list_entry) {
		if (devs_nr == MAX_DEVS)
			break;
		xio_device_get(dev);
		devs_arr[devs_nr] = dev;
		devs_nr++;
	}
	spin_unlock(&dev_list_lock);

	tmr = (struct xio_mr *)ucalloc(1, sizeof(*tmr));
	if (tmr == NULL) {
		xio_set_error(errno);
		ERROR_LOG("malloc failed. (errno=%d %m)\n", errno);
		goto cleanup2;
	}
	INIT_LIST_HEAD(&tmr->dm_list);
	/* xio_dereg_mr may be called on error path and it will call
	 * list_del on mr_list_entry, make sure it is initialized
	 */
	INIT_LIST_HEAD(&tmr->mr_list_entry);

	for (i = 0; i < devs_nr; i++) {
		dev = devs_arr[i];
		tmr_elem = xio_reg_mr_ex_dev(dev, addr, length, access);
		if (tmr_elem == NULL) {
			xio_device_put(dev);
			xio_set_error(errno);
			goto cleanup1;
		}
		list_add(&tmr_elem->dm_list_entry, &tmr->dm_list);
		list_add(&tmr_elem->xm_list_entry, &dev->xm_list);

		if (access & IBV_XIO_ACCESS_ALLOCATE_MR) {
			access  &= ~IBV_XIO_ACCESS_ALLOCATE_MR;
			*addr = tmr_elem->mr->addr;
		}
		xio_device_put(dev);
	}


	/* For dynamically discovered devices */
	tmr->addr   = *addr;
	tmr->length = length;
	tmr->access = access;

	spin_lock(&mr_list_lock);
	mr_num++;
	list_add(&tmr->mr_list_entry, &mr_list);
	spin_unlock(&mr_list_lock);

	return tmr;

cleanup1:
	retval = xio_dereg_mr(&tmr);
	if (retval != 0)
		ERROR_LOG("xio_dereg_mr failed\n");
cleanup2:
	return  NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_reg_mr								     */
/*---------------------------------------------------------------------------*/
struct xio_mr *xio_reg_mr(void *addr, size_t length)
{
	if (addr == NULL) {
		xio_set_error(EINVAL);
		return NULL;
	}

	return xio_reg_mr_ex(&addr, length,
			     IBV_ACCESS_LOCAL_WRITE |
			     IBV_ACCESS_REMOTE_WRITE|
			     IBV_ACCESS_REMOTE_READ);
}

/*---------------------------------------------------------------------------*/
/* xio_reg_mr_add_dev							     */
/* add a new discovered device to a the mr list				     */
/*---------------------------------------------------------------------------*/
int xio_reg_mr_add_dev(struct xio_device *dev)
{
	struct xio_mr *tmr;
	struct xio_mr_elem *tmr_elem;

	spin_lock(&dev_list_lock);
	spin_lock(&mr_list_lock);
	list_for_each_entry(tmr, &mr_list, mr_list_entry) {
		tmr_elem = xio_reg_mr_ex_dev(dev,
					     &tmr->addr, tmr->length,
					     tmr->access);
		if (tmr_elem == NULL) {
			xio_set_error(errno);
			ERROR_LOG("ibv_reg_mr failed, %m\n");
			spin_unlock(&mr_list_lock);
			spin_unlock(&dev_list_lock);
			goto cleanup;
		}
		list_add(&tmr_elem->dm_list_entry, &tmr->dm_list);
		list_add(&tmr_elem->xm_list_entry, &dev->xm_list);
	}
	spin_unlock(&mr_list_lock);
	spin_unlock(&dev_list_lock);


	return 0;

cleanup:
	xio_dereg_mr_by_dev(dev);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_dereg_mr								     */
/*---------------------------------------------------------------------------*/
int xio_dereg_mr(struct xio_mr **p_tmr)
{
	struct xio_mr		*tmr = *p_tmr;
	struct xio_mr		*ptmr, *tmp_ptmr;
	struct xio_mr_elem	*tmr_elem, *tmp_tmr_elem;
	int			retval, found = 0;

	spin_lock(&mr_list_lock);
	list_for_each_entry_safe(ptmr, tmp_ptmr, &mr_list, mr_list_entry) {
		if (ptmr == tmr) {
			list_del(&tmr->mr_list_entry);
			found = 1;
			break;
		}
	}
	spin_unlock(&mr_list_lock);

	if (found) {
		list_for_each_entry_safe(tmr_elem, tmp_tmr_elem, &tmr->dm_list,
					 dm_list_entry) {
			retval = ibv_dereg_mr(tmr_elem->mr);
			if (retval != 0) {
				xio_set_error(errno);
				ERROR_LOG("ibv_dereg_mr failed, %m\n");
			}
			/* Remove the item from the list. */
			spin_lock(&dev_list_lock);
			list_del(&tmr_elem->dm_list_entry);
			list_del(&tmr_elem->xm_list_entry);
			spin_unlock(&dev_list_lock);
			ufree(tmr_elem);
		}
		ufree(tmr);
		*p_tmr = NULL;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_dereg_mr_by_dev							     */
/*---------------------------------------------------------------------------*/
int xio_dereg_mr_by_dev(struct xio_device *dev)
{
	struct xio_mr_elem	*tmr_elem, *tmp_tmr_elem;
	int			retval;

	spin_lock(&dev_list_lock);
	if (list_empty(&dev->xm_list)) {
		spin_unlock(&dev_list_lock);
		return 0;
	}

	list_for_each_entry_safe(tmr_elem, tmp_tmr_elem, &dev->xm_list,
				 xm_list_entry) {
		if (tmr_elem->mr) {
			retval = ibv_dereg_mr(tmr_elem->mr);
			if (retval != 0) {
				xio_set_error(errno);
				ERROR_LOG("ibv_dereg_mr failed, %m\n");
			}
		}
		/* Remove the item from the lists. */
		list_del(&tmr_elem->dm_list_entry);
		list_del(&tmr_elem->xm_list_entry);
		ufree(tmr_elem);
	}
	spin_unlock(&dev_list_lock);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_alloc								     */
/*---------------------------------------------------------------------------*/
struct xio_buf *xio_alloc(size_t length)
{
	struct xio_buf		*buf;
	struct xio_device	*dev;
	size_t			real_size;
	uint64_t		access;

	access = IBV_ACCESS_LOCAL_WRITE |
		 IBV_ACCESS_REMOTE_WRITE|
		 IBV_ACCESS_REMOTE_READ;

	buf = (struct xio_buf *)ucalloc(1, sizeof(*buf));
	if (!buf) {
		xio_set_error(errno);
		ERROR_LOG("calloc failed. (errno=%d %m)\n", errno);
		return NULL;
	}

	dev = list_first_entry(&dev_list, struct xio_device, dev_list_entry);

	if (dev && IBV_IS_MPAGES_AVAIL(&dev->device_attr)) {
		access |= IBV_XIO_ACCESS_ALLOCATE_MR;
		buf->mr = xio_reg_mr_ex(&buf->addr, length, access);
		if (buf->mr) {
			buf->length		= length;
			buf->mr->addr_alloced	= 0;
			goto exit;
		}
		WARN_LOG("Contig pages allocation failed. (errno=%d %m)\n",
			 errno);
	}

	real_size = ALIGN(length, page_size);
	buf->addr = umemalign(page_size, real_size);
	if (!buf->addr) {
		ERROR_LOG("memalign failed. sz:%zu\n", real_size);
		goto cleanup;
	}
	buf->mr = xio_reg_mr_ex(&buf->addr, length, access);
	if (!buf->mr) {
		ERROR_LOG("xio_reg_mr_ex failed. " \
			  "addr:%p, length:%d, access:0x%x\n",
			  buf->addr, length, access);

		goto cleanup1;
	}
	memset(buf->addr, 0, length);
	buf->length		= length;
	buf->mr->addr_alloced	= 1;

exit:
	return buf;

cleanup1:
	ufree(buf->addr);
cleanup:
	ufree(buf);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_free								     */
/*---------------------------------------------------------------------------*/
int xio_free(struct xio_buf **buf)
{
	struct xio_mr		*tmr = (*buf)->mr;
	int			retval = 0;

	if (tmr->addr_alloced)
		ufree((*buf)->addr);

	retval = xio_dereg_mr(&tmr);

	ufree(*buf);
	*buf = NULL;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_mr_list_init							     */
/*---------------------------------------------------------------------------*/
void xio_mr_list_init(void)
{
	INIT_LIST_HEAD(&mr_list);
	spin_lock_init(&mr_list_lock);
}

/*---------------------------------------------------------------------------*/
/* xio_mr_list_free							     */
/*---------------------------------------------------------------------------*/
int xio_mr_list_free(void)
{
	struct xio_mr		*tmr;

	while (!list_empty(&mr_list)) {
		tmr = list_first_entry(&mr_list, struct xio_mr, mr_list_entry);
		xio_dereg_mr(&tmr);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rkey_table_create						     */
/*---------------------------------------------------------------------------*/
int xio_rkey_table_create(struct xio_device *old, struct xio_device *_new,
			  struct xio_rkey_tbl **htbl, uint16_t *len)
{
	struct xio_rkey_tbl *tbl, *te;
	struct list_head *old_h, *new_h;
	struct list_head *old_n, *new_n;
	struct xio_mr_elem *old_e, *new_e;

	if (!mr_num) {
		/* This is O.K. memory wasn't yet allocated and registered */
		*len = 0;
		return 0;
	}

	tbl = (struct xio_rkey_tbl *)ucalloc(mr_num, sizeof(*tbl));
	if (!tbl) {
		*len = 0;
		return -ENOMEM;
	}

	/* MR elements are arranged in a matrix like fashion, were MR is one
	 * axis and device is the other axis
	 */
	old_h = &old->xm_list;
	new_h = &_new->xm_list;
	te = tbl;

	for (old_n = old_h->next, new_n = new_h->next;
	     old_n != old_h && new_n != new_h;
	     old_n = old_n->next, new_n = new_h->next) {
		old_e = list_entry(old_n, struct xio_mr_elem, xm_list_entry);
		new_e = list_entry(new_n, struct xio_mr_elem, xm_list_entry);
		te->old_rkey = old_e->mr->rkey;
		te->new_rkey = new_e->mr->rkey;
		te++;
	}

	if (old_n != old_h || new_n != new_h) {
		/* one list terminated before the other this is a program error
		 * there should be an entry per device
		 */
		ERROR_LOG("bug\n");
		goto cleanup;
	}

	*len = mr_num;
	*htbl = tbl;
	return 0;

cleanup:
	ufree(tbl);
	*len = 0;
	return -1;
}
