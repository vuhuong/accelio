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
#include "xio_protocol.h"
#include "xio_mbuf.h"
#include "xio_task.h"
#include "xio_usr_transport.h"
#include "xio_transport.h"
#include "xio_protocol.h"
#include "get_clock.h"
#include "xio_mem.h"
#include "xio_mempool.h"
#include "xio_rdma_utils.h"
#include "xio_ev_data.h"
#include "xio_ev_loop.h"
#include "xio_sg_table.h"
#include "xio_workqueue.h"
#include "xio_context.h"
#include "xio_rdma_transport.h"
#include "xio_context_priv.h"



/* default option values */
#define XIO_OPTVAL_DEF_ENABLE_MEM_POOL			1
#define XIO_OPTVAL_DEF_ENABLE_DMA_LATENCY		0
#define XIO_OPTVAL_DEF_MAX_IN_IOVSZ			XIO_IOVLEN
#define XIO_OPTVAL_DEF_MAX_OUT_IOVSZ			XIO_IOVLEN

/*---------------------------------------------------------------------------*/
/* globals								     */
/*---------------------------------------------------------------------------*/
static spinlock_t			mngmt_lock;
static pthread_rwlock_t			dev_lock;
static pthread_rwlock_t			cm_lock;
static pthread_once_t			ctor_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t			dtor_key_once = PTHREAD_ONCE_INIT;

spinlock_t				dev_list_lock; /* devices list lock */
LIST_HEAD(dev_list);
LIST_HEAD(dev_del_list);
static LIST_HEAD(cm_list);

static struct xio_dev_tdata		dev_tdata;

static int				cdl_fd = -1;

static int				rdma_num_devices; /*= 0;*/

/* rdma options */
struct xio_rdma_options			rdma_options = {
	.enable_mem_pool		= XIO_OPTVAL_DEF_ENABLE_MEM_POOL,
	.enable_dma_latency		= XIO_OPTVAL_DEF_ENABLE_DMA_LATENCY,
	.max_in_iovsz			= XIO_OPTVAL_DEF_MAX_IN_IOVSZ,
	.max_out_iovsz			= XIO_OPTVAL_DEF_MAX_OUT_IOVSZ,
};

/*---------------------------------------------------------------------------*/
/* forward declaration							     */
/*---------------------------------------------------------------------------*/
static struct xio_transport_base *xio_rdma_open(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer,
		uint32_t		trans_attr_mask,
		struct xio_transport_init_attr *attr);


static int xio_rdma_reject(struct xio_transport_base *transport);
static void xio_rdma_close(struct xio_transport_base *transport);
static struct xio_cm_channel *xio_cm_channel_get(struct xio_context *ctx);
static void xio_rdma_post_close(struct xio_transport_base *trans_hndl);
static int xio_rdma_flush_all_tasks(struct xio_rdma_transport *rdma_hndl);
static void xio_device_release(struct xio_device *dev);

/*---------------------------------------------------------------------------*/
/* xio_rdma_get_max_header_size						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_get_max_header_size(void)
{
	int req_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_rdma_req_hdr);
	int rsp_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_rdma_rsp_hdr);
	int iovsz = rdma_options.max_out_iovsz + rdma_options.max_in_iovsz;

	req_hdr += iovsz*sizeof(struct xio_sge);
	rsp_hdr += rdma_options.max_out_iovsz*sizeof(struct xio_sge);

	return max(req_hdr, rsp_hdr);
}

/*---------------------------------------------------------------------------*/
/* xio_async_ev_handler							     */
/*---------------------------------------------------------------------------*/
static void xio_async_ev_handler(int fd, int events, void *user_context)
{
	char			*dev_name = NULL;
	struct ibv_async_event	async_event;
	struct xio_device	*dev = (struct xio_device *)user_context;

	dev_name = dev->verbs->device->name;

	while (1) {
		if (ibv_get_async_event(dev->verbs, &async_event)) {
			if (errno == EAGAIN)
				return;

			xio_set_error(errno);
			ERROR_LOG("ibv_get_async_event failed. (errno=%d %m)\n",
				  errno);
			return;
		}
		ERROR_LOG("ibv_get_async_event: dev:%s evt: %s\n", dev_name,
			  ibv_event_type_str(async_event.event_type));

		if (async_event.event_type == IBV_EVENT_COMM_EST) {
			struct xio_rdma_transport *rdma_hndl;

			rdma_hndl = (struct xio_rdma_transport *)
					async_event.element.qp->qp_context;
			/* force "connection established" event */
			rdma_notify(rdma_hndl->cm_id, IBV_EVENT_COMM_EST);
		}

		ibv_ack_async_event(&async_event);
	}

	return;
}

/*---------------------------------------------------------------------------*/
/* device thread callback						     */
/*---------------------------------------------------------------------------*/
static void *device_thread_cb(void *data)
{
	cpu_set_t		cpuset;
	pthread_t		thread;

	/* set affinity to thread */
	thread = pthread_self();

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset); /* bind the devices thread to first core */

	pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	/* the default xio supplied main loop */
	xio_ev_loop_run(dev_tdata.async_loop);

	/* normal exit phase */
	TRACE_LOG("devices thread exit signaled\n");

	/* destroy the default loop */
	xio_ev_loop_destroy(&dev_tdata.async_loop);
	dev_tdata.async_loop = NULL;

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_device_thread_init						     */
/*---------------------------------------------------------------------------*/
static int xio_device_thread_init(void)
{
	int ret;

	/* open default event loop */
	dev_tdata.async_loop = xio_ev_loop_create();
	if (!dev_tdata.async_loop) {
		ERROR_LOG("xio_ev_loop_init failed\n");
		return -1;
	}
	ret = pthread_create(&dev_tdata.dev_thread, NULL,
			     device_thread_cb, NULL);
	if (ret < 0) {
		ERROR_LOG("pthread_create failed. %m\n");
		/* destroy the default loop */
		xio_ev_loop_destroy(&dev_tdata.async_loop);
		dev_tdata.async_loop = NULL;
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_device_thread_stop						     */
/*---------------------------------------------------------------------------*/
static void xio_device_thread_stop(void)
{
	xio_ev_loop_stop(dev_tdata.async_loop);

	pthread_join(dev_tdata.dev_thread, NULL);
}

/*---------------------------------------------------------------------------*/
/* xio_device_thread_add_device						     */
/*---------------------------------------------------------------------------*/
int xio_device_thread_add_device(struct xio_device *dev)
{
	int retval;

	retval = fcntl(dev->verbs->async_fd, F_GETFL, 0);
	if (retval != -1) {
		retval = fcntl(dev->verbs->async_fd, F_SETFL,
			       retval | O_NONBLOCK);
	}
	if (retval == -1) {
		xio_set_error(errno);
		ERROR_LOG("fcntl failed. (errno=%d %m)\n", errno);
		return -1;
	}

	/* add to epoll */
	retval = xio_ev_loop_add(
			dev_tdata.async_loop,
			dev->verbs->async_fd,
			XIO_POLLIN,
			xio_async_ev_handler,
			dev);
	if (retval != 0) {
		xio_set_error(errno);
		ERROR_LOG("ev_loop_add failed. (errno=%d %m)\n", errno);
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_device_thread_remove_device					     */
/*---------------------------------------------------------------------------*/
int xio_device_thread_remove_device(struct xio_device *dev)
{
	if (dev_tdata.async_loop)
		return xio_ev_loop_del(
				dev_tdata.async_loop,
				dev->verbs->async_fd);
	return 0;
}

#ifdef HAVE_IBV_MODIFY_CQ
/*---------------------------------------------------------------------------*/
/* xio_cq_modify - use to throttle rates				     */
/*---------------------------------------------------------------------------*/
static int xio_cq_modify(struct xio_cq *tcq, int cq_count, int cq_pariod)
{
	struct ibv_cq_attr  cq_attr;
	int		    retval;

	memset(&cq_attr, 0, sizeof(cq_attr));

	cq_attr.comp_mask = IBV_CQ_ATTR_MODERATION;
	cq_attr.moderation.cq_count = cq_count;
	cq_attr.moderation.cq_period = cq_pariod;

	retval = ibv_modify_cq(tcq->cq, &cq_attr,
			       IBV_CQ_MODERATION);
	if (retval)
		ERROR_LOG("ibv_modify_cq failed. (errno=%d %m)\n", errno);

	return retval;
}
#endif

/*---------------------------------------------------------------------------*/
/* xio_cq_down								     */
/*---------------------------------------------------------------------------*/
static void xio_cq_down(struct kref *kref)
{
	struct xio_cq	*tcq = container_of(kref, struct xio_cq, kref);
	int		retval;

	pthread_rwlock_wrlock(&tcq->dev->cq_lock);
	list_del(&tcq->cq_list_entry);
	pthread_rwlock_unlock(&tcq->dev->cq_lock);

	if (!list_empty(&tcq->trans_list))
		ERROR_LOG("rdma_hndl memory leakage\n");

	xio_ctx_remove_event(tcq->ctx, &tcq->consume_cq_event_data);
	xio_ctx_remove_event(tcq->ctx, &tcq->poll_cq_event_data);

	xio_context_unreg_observer(tcq->ctx, &tcq->observer);

	if (tcq->cq_events_that_need_ack != 0) {
		ibv_ack_cq_events(tcq->cq,
				  tcq->cq_events_that_need_ack);
		tcq->cq_events_that_need_ack = 0;
	}

	retval = xio_context_del_ev_handler(
			tcq->ctx,
			tcq->channel->fd);
	if (retval)
		ERROR_LOG("ev_loop_del_cb failed. (errno=%d %m)\n",
			  errno);

	/* the event loop may be release by the time this function is called */
	retval = ibv_destroy_cq(tcq->cq);
	if (retval)
		ERROR_LOG("ibv_destroy_cq failed. (errno=%d %m)\n", errno);

	retval = ibv_destroy_comp_channel(tcq->channel);
	if (retval)
		ERROR_LOG("ibv_destroy_comp_channel failed. (errno=%d %m)\n",
			  errno);

	XIO_OBSERVER_DESTROY(&tcq->observer);

	ufree(tcq->wc_array);
	ufree(tcq);
}

/*---------------------------------------------------------------------------*/
/* xio_cq_release							     */
/*---------------------------------------------------------------------------*/
static inline void xio_cq_release(struct xio_cq *tcq)
{
	kref_put(&tcq->kref, xio_cq_down);
}

/*---------------------------------------------------------------------------*/
/* xio_on_context_event							     */
/*---------------------------------------------------------------------------*/
static int xio_on_context_event(void *observer, void *sender,
				int event, void *event_data)
{
	struct xio_cq	*cq = (struct xio_cq *)observer;

	if (event == XIO_CONTEXT_EVENT_POST_CLOSE) {
		TRACE_LOG("context: [close] ctx:%p\n", sender);
		xio_cq_release(cq);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_cq_create							     */
/*---------------------------------------------------------------------------*/
static struct xio_cq *xio_cq_get(struct xio_device *dev,
				 struct xio_context *ctx)
{
	struct xio_cq		*tcq;
	int			retval;
	int			comp_vec = 0;
	int			alloc_sz;
#ifdef HAVE_IBV_MODIFY_CQ
	int			throttle = 0;
#endif

	list_for_each_entry(tcq, &dev->cq_list, cq_list_entry) {
		if (tcq->ctx == ctx) {
			kref_get(&tcq->kref);
			return tcq;
		}
	}
	tcq = (struct xio_cq *)ucalloc(1, sizeof(struct xio_cq));
	if (tcq == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		goto cleanup;
	}
	tcq->ctx = ctx;

	tcq->wc_array_len = MAX_POLL_WC;
	/* allocate device wc array */
	tcq->wc_array = (struct ibv_wc *)ucalloc(tcq->wc_array_len,
						 sizeof(struct ibv_wc));
	if (tcq->wc_array == NULL) {
		xio_set_error(errno);
		ERROR_LOG("ev_loop_add failed. (errno=%d %m)\n", errno);
		goto cleanup1;
	}

	tcq->alloc_sz = min(dev->device_attr.max_cqe, CQE_ALLOC_SIZE);
	tcq->max_cqe  = dev->device_attr.max_cqe;
	alloc_sz = tcq->alloc_sz;

	/* set com_vector to cpu */
	comp_vec = ctx->cpuid % dev->verbs->num_comp_vectors;

	tcq->channel = ibv_create_comp_channel(dev->verbs);
	if (tcq->channel == NULL) {
		xio_set_error(errno);
		ERROR_LOG("ibv_create_comp_channel failed. (errno=%d %m)\n",
			  errno);
		goto cleanup2;
	}
	retval = fcntl(tcq->channel->fd, F_GETFL, 0);
	if (retval != -1) {
		retval = fcntl(tcq->channel->fd, F_SETFL,
			       retval | O_NONBLOCK);
	}
	if (retval == -1) {
		xio_set_error(errno);
		ERROR_LOG("fcntl failed. (errno=%d %m)\n", errno);
		goto cleanup2;
	}


	/* add to epoll */
	retval = xio_context_add_ev_handler(
			ctx,
			tcq->channel->fd,
			XIO_POLLIN,
			xio_cq_event_handler,
			tcq);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("ev_loop_add_cb failed. (errno=%d %m)\n", errno);
		goto cleanup3;
	}


	tcq->cq = ibv_create_cq(dev->verbs, alloc_sz, tcq,
				tcq->channel, comp_vec);
	TRACE_LOG("comp_vec:%d\n", comp_vec);
	if (tcq->cq == NULL) {
		xio_set_error(errno);
		ERROR_LOG("ibv_create_cq failed. (errno=%d %m)\n", errno);
		if (errno == ENOMEM)
			xio_validate_ulimit_memlock();
		goto cleanup4;
	}

#ifdef HAVE_IBV_MODIFY_CQ
	if (throttle)
		retval = xio_cq_modify(tcq, 5, 5);
#endif

	retval = ibv_req_notify_cq(tcq->cq, 0);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("ibv_req_notify_cq failed. (errno=%d %m)\n",
			  errno);
		goto cleanup5;
	}

	/* set cq depth params */
	tcq->dev	= dev;
	tcq->cq_depth	= tcq->cq->cqe;
	tcq->cqe_avail	= tcq->cq->cqe;

	INIT_LIST_HEAD(&tcq->trans_list);

	list_add(&tcq->cq_list_entry, &dev->cq_list);

	/* One reference count for the context and one for the rdma handle */
	kref_init(&tcq->kref);
	kref_get(&tcq->kref);

	/* set the tcq to be the observer for context events */
	XIO_OBSERVER_INIT(&tcq->observer, tcq, xio_on_context_event);
	xio_context_reg_observer(ctx, &tcq->observer);

	return tcq;

cleanup5:
	retval = ibv_destroy_cq(tcq->cq);
	if (retval)
		ERROR_LOG("ibv_destroy_cq failed. (errno=%d %m)\n", errno);
cleanup4:
	xio_context_del_ev_handler(
			ctx,
			tcq->channel->fd);
cleanup3:
	retval = ibv_destroy_comp_channel(tcq->channel);
	if (retval)
		ERROR_LOG("ibv_destroy_comp_channel failed. (errno=%d %m)\n",
			  errno);
cleanup2:
	ufree(tcq->wc_array);
cleanup1:
	ufree(tcq);
cleanup:
	return NULL;
}


/*---------------------------------------------------------------------------*/
/* xio_device_init							     */
/*---------------------------------------------------------------------------*/
static struct xio_device *xio_device_init(struct ibv_context *ib_ctx)
{
	struct xio_device	*dev;
	int			retval;

	dev = (struct xio_device *)ucalloc(1, sizeof(*dev));
	if (dev == NULL) {
		xio_set_error(errno);
		ERROR_LOG("ucalloc failed. (errno=%d %m)\n", errno);
		return NULL;
	}
	dev->verbs	= ib_ctx;

	dev->pd = ibv_alloc_pd(dev->verbs);
	if (dev->pd == NULL) {
		xio_set_error(errno);
		ERROR_LOG("ibv_alloc_pd failed. (errno=%d %m)\n", errno);
		goto cleanup;
	}
	retval = ibv_xio_query_device(dev->verbs, &dev->device_attr);
	if (retval < 0) {
		ERROR_LOG("ibv_query_device failed. (errno=%d %m)\n", errno);
		goto cleanup1;
	}

	retval = xio_device_thread_add_device(dev);
	if (retval) {
		ERROR_LOG(
		"xio_device_thread_add_device failed. (errno=%d %m)\n",
		errno);
		goto cleanup1;
	}

	INIT_LIST_HEAD(&dev->cq_list);
	/* Initialize list of MR for this device */
	INIT_LIST_HEAD(&dev->xm_list);
	INIT_LIST_HEAD(&dev->dev_list_entry);
	pthread_rwlock_init(&dev->cq_lock, NULL);
	kref_init(&dev->kref);
	TRACE_LOG("rdma device: [new] %p\n", dev);


	return dev;

cleanup1:
	ibv_dealloc_pd(dev->pd);
cleanup:
	ufree(dev);

	ERROR_LOG("rdma device: [new] failed\n");
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_device_lookup							     */
/*---------------------------------------------------------------------------*/
static struct xio_device *xio_device_lookup(struct ibv_context *verbs)
{
	struct xio_device *dev;

	/* Actually we should compare GUID(s) assume device is released and
	 * a new device gets the memory allocated for the old one
	 */
	spin_lock(&dev_list_lock);
	/* Loop on known devices (need locking) */
	list_for_each_entry(dev, &dev_list, dev_list_entry) {
		if (dev->verbs == verbs) {
			/* increment device reference count */
			xio_device_get(dev);
			spin_unlock(&dev_list_lock);
			return dev;
		}
	}
	spin_unlock(&dev_list_lock);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_device_lookup_init						     */
/*---------------------------------------------------------------------------*/
static struct xio_device *xio_device_lookup_init(struct ibv_context *verbs)
{
	struct xio_device *dev;

	if (verbs == NULL) {
		xio_set_error(ENODEV);
		ERROR_LOG("NULL ibv_context\n");
		return NULL;
	}

	dev = xio_device_lookup(verbs);
	if (dev)
		goto exit;

	/* Connection on new device */
	TRACE_LOG("Connection via new device %s\n",
		  ibv_get_device_name(verbs->device));

	dev = xio_device_init(verbs);
	if (!dev) {
		ERROR_LOG("Couldn't allocate device %s\n",
			  ibv_get_device_name(verbs->device));
		goto cleanup0;
	}

	/* Update all MR with new device */
	if (xio_reg_mr_add_dev(dev)) {
		ERROR_LOG("Couldn't allocate device %s\n",
			  ibv_get_device_name(verbs->device));
		goto cleanup1;
	}

	/* Add reference count on behalf of the new connection */
	xio_device_get(dev);

	/* Add the new device */
	spin_lock(&dev_list_lock);
	list_add(&dev->dev_list_entry, &dev_list);
	spin_unlock(&dev_list_lock);

exit:
	return dev;

cleanup1:
	xio_device_release(dev);

cleanup0:
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_device_down							     */
/*---------------------------------------------------------------------------*/
void xio_device_down(struct kref *kref)
{
	struct xio_device *dev = container_of(kref, struct xio_device, kref);
	int retval;

	spin_lock(&dev_list_lock);
	list_del(&dev->dev_list_entry);
	spin_unlock(&dev_list_lock);

	xio_dereg_mr_by_dev(dev);

	retval = ibv_dealloc_pd(dev->pd);
	if (retval)
		ERROR_LOG("ibv_dealloc_pd failed. (errno=%d %s)\n",
			  retval, strerror(retval));

	ufree(dev);
}

/*---------------------------------------------------------------------------*/
/* xio_device_release							     */
/*---------------------------------------------------------------------------*/
static void xio_device_release(struct xio_device *dev)
{
	int			retval;

	TRACE_LOG("rdma device: [close] dev:%p\n", dev);

	retval = xio_device_thread_remove_device(dev);
	if (retval) {
		ERROR_LOG(
			"xio_device_thread_add_device failed. (errno=%d %m)\n",
			errno);
	}

	/* don't delete the fd - the  loop may not exist at this stage */
	if (!list_empty(&dev->cq_list))
		ERROR_LOG("cq memory leakage\n");

	pthread_rwlock_destroy(&dev->cq_lock);

	spin_lock(&dev_list_lock);
	list_move_tail(&dev->dev_list_entry, &dev_del_list);
	spin_unlock(&dev_list_lock);

	/* ibv_dealloc_pd will be called from xio_device_down (kerf) */
	xio_device_put(dev);
}

/*---------------------------------------------------------------------------*/
/* xio_device_list_check						     */
/*---------------------------------------------------------------------------*/
static void xio_device_list_check(void)
{
	struct ibv_context **ctx_list;
	int num_devices = 0;

	rdma_num_devices = 0;

	ctx_list = rdma_get_devices(&num_devices);
	if (!ctx_list)
		return;

	if (!*ctx_list || num_devices == 0)
		goto exit;

	rdma_num_devices = num_devices;
exit:
	rdma_free_devices(ctx_list);

	return;
}

/*---------------------------------------------------------------------------*/
/* xio_device_list_init							     */
/*---------------------------------------------------------------------------*/
static int xio_device_list_init(void)
{
	struct ibv_context **ctx_list;
	struct xio_device *dev;
	int num_devices = 0, i;
	int retval = 0;

	INIT_LIST_HEAD(&dev_list);

	rdma_num_devices = 0;

	ctx_list = rdma_get_devices(&num_devices);
	if (!ctx_list) {
		xio_set_error(errno);
		ERROR_LOG("Failed to get IB devices list\n");
		return -1;
	}

	if (!*ctx_list) {
		xio_set_error(ENODEV);
		ERROR_LOG("No IB devices found\n");
		retval = -1;
		goto exit;
	}

	rdma_num_devices = num_devices;

	for (i = 0; i < num_devices; ++i) {
		dev = xio_device_init(ctx_list[i]);
		if (!dev) {
			ERROR_LOG("Couldn't allocate device %s\n",
				  ibv_get_device_name(ctx_list[i]->device));
			retval = -1;
			goto exit;
		}
		pthread_rwlock_wrlock(&dev_lock);
		list_add(&dev->dev_list_entry, &dev_list);
		pthread_rwlock_unlock(&dev_lock);
	}

exit:
	rdma_free_devices(ctx_list);

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_device_list_release						     */
/*---------------------------------------------------------------------------*/
static void xio_device_list_release(void)
{
	struct xio_device	*dev, *next;

	/* free devices */
	pthread_rwlock_wrlock(&dev_lock);
	list_for_each_entry_safe(dev, next, &dev_list, dev_list_entry) {
		/* xio_device_release needs to do list_move -> _init */
		list_del_init(&dev->dev_list_entry);
		xio_device_release(dev);
	}
	pthread_rwlock_unlock(&dev_lock);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_mr_lookup							     */
/*---------------------------------------------------------------------------*/
static inline struct ibv_mr *xio_rdma_mr_lookup(struct xio_mr *tmr,
						struct xio_device *dev)
{
	struct xio_mr_elem *tmr_elem;

	list_for_each_entry(tmr_elem, &tmr->dm_list, dm_list_entry) {
		if (dev == tmr_elem->dev)
			return tmr_elem->mr;
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_cm_channel_down							     */
/*---------------------------------------------------------------------------*/
void xio_cm_channel_down(struct kref *kref)
{
	struct xio_cm_channel	*channel =
		container_of(kref, struct xio_cm_channel, kref);

	pthread_rwlock_wrlock(&cm_lock);
	list_del(&channel->channels_list_entry);
	pthread_rwlock_unlock(&cm_lock);
	xio_context_del_ev_handler(channel->ctx, channel->cm_channel->fd);
	rdma_destroy_event_channel(channel->cm_channel);
	ufree(channel);
}

/*---------------------------------------------------------------------------*/
/* xio_cm_channel_release						     */
/*---------------------------------------------------------------------------*/
static inline void xio_cm_channel_release(struct xio_cm_channel *channel)
{
	kref_put(&channel->kref, xio_cm_channel_down);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_context_shutdown						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_context_shutdown(struct xio_transport_base *trans_hndl,
				     struct xio_context *ctx)
{
	struct xio_rdma_transport *rdma_hndl =
				(struct xio_rdma_transport *)trans_hndl;

	DEBUG_LOG("context: [shutdown] trans_hndl:%p\n", trans_hndl);
	/*due to long timewait - force ignoring */
	rdma_hndl->ignore_timewait = 1;

	xio_context_destroy_wait(ctx);
	xio_rdma_close(trans_hndl);

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_cq_alloc_slots							     */
/*---------------------------------------------------------------------------*/
int xio_cq_alloc_slots(struct xio_cq *tcq, int cqe_num)
{
	if (cqe_num < tcq->cqe_avail) {
		tcq->cqe_avail -= cqe_num;
		return 0;
	} else if (tcq->cq_depth + tcq->alloc_sz < tcq->max_cqe) {
		int cqe = tcq->cq->cqe;
		int retval = ibv_resize_cq(tcq->cq,
					   (tcq->cq_depth + tcq->alloc_sz));
		if (retval != 0 || (cqe == tcq->cq->cqe)) {
			ERROR_LOG("ibv_resize_cq failed. %m, cqe:%d\n", cqe);
			return -1;
		}
		tcq->cq_depth  += (tcq->cq->cqe - cqe);
		tcq->cqe_avail += (tcq->cq->cqe - cqe);
		DEBUG_LOG("cq_resize: expected:%d, actual:%d\n",
			  tcq->cq_depth, tcq->cq->cqe);
		tcq->cqe_avail -= cqe_num;
		return 0;
	} else {
		ERROR_LOG("cq overflow reached\n");
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_cq_free_slots							     */
/*---------------------------------------------------------------------------*/
static int xio_cq_free_slots(struct xio_cq *tcq, int cqe_num)
{
	if (tcq->cqe_avail + cqe_num <= tcq->cq_depth) {
		tcq->cqe_avail += cqe_num;
		return 0;
	}
	ERROR_LOG("cq allocation error");

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_qp_create							     */
/*---------------------------------------------------------------------------*/
static int xio_qp_create(struct xio_rdma_transport *rdma_hndl)
{
	struct	xio_cq			*tcq;
	struct xio_device		*dev = rdma_hndl->dev;
	struct ibv_qp_init_attr		qp_init_attr;
	struct ibv_qp_attr		qp_attr;
	int				retval = 0;

	tcq = xio_cq_get(dev, rdma_hndl->base.ctx);
	if (tcq == NULL) {
		ERROR_LOG("cq initialization failed\n");
		return -1;
	}
	retval = xio_cq_alloc_slots(tcq, MAX_CQE_PER_QP);
	if (retval != 0) {
		ERROR_LOG("cq full capacity reached\n");
		goto release_cq;
	}

	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	qp_init_attr.qp_context		  = rdma_hndl;
	qp_init_attr.qp_type		  = IBV_QPT_RC;
	qp_init_attr.send_cq		  = tcq->cq;
	qp_init_attr.recv_cq		  = tcq->cq;
	qp_init_attr.cap.max_send_wr	  = MAX_SEND_WR;
	qp_init_attr.cap.max_recv_wr	  = MAX_RECV_WR + EXTRA_RQE;
	qp_init_attr.cap.max_send_sge	  = min(rdma_options.max_out_iovsz + 1,
						dev->device_attr.max_sge);
	qp_init_attr.cap.max_recv_sge	  = 1;
	qp_init_attr.cap.max_inline_data  = MAX_INLINE_DATA;

	/* only generate completion queue entries if requested */
	qp_init_attr.sq_sig_all		= 0;

	retval = rdma_create_qp(rdma_hndl->cm_id, dev->pd, &qp_init_attr);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("rdma_create_qp failed. (errno=%d %m)\n", errno);
		if (errno == ENOMEM)
			xio_validate_ulimit_memlock();
		goto free_slots;
	}
	rdma_hndl->tcq		= tcq;
	rdma_hndl->qp		= rdma_hndl->cm_id->qp;
	rdma_hndl->sqe_avail	= MAX_SEND_WR;

	rdma_hndl->beacon_task.dd_data = ptr_from_int64(XIO_BEACON_WRID);
	rdma_hndl->beacon.wr_id	 = uint64_from_ptr(&rdma_hndl->beacon_task);
	rdma_hndl->beacon.opcode = IBV_WR_SEND;

	memset(&qp_attr, 0, sizeof(qp_attr));
	if (ibv_query_qp(rdma_hndl->qp, &qp_attr, 0, &qp_init_attr) != 0)
		ERROR_LOG("ibv_query_qp failed. (errno=%d %m)\n", errno);
	rdma_hndl->max_inline_data = qp_attr.cap.max_inline_data;
	rdma_hndl->max_sge	   = min(rdma_options.max_out_iovsz + 1,
					 dev->device_attr.max_sge);

	list_add(&rdma_hndl->trans_list_entry, &tcq->trans_list);

	DEBUG_LOG("rdma qp: [new] handle:%p, qp:0x%x, max inline:%d\n",
		  rdma_hndl,
		  rdma_hndl->qp->qp_num,
		  rdma_hndl->max_inline_data);

	return 0;

free_slots:
	xio_cq_free_slots(tcq, MAX_CQE_PER_QP);

release_cq:

	xio_cq_release(tcq);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_qp_release							     */
/*---------------------------------------------------------------------------*/
static void xio_qp_release(struct xio_rdma_transport *rdma_hndl)
{
	if (rdma_hndl->qp) {
		TRACE_LOG("rdma qp: [close] handle:%p, qp:%p\n", rdma_hndl,
			  rdma_hndl->qp);
		xio_cq_free_slots(rdma_hndl->tcq, MAX_CQE_PER_QP);
		list_del(&rdma_hndl->trans_list_entry);
		rdma_destroy_qp(rdma_hndl->cm_id);
		xio_cq_release(rdma_hndl->tcq);
		rdma_hndl->qp	= NULL;
	}
}

/*---------------------------------------------------------------------------*/
/* xio_rxd_init								     */
/*---------------------------------------------------------------------------*/
static void xio_rxd_init(struct xio_work_req *rxd,
			 struct xio_task *task,
			 void *buf, unsigned size,
			 struct ibv_mr *srmr)
{
	if (size) {
		rxd->sge[0].addr	= uint64_from_ptr(buf);
		rxd->sge[0].length	= size;
		rxd->sge[0].lkey	= srmr->lkey;
	}

	rxd->recv_wr.wr_id	= uint64_from_ptr(task);
	rxd->recv_wr.sg_list	= rxd->sge;
	rxd->recv_wr.num_sge	= size ? 1 : 0;
	rxd->recv_wr.next	= NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_txd_init								     */
/*---------------------------------------------------------------------------*/
static void xio_txd_init(struct xio_work_req *txd,
			 struct xio_task *task,
			 void *buf, unsigned size,
			 struct ibv_mr *srmr)
{
	if (size) {
		txd->sge[0].addr	= uint64_from_ptr(buf);
		txd->sge[0].length	= size;
		txd->sge[0].lkey	= srmr->lkey;
	}

	txd->send_wr.wr_id	= uint64_from_ptr(task);
	txd->send_wr.next	= NULL;
	txd->send_wr.sg_list	= txd->sge;
	txd->send_wr.num_sge	= size ? 1 : 0;
	txd->send_wr.opcode	= IBV_WR_SEND;

	/*txd->send_wr.send_flags = IBV_SEND_SIGNALED; */
}

/*---------------------------------------------------------------------------*/
/* xio_rdmad_init							     */
/*---------------------------------------------------------------------------*/
static void xio_rdmad_init(struct xio_work_req *rdmad,
			   struct xio_task *task)
{
	rdmad->send_wr.wr_id = uint64_from_ptr(task);
	rdmad->send_wr.sg_list = rdmad->sge;
	rdmad->send_wr.num_sge = 1;
	rdmad->send_wr.next = NULL;
	rdmad->send_wr.send_flags = IBV_SEND_SIGNALED;

	/* to be set before posting:
	   rdmad->iser_ib_op, rdmad->send_wr.opcode
	   rdmad->sge.addr, rdmad->sge.length
	   rdmad->send_wr.wr.rdma.(remote_addr,rkey) */
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_task_init							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_task_init(struct xio_task *task,
			       struct xio_rdma_transport *rdma_hndl,
			       void *buf,
			       unsigned long size,
			       struct ibv_mr *srmr)
{
	XIO_TO_RDMA_TASK(task, rdma_task);

	rdma_task->rdma_hndl = rdma_hndl;

	xio_rxd_init(&rdma_task->rxd, task, buf, size, srmr);
	xio_txd_init(&rdma_task->txd, task, buf, size, srmr);
	xio_rdmad_init(&rdma_task->rdmad, task);

	/* initialize the mbuf */
	if (buf)
		xio_mbuf_init(&task->mbuf, buf, size, 0);
}

/*---------------------------------------------------------------------------*/
/* xio_txd_reinit							     */
/*---------------------------------------------------------------------------*/
static void xio_xd_reinit(struct xio_work_req *xd,
			  size_t xd_nr,
			  struct ibv_mr *srmr)
{
	unsigned int i;

	if (!srmr)
		return;

	for (i = 0; i < xd_nr; i++) {
		if (!xd->sge[i].lkey)
			break;
		xd->sge[i].lkey = srmr->lkey;
	}
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_task_reinit							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_task_reinit(struct xio_task *task,
				struct xio_rdma_transport *rdma_hndl,
				struct ibv_mr *srmr)
{
	XIO_TO_RDMA_TASK(task, rdma_task);

	xio_xd_reinit(&rdma_task->rxd, rdma_hndl->max_sge, srmr);
	xio_xd_reinit(&rdma_task->txd, rdma_hndl->max_sge, srmr);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_flush_all_tasks						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_flush_all_tasks(struct xio_rdma_transport *rdma_hndl)
{
	if (!list_empty(&rdma_hndl->in_flight_list)) {
		TRACE_LOG("in_flight_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->in_flight_list);
		/* for task that attached to senders with ref count = 2 */
		xio_transport_flush_task_list(&rdma_hndl->in_flight_list);
	}

	if (!list_empty(&rdma_hndl->rdma_rd_in_flight_list)) {
		TRACE_LOG("rdma_rd_in_flight_list not empty!\n");
		xio_transport_flush_task_list(
					 &rdma_hndl->rdma_rd_in_flight_list);
	}

	if (!list_empty(&rdma_hndl->rdma_rd_list)) {
		TRACE_LOG("rdma_rd_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->rdma_rd_list);
	}

	if (!list_empty(&rdma_hndl->tx_comp_list)) {
		TRACE_LOG("tx_comp_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->tx_comp_list);
	}
	if (!list_empty(&rdma_hndl->io_list)) {
		TRACE_LOG("io_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->io_list);
	}

	if (!list_empty(&rdma_hndl->tx_ready_list)) {
		TRACE_LOG("tx_ready_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->tx_ready_list);
		/* for task that attached to senders with ref count = 2 */
		xio_transport_flush_task_list(&rdma_hndl->tx_ready_list);
	}

	if (!list_empty(&rdma_hndl->rx_list)) {
		TRACE_LOG("rx_list not empty!\n");
		xio_transport_flush_task_list(&rdma_hndl->rx_list);
	}

	rdma_hndl->kick_rdma_rd = 0;
	rdma_hndl->rdma_in_flight = 0;
	rdma_hndl->reqs_in_flight_nr = 0;
	rdma_hndl->rsps_in_flight_nr = 0;
	rdma_hndl->tx_ready_tasks_num = 0;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_pool_slab_pre_create				     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_initial_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr, void *pool_dd_data, void *slab_dd_data)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;
	uint32_t pool_size;

	rdma_slab->buf_size = CONN_SETUP_BUF_SIZE;
	pool_size = rdma_slab->buf_size * alloc_nr;
	rdma_slab->data_pool = ucalloc(pool_size, sizeof(uint8_t));
	if (rdma_slab->data_pool == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc conn_setup_data_pool sz: %u failed\n",
			  pool_size);
		return -1;
	}

	rdma_slab->data_mr = ibv_reg_mr(rdma_hndl->tcq->dev->pd,
			rdma_slab->data_pool,
			pool_size, IBV_ACCESS_LOCAL_WRITE);
	if (!rdma_slab->data_mr) {
		xio_set_error(errno);
		ufree(rdma_slab->data_pool);
		ERROR_LOG("ibv_reg_mr conn_setup pool failed, %m\n");
		if (errno == ENOMEM)
			xio_validate_ulimit_memlock();
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_task_alloc						     */
/*---------------------------------------------------------------------------*/
static inline struct xio_task *xio_rdma_initial_task_alloc(
					struct xio_rdma_transport *rdma_hndl)
{
	if (rdma_hndl->initial_pool_cls.task_get) {
		return rdma_hndl->initial_pool_cls.task_get(
					rdma_hndl->initial_pool_cls.pool);
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_task_alloc						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_rdma_primary_task_alloc(
					struct xio_rdma_transport *rdma_hndl)
{
	if (rdma_hndl->primary_pool_cls.task_get)
		return rdma_hndl->primary_pool_cls.task_get(
					rdma_hndl->primary_pool_cls.pool);
	return NULL;
}
/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_task_lookup						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_rdma_primary_task_lookup(
					struct xio_rdma_transport *rdma_hndl,
					int tid)
{
	if (rdma_hndl->primary_pool_cls.task_lookup)
		return rdma_hndl->primary_pool_cls.task_lookup(
					rdma_hndl->primary_pool_cls.pool, tid);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_task_free							     */
/*---------------------------------------------------------------------------*/
inline void xio_rdma_task_free(struct xio_rdma_transport *rdma_hndl,
			       struct xio_task *task)
{
	if (rdma_hndl->primary_pool_cls.task_put)
		return rdma_hndl->primary_pool_cls.task_put(task);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_initial_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_task *task;
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_task *rdma_task;
	int	retval;

	rdma_hndl->initial_pool_cls.pool = pool;

	task = xio_rdma_initial_task_alloc(rdma_hndl);
	if (task == NULL) {
		ERROR_LOG("failed to get task\n");
	} else {
		DEBUG_LOG("post_recv conn_setup rx task:%p\n", task);
		retval = xio_post_recv(rdma_hndl, task, 1);
		if (retval)
			ERROR_LOG("xio_post_recv failed\n");

		/* assuming that both sides posted one recv wr for initial
		 * negotiation
		 */
		rdma_hndl->peer_credits	= 1;
		rdma_hndl->sim_peer_credits = 1;
		rdma_task = (struct xio_rdma_task *)task->dd_data;

		rdma_task->ib_op	= XIO_IB_RECV;
		list_add_tail(&task->tasks_list_entry, &rdma_hndl->rx_list);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_task_pre_put						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_task_pre_put(
		struct xio_transport_base *trans_hndl,
		struct xio_task *task)
{
	unsigned int	i;
	XIO_TO_RDMA_TASK(task, rdma_task);

	/* recycle RDMA  buffers back to pool */

	/* put buffers back to pool */
	if (rdma_task->read_num_sge) {
		for (i = 0; i < rdma_task->read_num_sge; i++) {
			if (rdma_task->read_sge[i].cache) {
				xio_mempool_free(&rdma_task->read_sge[i]);
				rdma_task->read_sge[i].cache = NULL;
			}
		}
		rdma_task->read_num_sge = 0;
	}

	if (rdma_task->write_num_sge) {
		for (i = 0; i < rdma_task->write_num_sge; i++) {
			if (rdma_task->write_sge[i].cache) {
				xio_mempool_free(&rdma_task->write_sge[i]);
				rdma_task->write_sge[i].cache = NULL;
			}
		}
		rdma_task->write_num_sge	= 0;
	}
	/*
	rdma_task->req_write_num_sge	= 0;
	rdma_task->rsp_write_num_sge	= 0;
	rdma_task->req_read_num_sge	= 0;
	rdma_task->req_recv_num_sge	= 0;

	rdma_task->txd.send_wr.num_sge = 1;
	rdma_task->ib_op = XIO_IB_NULL;
	rdma_task->phantom_idx = 0;
	rdma_task->sn = 0;
	*/
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_initial_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;

	ibv_dereg_mr(rdma_slab->data_mr);
	ufree(rdma_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_initial_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data,
		void *slab_dd_data, int tid, struct xio_task *task)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;
	void *buf = rdma_slab->data_pool + tid*rdma_slab->buf_size;
	char *ptr;

	XIO_TO_RDMA_TASK(task, rdma_task);

	/* fill xio_rdma_task */
	ptr = (char *)rdma_task;
	ptr += sizeof(struct xio_rdma_task);

	/* fill xio_work_req */
	rdma_task->txd.sge = (struct ibv_sge *)ptr;
	ptr += sizeof(struct ibv_sge);

	rdma_task->rxd.sge = (struct ibv_sge *)ptr;
	ptr += sizeof(struct ibv_sge);
	/*****************************************/

	xio_rdma_task_init(
			task,
			rdma_hndl,
			buf,
			rdma_slab->buf_size,
			rdma_slab->data_mr);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_initial_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_initial_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{
	*start_nr = NUM_CONN_SETUP_TASKS;
	*alloc_nr = 0;
	*max_nr = NUM_CONN_SETUP_TASKS;
	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_rdma_tasks_slab);
	*task_dd_sz = sizeof(struct xio_rdma_task) +
		      2*sizeof(struct ibv_sge);
}

static struct xio_tasks_pool_ops initial_tasks_pool_ops = {
	.pool_get_params	= xio_rdma_initial_pool_get_params,
	.slab_pre_create	= xio_rdma_initial_pool_slab_pre_create,
	.slab_destroy		= xio_rdma_initial_pool_slab_destroy,
	.slab_init_task		= xio_rdma_initial_pool_slab_init_task,
	.pool_post_create	= xio_rdma_initial_pool_post_create
};

/*---------------------------------------------------------------------------*/
/* xio_rdma_phantom_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_phantom_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data,
		void *slab_dd_data, int tid, struct xio_task *task)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	char *ptr;

	XIO_TO_RDMA_TASK(task, rdma_task);

	/* fill xio_rdma_task */
	ptr = (char *)rdma_task;
	ptr += sizeof(struct xio_rdma_task);

	/* fill xio_work_req */
	rdma_task->rdmad.sge = (struct ibv_sge *)ptr;
	/*ptr += rdma_hndl->max_sge*sizeof(struct ibv_sge);*/
	/*****************************************/

	rdma_task->ib_op = (enum xio_ib_op_code)0x200;
	xio_rdma_task_init(
			task,
			rdma_hndl,
			NULL,
			0,
			NULL);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_phantom_pool_create						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_phantom_pool_create(struct xio_rdma_transport *rdma_hndl)
{
	struct xio_tasks_pool_params	params;

	memset(&params, 0, sizeof(params));

	params.start_nr			   = NUM_START_PHANTOM_POOL_TASKS;
	params.max_nr			   = NUM_MAX_PHANTOM_POOL_TASKS;
	params.alloc_nr			   = NUM_ALLOC_PHANTOM_POOL_TASKS;
	params.pool_dd_data_sz		   = 0;
	params.slab_dd_data_sz		   = sizeof(struct xio_rdma_tasks_slab);
	params.task_dd_data_sz		   = sizeof(struct xio_rdma_task) +
				rdma_hndl->max_sge*sizeof(struct ibv_sge);

	params.pool_hooks.context	   = rdma_hndl;
	params.pool_hooks.slab_init_task   =
		(int (*)(void *, void *, void *, int,  struct xio_task *))
		xio_rdma_phantom_pool_slab_init_task;
	params.pool_hooks.slab_uninit_task = NULL;
	params.pool_hooks.task_pre_put	   =
		(int (*)(void *, struct xio_task *))xio_rdma_task_pre_put;

	/* initialize the tasks pool */
	rdma_hndl->phantom_tasks_pool = xio_tasks_pool_create(&params);
	if (rdma_hndl->phantom_tasks_pool == NULL) {
		ERROR_LOG("xio_tasks_pool_create failed\n");
		goto cleanup;
	}

	return 0;

cleanup:
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_phantom_pool_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_phantom_pool_destroy(struct xio_rdma_transport *rdma_hndl)
{
	if (!rdma_hndl->phantom_tasks_pool)
		return -1;

	xio_tasks_pool_destroy(rdma_hndl->phantom_tasks_pool);
	return  0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_slab_pre_create				     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr, void *pool_dd_data, void *slab_dd_data)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;
	size_t alloc_sz = alloc_nr*rdma_hndl->membuf_sz;

	rdma_slab->alloc_nr = alloc_nr;
	rdma_slab->buf_size = rdma_hndl->membuf_sz;

	if (disable_huge_pages) {
		rdma_slab->io_buf = xio_alloc(alloc_sz);
		if (!rdma_slab->io_buf) {
			xio_set_error(ENOMEM);
			ERROR_LOG("xio_alloc rdma pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}
		rdma_slab->data_pool = rdma_slab->io_buf->addr;
		rdma_slab->data_mr = xio_rdma_mr_lookup(
						rdma_slab->io_buf->mr,
						rdma_hndl->tcq->dev);
		if (!rdma_slab->data_mr) {
			xio_set_error(errno);
			xio_free(&rdma_slab->io_buf);
			ERROR_LOG("ibv_reg_mr failed, %m\n");
			return -1;
		}
	} else {
		/* maybe allocation of with unuma_alloc can provide better
		 * performance?
		 */
		rdma_slab->data_pool = umalloc_huge_pages(alloc_sz);
		if (!rdma_slab->data_pool) {
			xio_set_error(ENOMEM);
			ERROR_LOG("malloc rdma pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}

		/* One pool of registered memory per PD */
		rdma_slab->data_mr = ibv_reg_mr(rdma_hndl->tcq->dev->pd,
				rdma_slab->data_pool,
				alloc_sz,
				IBV_ACCESS_LOCAL_WRITE);
		if (!rdma_slab->data_mr) {
			xio_set_error(errno);
			ufree_huge_pages(rdma_slab->data_pool);
			ERROR_LOG("ibv_reg_mr failed, %m\n");
			if (errno == ENOMEM)
				xio_validate_ulimit_memlock();
			return -1;
		}
	}

	DEBUG_LOG("pool buf:%p, mr:%p lkey:0x%x\n",
		  rdma_slab->data_pool, rdma_slab->data_mr,
		  rdma_slab->data_mr->lkey);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_slab_post_create				     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_slab_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;

	if (!rdma_slab->data_mr)
		return 0;

	/* With reconnect can use another HCA */
	if (rdma_slab->data_mr->pd == rdma_hndl->tcq->dev->pd)
		return 0;

	if (!rdma_slab->io_buf) {
		size_t alloc_sz = rdma_slab->buf_size * rdma_slab->alloc_nr;
		ibv_dereg_mr(rdma_slab->data_mr);
		rdma_slab->data_mr = ibv_reg_mr(rdma_hndl->tcq->dev->pd,
						rdma_slab->data_pool,
						alloc_sz,
						IBV_ACCESS_LOCAL_WRITE);
		if (!rdma_slab->data_mr) {
			xio_set_error(errno);
			ufree_huge_pages(rdma_slab->data_pool);
			ERROR_LOG("ibv_reg_mr failed, %m\n");
			if (errno == ENOMEM)
				xio_validate_ulimit_memlock();
			return -1;
		}
	} else {
		rdma_slab->data_mr = xio_rdma_mr_lookup(
						rdma_slab->io_buf->mr,
						rdma_hndl->tcq->dev);
		if (!rdma_slab->data_mr) {
			xio_set_error(errno);
			xio_free(&rdma_slab->io_buf);
			ERROR_LOG("ibv_reg_mr failed, %m\n");
			return -1;
		}
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;

	rdma_hndl->primary_pool_cls.pool = pool;

	xio_rdma_rearm_rq(rdma_hndl);

	/* late creation */
	xio_rdma_phantom_pool_create(rdma_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;

	if (rdma_slab->io_buf) {
		xio_free(&rdma_slab->io_buf);
	} else {
		ibv_dereg_mr(rdma_slab->data_mr);
		ufree_huge_pages(rdma_slab->data_pool);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_slab_remap_task				     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_slab_remap_task(
				struct xio_transport_base *old_th,
				struct xio_transport_base *new_th,
				void *pool_dd_data, void *slab_dd_data,
				struct xio_task *task)
{
	struct xio_rdma_transport *old_hndl =
		(struct xio_rdma_transport *)old_th;
	struct xio_rdma_transport *new_hndl =
		(struct xio_rdma_transport *)new_th;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;
	XIO_TO_RDMA_TASK(task, rdma_task);

	rdma_task->rdma_hndl = new_hndl;

	/* if the same device is used then there is no need to remap */
	if (old_hndl->tcq->dev == new_hndl->tcq->dev)
		return 0;

	xio_rdma_task_reinit(task, new_hndl, rdma_slab->data_mr);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_primary_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data,
		void *slab_dd_data, int tid, struct xio_task *task)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	struct xio_rdma_tasks_slab *rdma_slab =
		(struct xio_rdma_tasks_slab *)slab_dd_data;
	void *buf = rdma_slab->data_pool + tid*rdma_slab->buf_size;
	int  max_iovsz = max(rdma_options.max_out_iovsz,
			     rdma_options.max_in_iovsz) + 1;
	int  max_sge = min(rdma_hndl->max_sge, max_iovsz);
	char *ptr;

	XIO_TO_RDMA_TASK(task, rdma_task);

	/* fill xio_rdma_task */
	ptr = (char *)rdma_task;
	ptr += sizeof(struct xio_rdma_task);

	/* fill xio_work_req */
	rdma_task->txd.sge = (struct ibv_sge *)ptr;
	ptr += max_sge*sizeof(struct ibv_sge);
	rdma_task->rxd.sge = (struct ibv_sge *)ptr;
	ptr += sizeof(struct ibv_sge);
	rdma_task->rdmad.sge = (struct ibv_sge *)ptr;
	ptr += max_sge*sizeof(struct ibv_sge);

	rdma_task->read_sge = (struct xio_mempool_obj *)ptr;
	ptr += max_iovsz*sizeof(struct xio_mempool_obj);
	rdma_task->write_sge = (struct xio_mempool_obj *)ptr;
	ptr += max_iovsz*sizeof(struct xio_mempool_obj);

	rdma_task->req_read_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	rdma_task->req_write_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	rdma_task->req_recv_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	rdma_task->rsp_write_sge = (struct xio_sge *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	/*****************************************/

	rdma_task->ib_op = (enum xio_ib_op_code)0x200;

	xio_rdma_task_init(
			task,
			rdma_hndl,
			buf,
			rdma_slab->buf_size,
			rdma_slab->data_mr);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_primary_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_primary_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport_hndl;
	int  max_iovsz = max(rdma_options.max_out_iovsz,
			     rdma_options.max_in_iovsz) + 1;
	int  max_sge = min(rdma_hndl->max_sge, max_iovsz);

	*start_nr = NUM_START_PRIMARY_POOL_TASKS;
	*alloc_nr = NUM_ALLOC_PRIMARY_POOL_TASKS;
	*max_nr = max((g_options.snd_queue_depth_msgs +
		       g_options.rcv_queue_depth_msgs)*40, 1024);

	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_rdma_tasks_slab);
	*task_dd_sz = sizeof(struct xio_rdma_task) +
		(max_sge + 1 + max_sge)*sizeof(struct ibv_sge) +
		 2 * max_iovsz * sizeof(struct xio_mempool_obj) +
		 4 * max_iovsz * sizeof(struct xio_sge);
}

static struct xio_tasks_pool_ops   primary_tasks_pool_ops = {
	.pool_get_params	= xio_rdma_primary_pool_get_params,
	.slab_pre_create	= xio_rdma_primary_pool_slab_pre_create,
	.slab_post_create	= xio_rdma_primary_pool_slab_post_create,
	.slab_destroy		= xio_rdma_primary_pool_slab_destroy,
	.slab_init_task		= xio_rdma_primary_pool_slab_init_task,
	.slab_remap_task	= xio_rdma_primary_pool_slab_remap_task,
	.pool_post_create	= xio_rdma_primary_pool_post_create,
	.task_pre_put		= xio_rdma_task_pre_put,
};

/*---------------------------------------------------------------------------*/
/* xio_rdma_post_close							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_post_close(struct xio_transport_base *trans_base)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_base;

	if (rdma_hndl->handler_nesting) {
		rdma_hndl->state = XIO_STATE_DESTROYED;
		return;
	}
	TRACE_LOG("rdma transport: [post close] handle:%p, qp:%p\n",
		  rdma_hndl, rdma_hndl->qp);

	xio_ctx_del_delayed_work(rdma_hndl->base.ctx,
				 &rdma_hndl->timewait_timeout_work);

	xio_ctx_remove_event(rdma_hndl->base.ctx,
			     &rdma_hndl->ev_data_timewait_exit);

	xio_ctx_remove_event(rdma_hndl->base.ctx,
			     &rdma_hndl->ev_data_close);

	xio_observable_unreg_all_observers(&rdma_hndl->base.observable);

	xio_rdma_phantom_pool_destroy(rdma_hndl);

	xio_qp_release(rdma_hndl);

	if (rdma_hndl->cm_id) {
		TRACE_LOG("call rdma_destroy_id\n");
		rdma_destroy_id(rdma_hndl->cm_id);
		rdma_hndl->cm_id = NULL;
	}

	xio_cm_channel_release(rdma_hndl->cm_channel);

	xio_context_destroy_resume(rdma_hndl->base.ctx);

	if (rdma_hndl->rkey_tbl) {
		ufree(rdma_hndl->rkey_tbl);
		rdma_hndl->rkey_tbl = NULL;
	}
	if (rdma_hndl->peer_rkey_tbl) {
		ufree(rdma_hndl->peer_rkey_tbl);
		rdma_hndl->peer_rkey_tbl = NULL;
	}

	if (trans_base->portal_uri) {
		ufree(trans_base->portal_uri);
		trans_base->portal_uri = NULL;
	}

	XIO_OBSERVABLE_DESTROY(&rdma_hndl->base.observable);

	ufree(rdma_hndl);
}

/*---------------------------------------------------------------------------*/
/* on_cm_addr_resolved	                                                     */
/*---------------------------------------------------------------------------*/
static void on_cm_addr_resolved(struct rdma_cm_event *ev,
				struct xio_rdma_transport *rdma_hndl)
{
	int				retval = 0;

	rdma_hndl->dev = xio_device_lookup_init(rdma_hndl->cm_id->verbs);
	if (!rdma_hndl->dev) {
		ERROR_LOG("failed find/init device. " \
			  "rdma_hndl:%p, cm_id->verbs:%p\n", rdma_hndl,
			  rdma_hndl->cm_id->verbs);
		goto notify_err0;
	}

	if (test_bits(XIO_TRANSPORT_ATTR_TOS, &rdma_hndl->trans_attr_mask)) {
		retval = rdma_set_option(rdma_hndl->cm_id, RDMA_OPTION_ID,
					 RDMA_OPTION_ID_TOS,
					 &rdma_hndl->trans_attr.tos,
					 sizeof rdma_hndl->trans_attr.tos);
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("set TOS option failed. %m\n");
		}
		DEBUG_LOG("set TOS option success. mask:0x%x, tos:0x%x\n",
			  rdma_hndl->trans_attr_mask,
			  rdma_hndl->trans_attr.tos);
	}

	retval = rdma_resolve_route(rdma_hndl->cm_id, ROUTE_RESOLVE_TIMEOUT);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_resolve_route failed. (errno=%d %m)\n", errno);
		goto notify_err1;
	}

	return;

notify_err1:
	xio_device_put(rdma_hndl->dev);

notify_err0:
	xio_transport_notify_observer_error(&rdma_hndl->base, xio_errno());

}

/*---------------------------------------------------------------------------*/
/* on_cm_route_resolved	                                                     */
/*---------------------------------------------------------------------------*/
static void on_cm_route_resolved(struct rdma_cm_event *ev,
				 struct xio_rdma_transport *rdma_hndl)
{
	int				retval = 0;
	struct rdma_conn_param		cm_params;


	retval = xio_qp_create(rdma_hndl);
	if (retval != 0) {
		ERROR_LOG("internal logic error in create_endpoint\n");
		goto notify_err0;
	}

	memset(&cm_params, 0 , sizeof(cm_params));
	cm_params.rnr_retry_count = 3; /* 7 - infinite retry */
	cm_params.retry_count     = 3;

	/*
	 * When choosing the responder resources for a ULP, it is usually
	 * best to use the maximum value of the HCA.  If the other side is
	 * not going to use RDMA read, then it should zero out the
	 * initiator_depth in the REP, which will zero out the local
	 * responder_resources when we program the QP. Generally, the
	 * initiator_depth should be either set to 0 or
	 * min(max_qp_rd_atom, max_send_wr).  Use 0 if RDMA read is
	 * never going to be sent from this side.
	 */
	cm_params.responder_resources =
		rdma_hndl->tcq->dev->device_attr.max_qp_rd_atom;
	cm_params.initiator_depth =
		rdma_hndl->tcq->dev->device_attr.max_qp_init_rd_atom;

	/* connect to peer */
	retval = rdma_connect(rdma_hndl->cm_id, &cm_params);
	if (retval != 0) {
		xio_set_error(ENOMEM);
		DEBUG_LOG("rdma_connect failed. (errno=%d %m)\n", errno);
		goto notify_err1;
	}
	rdma_hndl->client_responder_resources = cm_params.responder_resources;
	rdma_hndl->client_initiator_depth = cm_params.initiator_depth;
	rdma_hndl->state = XIO_STATE_CONNECTING;

	return;

notify_err1:
	xio_qp_release(rdma_hndl);
notify_err0:
	xio_transport_notify_observer_error(&rdma_hndl->base, xio_errno());
}

/*---------------------------------------------------------------------------*/
/* on_cm_connect_request						     */
/*---------------------------------------------------------------------------*/
static void  on_cm_connect_request(struct rdma_cm_event *ev,
				   struct xio_rdma_transport *parent_hndl)
{
	struct xio_rdma_transport	*child_hndl;
	union xio_transport_event_data	event_data;
	int				retval = 0;
	struct rdma_cm_id		*cm_id = ev->id;
	struct xio_device		*dev;

	dev = xio_device_lookup_init(cm_id->verbs);
	if (!dev) {
		ERROR_LOG("failed find/init device\n");
		retval = rdma_reject(ev->id, NULL, 0);
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("rdma_reject failed. (errno=%d %m)\n", errno);
		}

		goto notify_err1;
	}

	child_hndl = (struct xio_rdma_transport *)xio_rdma_open(
		parent_hndl->transport,
		parent_hndl->base.ctx,
		NULL,
		0, NULL);
	if (child_hndl == NULL) {
		ERROR_LOG("failed to open rdma transport\n");
		retval = rdma_reject(ev->id, NULL, 0);
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("rdma_reject failed. (errno=%d %m)\n", errno);
		}

		goto notify_err2;
	}
	child_hndl->state = XIO_STATE_CONNECTING;

	child_hndl->cm_id	= ev->id;
	/* Parent handle i.e. listener doesn't have a CQ */
	child_hndl->tcq		= NULL;
	child_hndl->dev		= dev;
	ev->id->context		= child_hndl;
	child_hndl->client_initiator_depth =
		ev->param.conn.initiator_depth;
	child_hndl->client_responder_resources =
		ev->param.conn.responder_resources;

	/* initiator is dst, target is src */
	memcpy(&child_hndl->base.peer_addr,
	       &child_hndl->cm_id->route.addr.dst_storage,
	       sizeof(child_hndl->base.peer_addr));
	memcpy(&child_hndl->base.local_addr,
	       &child_hndl->cm_id->route.addr.src_storage,
	       sizeof(child_hndl->base.local_addr));
	child_hndl->base.proto = XIO_PROTO_RDMA;

	retval = xio_qp_create(child_hndl);
	if (retval != 0) {
		ERROR_LOG("failed to create qp\n");
		xio_rdma_reject((struct xio_transport_base *)child_hndl);
		goto notify_err3;
	}

	event_data.new_connection.child_trans_hndl =
		(struct xio_transport_base *)child_hndl;
	xio_transport_notify_observer(&parent_hndl->base,
				      XIO_TRANSPORT_NEW_CONNECTION,
				      &event_data);

	return;

notify_err3:
	xio_rdma_close((struct xio_transport_base *)child_hndl);
notify_err2:
	xio_device_put(dev);
notify_err1:
	xio_transport_notify_observer_error(&parent_hndl->base, xio_errno());
}

/*---------------------------------------------------------------------------*/
/* on_cm_refused							     */
/*---------------------------------------------------------------------------*/
static void  on_cm_refused(struct rdma_cm_event *ev,
			   struct xio_rdma_transport *rdma_hndl)
{
	DEBUG_LOG("on_cm refused. reason:%s\n",
		  xio_cm_rej_reason_str(ev->status));
	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_REFUSED, NULL);
}

/*---------------------------------------------------------------------------*/
/* on_cm_established						             */
/*---------------------------------------------------------------------------*/
static void  on_cm_established(struct rdma_cm_event *ev,
			       struct xio_rdma_transport *rdma_hndl)
{
	/* initiator is dst, target is src */
	memcpy(&rdma_hndl->base.peer_addr,
	       &rdma_hndl->cm_id->route.addr.dst_storage,
	       sizeof(rdma_hndl->base.peer_addr));
	memcpy(&rdma_hndl->base.local_addr,
	       &rdma_hndl->cm_id->route.addr.src_storage,
	       sizeof(rdma_hndl->base.local_addr));


	/* one for beacon */
	kref_get(&rdma_hndl->base.kref);
	/* one for timedwait_exit */
	kref_get(&rdma_hndl->base.kref);

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_ESTABLISHED,
				      NULL);
}

/*
 * Handle RDMA_CM_EVENT_TIMEWAIT_EXIT which is expected to be the last
 * event during the life cycle of a connection, when it had been shut down
 * and the network has cleared from the remaining in-flight messages.
*/
/*---------------------------------------------------------------------------*/
/* on_cm_timedwait_exit							     */
/*---------------------------------------------------------------------------*/
static void on_cm_timewait_exit(void *trans_hndl)
{
	struct xio_rdma_transport *rdma_hndl =
				(struct xio_rdma_transport *)trans_hndl;

	TRACE_LOG("on_cm_timedwait_exit rdma_hndl:%p\n", rdma_hndl);

	if (rdma_hndl->timewait)
		return;
	rdma_hndl->timewait++;

	xio_ctx_del_delayed_work(rdma_hndl->base.ctx,
				 &rdma_hndl->timewait_timeout_work);

	xio_rdma_flush_all_tasks(rdma_hndl);

	if (rdma_hndl->state == XIO_STATE_DISCONNECTED) {
		xio_transport_notify_observer(&rdma_hndl->base,
					      XIO_TRANSPORT_DISCONNECTED,
					      NULL);
	}

	kref_put(&rdma_hndl->base.kref, xio_rdma_close_cb);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_disoconnect							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_disconnect(struct xio_rdma_transport *rdma_hndl,
			int send_beacon)
{
	struct ibv_send_wr	*bad_wr;
	int			retval;

	retval = rdma_disconnect(rdma_hndl->cm_id);
	if (retval) {
		ERROR_LOG("rdma_hndl:%p rdma_disconnect failed, %m\n",
			  rdma_hndl);
		return -1;
	}
	if (!send_beacon)
		return 0;

	/* post an indication that all flush errors were consumed */
	retval = ibv_post_send(rdma_hndl->qp, &rdma_hndl->beacon, &bad_wr);
	if (retval) {
		ERROR_LOG("rdma_hndl %p failed to post beacon", rdma_hndl);
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_set_timewait_timer						     */
/*---------------------------------------------------------------------------*/
void xio_set_timewait_timer(struct xio_rdma_transport *rdma_hndl)
{
	int retval;
	int timeout;

	if (rdma_hndl->timewait)
		return;

	/* from context shutdown */
	if (rdma_hndl->ignore_timewait)
		timeout = XIO_TIMEWAIT_EXIT_FAST_TIMEOUT;
	else
		timeout = XIO_TIMEWAIT_EXIT_TIMEOUT;

	/* trigger the timer */
	retval = xio_ctx_add_delayed_work(
				rdma_hndl->base.ctx,
				timeout, rdma_hndl,
				on_cm_timewait_exit,
				&rdma_hndl->timewait_timeout_work);
	if (retval != 0) {
		ERROR_LOG("xio_ctx_timer_add_delayed_work failed.\n");
		return;
	}
}

/*---------------------------------------------------------------------------*/
/* on_cm_disconnected							     */
/*---------------------------------------------------------------------------*/
static void  on_cm_disconnected(struct rdma_cm_event *ev,
				struct xio_rdma_transport *rdma_hndl)
{
	int retval;

	DEBUG_LOG("on_cm_disconnected. rdma_hndl:%p, state:%d\n",
		  rdma_hndl, rdma_hndl->state);

	rdma_hndl->timewait = 0;

	switch (rdma_hndl->state) {
	case XIO_STATE_CONNECTED:
		TRACE_LOG("call to rdma_disconnect. rdma_hndl:%p\n",
			  rdma_hndl);
		rdma_hndl->state = XIO_STATE_DISCONNECTED;
		retval = xio_rdma_disconnect(rdma_hndl, 1);
		if (retval)
			ERROR_LOG("rdma_hndl:%p rdma_disconnect failed, %m\n",
				  rdma_hndl);
		break;
	case XIO_STATE_CONNECTING:
		TRACE_LOG("call to rdma_disconnect. rdma_hndl:%p\n",
			  rdma_hndl);
		rdma_hndl->state = XIO_STATE_DISCONNECTED;
		retval = xio_rdma_disconnect(rdma_hndl, 0);
		if (retval)
			ERROR_LOG("rdma_hndl:%p rdma_disconnect failed, %m\n",
				  rdma_hndl);
		/*  for beacon */
		kref_put(&rdma_hndl->base.kref, xio_rdma_close_cb);
	break;
	case XIO_STATE_CLOSED:
		/* coming here from
		 * context_shutdown/rdma_close,
		 * don't go to disconnect state
		 */
		retval = xio_rdma_disconnect(rdma_hndl, 1);
		if (retval)
			ERROR_LOG("rdma_hndl:%p rdma_disconnect failed, " \
				  "err=%d\n", rdma_hndl, retval);
	break;
	case XIO_STATE_INIT:
	case XIO_STATE_LISTEN:
	case XIO_STATE_DISCONNECTED:
	case XIO_STATE_RECONNECT:
	case XIO_STATE_DESTROYED:
	break;
	}
}

/*---------------------------------------------------------------------------*/
/* on_cm_device_release							     */
/*---------------------------------------------------------------------------*/
static void on_cm_device_release(struct rdma_cm_event *ev,
				 struct xio_rdma_transport *rdma_hndl)
{
	struct xio_device *dev;

	if (!rdma_hndl->cm_id)
		return;

	dev = xio_device_lookup(rdma_hndl->cm_id->verbs);
	if (!dev) {
		ERROR_LOG("device release, device not found\n");
		return;
	}

	xio_device_release(dev);
}

/*---------------------------------------------------------------------------*/
/* on_cm_error								     */
/*---------------------------------------------------------------------------*/
static void on_cm_error(struct rdma_cm_event *ev,
			struct xio_rdma_transport *rdma_hndl)
{
	int	reason;

	DEBUG_LOG("rdma transport [error] %s, hndl:%p\n",
		  rdma_event_str(ev->event), rdma_hndl);

	switch (ev->event) {
	case RDMA_CM_EVENT_CONNECT_ERROR:
		reason = XIO_E_CONNECT_ERROR;
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
		reason = XIO_E_ADDR_ERROR;
		break;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		reason = XIO_E_ROUTE_ERROR;
		break;
	case RDMA_CM_EVENT_UNREACHABLE:
		reason = XIO_E_UNREACHABLE;
		break;
	default:
		reason = XIO_E_NOT_SUPPORTED;
		break;
	};
	xio_transport_notify_observer_error(&rdma_hndl->base, reason);
}

/*---------------------------------------------------------------------------*/
/* xio_close_handler							     */
/*---------------------------------------------------------------------------*/
void xio_close_handler(void *hndl)
{
	xio_rdma_post_close((struct xio_transport_base *)hndl);


}

/*---------------------------------------------------------------------------*/
/* xio_handle_cm_event							     */
/*---------------------------------------------------------------------------*/
static void xio_handle_cm_event(struct rdma_cm_event *ev,
				struct xio_rdma_transport *rdma_hndl)
{
	DEBUG_LOG("cm event: [%s], hndl:%p, status:\n",
		  rdma_event_str(ev->event), rdma_hndl, ev->status);

	rdma_hndl->handler_nesting++;
	switch (ev->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		on_cm_addr_resolved(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		on_cm_route_resolved(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		on_cm_connect_request(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		on_cm_established(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_REJECTED:
		on_cm_refused(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_ADDR_CHANGE:
	case RDMA_CM_EVENT_DISCONNECTED:
		on_cm_disconnected(ev, rdma_hndl);
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		/* The caller of this callback i.e. cma_ib_handler is holding
		 * cma_disable_callback, thus rdma_destroy_id should not
		 * be called in xio_rdma_close_complete! this is prevented as
		 * rdma_hndl->handler_nesting > 0. We return one to ensure that
		 * cma_ib_handler will call
		 */
		xio_ctx_init_event(&rdma_hndl->ev_data_timewait_exit,
				   on_cm_timewait_exit, (void *)rdma_hndl);

		xio_ctx_add_event(rdma_hndl->base.ctx,
				  &rdma_hndl->ev_data_timewait_exit);
		break;

	case RDMA_CM_EVENT_MULTICAST_JOIN:
	case RDMA_CM_EVENT_MULTICAST_ERROR:
		ERROR_LOG("Unrelated event:%d, %s - ignored\n", ev->event,
			  rdma_event_str(ev->event));
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		on_cm_device_release(ev, rdma_hndl);
		break;

	case RDMA_CM_EVENT_CONNECT_RESPONSE:
		break;

	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	default:
		on_cm_error(ev, rdma_hndl);
		break;
	};
	rdma_hndl->handler_nesting--;

	/* state can be modified to destroyed (side effect) */
	if (rdma_hndl->state == XIO_STATE_DESTROYED) {
		/* user space code calls here, xio_rdma_post_close which may
		 * call rdma_destroy_id which is not allowed in an handler
		 */
		xio_ctx_init_event(&rdma_hndl->ev_data_close,
				   xio_close_handler, (void *)rdma_hndl);

		/* tell "poller mechanism" */
		xio_ctx_add_event(rdma_hndl->base.ctx,
				  &rdma_hndl->ev_data_close);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_cma_handler							     */
/*---------------------------------------------------------------------------*/
static void xio_cma_handler(int fd, int events, void *user_context)
{
	struct rdma_event_channel	*p_cm_channel =
		(struct rdma_event_channel *)(user_context);
	struct rdma_cm_event		*ev, lev;
	struct xio_rdma_transport	*rdma_hndl;
	int				retval;

	do {
		/* get the event */
		retval = rdma_get_cm_event(p_cm_channel, &ev);
		if (retval) {
			if (errno == EAGAIN)
				break;
			xio_set_error(errno);
			ERROR_LOG("rdma_get_cm_event failed. " \
					"(errno=%d %m)\n", errno);
			break;
		}

		rdma_hndl = (struct xio_rdma_transport *)ev->id->context;

		lev = *ev;

		/* ack the event */
		rdma_ack_cm_event(ev);

		/* and handle it */
		xio_handle_cm_event(&lev, rdma_hndl);
	} while (1);
}

/*---------------------------------------------------------------------------*/
/* xio_cm_channel_get							     */
/*---------------------------------------------------------------------------*/
static struct xio_cm_channel *xio_cm_channel_get(struct xio_context *ctx)
{
	struct xio_cm_channel	*channel;
	int			retval;

	pthread_rwlock_rdlock(&cm_lock);
	list_for_each_entry(channel, &cm_list, channels_list_entry) {
		if (channel->ctx == ctx) {
			pthread_rwlock_unlock(&cm_lock);
			kref_get(&channel->kref);
			return channel;
		}
	}
	pthread_rwlock_unlock(&cm_lock);

	channel = (struct xio_cm_channel *)
			ucalloc(1, sizeof(struct xio_cm_channel));
	if (!channel) {
		ERROR_LOG("rdma_create_event_channel failed " \
				"(errno=%d %m)\n", errno);
		return NULL;
	}

	channel->cm_channel = rdma_create_event_channel();
	if (!channel->cm_channel) {
		ERROR_LOG("rdma_create_event_channel failed " \
				"(errno=%d %m)\n", errno);
		goto free;
	}
	/* turn the file descriptor to non blocking */
	retval = fcntl(channel->cm_channel->fd, F_GETFL, 0);
	if (retval != -1) {
		retval = fcntl(channel->cm_channel->fd, F_SETFL,
			       retval | O_NONBLOCK);
	}
	if (retval == -1) {
		xio_set_error(errno);
		ERROR_LOG("fcntl failed. (errno=%d %m)\n", errno);
		goto cleanup;
	}

	retval = xio_context_add_ev_handler(
			ctx,
			channel->cm_channel->fd,
			XIO_POLLIN,
			xio_cma_handler,
			channel->cm_channel);
	if (retval != 0) {
		xio_set_error(errno);
		ERROR_LOG("Adding to event loop failed (errno=%d %m)\n",
			  errno);
		goto cleanup;
	}
	channel->ctx = ctx;

	pthread_rwlock_wrlock(&cm_lock);
	list_add(&channel->channels_list_entry, &cm_list);
	pthread_rwlock_unlock(&cm_lock);

	/* One reference count for the rdma handle */
	kref_init(&channel->kref);

	return channel;

cleanup:
	rdma_destroy_event_channel(channel->cm_channel);
free:
	ufree(channel);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_open		                                             */
/*---------------------------------------------------------------------------*/
static struct xio_transport_base *xio_rdma_open(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer,
		uint32_t		trans_attr_mask,
		struct xio_transport_init_attr *attr)
{
	struct xio_rdma_transport	*rdma_hndl;
	int				max_xio_hdr;


	/*allocate rdma handle */
	rdma_hndl = (struct xio_rdma_transport *)
			ucalloc(1, sizeof(struct xio_rdma_transport));
	if (!rdma_hndl) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		return NULL;
	}
	if (attr && trans_attr_mask) {
		memcpy(&rdma_hndl->trans_attr, attr, sizeof *attr);
		rdma_hndl->trans_attr_mask = trans_attr_mask;
	}

	XIO_OBSERVABLE_INIT(&rdma_hndl->base.observable, rdma_hndl);

	if (rdma_options.enable_mem_pool) {
		rdma_hndl->rdma_mempool =
			xio_transport_mempool_get(ctx, 1);
		if (rdma_hndl->rdma_mempool == NULL) {
			xio_set_error(ENOMEM);
			ERROR_LOG("allocating rdma mempool failed. %m\n");
			goto cleanup;
		}
	}
	max_xio_hdr = xio_rdma_get_max_header_size();
	max_xio_hdr = ALIGN(max_xio_hdr, 64);

	rdma_hndl->base.portal_uri	= NULL;
	rdma_hndl->base.proto		= XIO_PROTO_RDMA;
	kref_init(&rdma_hndl->base.kref);
	rdma_hndl->transport		= transport;
	rdma_hndl->cm_id		= NULL;
	rdma_hndl->qp			= NULL;
	rdma_hndl->tcq			= NULL;
	rdma_hndl->base.ctx		= ctx;
	rdma_hndl->rq_depth		= MAX_RECV_WR;
	rdma_hndl->sq_depth		= MAX_SEND_WR;
	rdma_hndl->peer_credits		= 0;
	rdma_hndl->cm_channel		= xio_cm_channel_get(ctx);
	rdma_hndl->max_inline_buf_sz	= max_xio_hdr +
					  g_options.max_inline_hdr +
					  g_options.max_inline_data;
	rdma_hndl->max_inline_buf_sz	=
				ALIGN(rdma_hndl->max_inline_buf_sz, 1024);

	/*
	DEBUG_LOG("max_inline_buf:%d\n", rdma_hndl->max_inline_buf_sz);
	*/
	if (!rdma_hndl->cm_channel) {
		TRACE_LOG("rdma transport: failed to allocate cm_channel\n");
		goto cleanup;
	}
	if (observer)
		xio_observable_reg_observer(&rdma_hndl->base.observable,
					    observer);

	INIT_LIST_HEAD(&rdma_hndl->in_flight_list);
	INIT_LIST_HEAD(&rdma_hndl->rdma_rd_in_flight_list);
	INIT_LIST_HEAD(&rdma_hndl->tx_ready_list);
	INIT_LIST_HEAD(&rdma_hndl->tx_comp_list);
	INIT_LIST_HEAD(&rdma_hndl->rx_list);
	INIT_LIST_HEAD(&rdma_hndl->io_list);
	INIT_LIST_HEAD(&rdma_hndl->rdma_rd_list);

	TRACE_LOG("xio_rdma_open: [new] handle:%p\n", rdma_hndl);

	return (struct xio_transport_base *)rdma_hndl;

cleanup:
	if (rdma_hndl->cm_channel)
		xio_cm_channel_release(rdma_hndl->cm_channel);

	ufree(rdma_hndl);

	return NULL;
}



/*
 * Start closing connection. Transfer IB QP to error state.
 * This will be followed by WC error and buffers flush events.
 * We also should expect DISCONNECTED and TIMEWAIT_EXIT events.
 * Only after the draining is over we are sure to have reclaimed
 * all buffers (and tasks). After the RDMA CM events are collected,
 * the connection QP may be destroyed, and its number may be recycled.
 */
/*---------------------------------------------------------------------------*/
/* xio_rdma_close_cb		                                             */
/*---------------------------------------------------------------------------*/
void xio_rdma_close_cb(struct kref *kref)
{
	struct xio_transport_base *transport = container_of(
					kref, struct xio_transport_base, kref);
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;

	xio_transport_notify_observer(
				transport,
				XIO_TRANSPORT_CLOSED,
				NULL);

	xio_rdma_post_close((struct xio_transport_base *)rdma_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_close		                                             */
/*---------------------------------------------------------------------------*/
static void xio_rdma_close(struct xio_transport_base *transport)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	int	retval;

	/* now it is zero */
	DEBUG_LOG("xio_rmda_close: [close] handle:%p, qp:%p state:%s\n",
		  rdma_hndl, rdma_hndl->qp,
		  xio_transport_state_str(rdma_hndl->state));


	switch (rdma_hndl->state) {
	case XIO_STATE_LISTEN:
		rdma_hndl->state = XIO_STATE_CLOSED;
		break;
	case XIO_STATE_CONNECTED:
		TRACE_LOG("call to rdma_disconnect. rdma_hndl:%p\n",
			  rdma_hndl);

		rdma_hndl->state = XIO_STATE_CLOSED;

		retval = xio_rdma_disconnect(rdma_hndl, 0);
		if (retval)
			DEBUG_LOG("handle:%p rdma_disconnect failed, " \
				  "%m\n", rdma_hndl);

		break;
	case XIO_STATE_DISCONNECTED:
		rdma_hndl->state = XIO_STATE_CLOSED;

		if (rdma_hndl->ignore_timewait && rdma_hndl->timewait == 0) {
			xio_ctx_del_delayed_work(rdma_hndl->base.ctx,
						 &rdma_hndl->timewait_timeout_work);
			xio_set_timewait_timer(rdma_hndl);
		}
		break;
	case XIO_STATE_CLOSED:
		return;
		break;
	default:
		rdma_hndl->state = XIO_STATE_CLOSED;
		break;
	}

	kref_put(&transport->kref, xio_rdma_close_cb);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_dup2		                                             */
/* makes new_trans_hndl be the copy of old_trans_hndl, closes new_trans_hndl */
/* Note old and new are in dup2 terminology opposite to reconnect terms	     */
/* --------------------------------------------------------------------------*/
static int xio_rdma_dup2(struct xio_transport_base *old_trans_hndl,
			 struct xio_transport_base **new_trans_hndl)
{
	int ret = 0;

	struct xio_rdma_transport *old_hndl =
		(struct xio_rdma_transport *)old_trans_hndl;
	struct xio_rdma_transport *new_hndl =
		(struct xio_rdma_transport *)*new_trans_hndl;

	/* if device is not the same an R_KEY replacement table is created */
	if (old_hndl->tcq->dev != new_hndl->tcq->dev) {
		/* new is actually the old one we want to replace */
		ret = xio_rkey_table_create(new_hndl->tcq->dev,
					    old_hndl->tcq->dev,
					    &old_hndl->rkey_tbl,
					    &old_hndl->rkey_tbl_size);
		if (ret) {
			ERROR_LOG("rkey table creation failed\n");
			return -1;
		}
	}

	xio_rdma_close(*new_trans_hndl);

	/* nexus layer will call close which will only decrement */
	kref_get(&old_trans_hndl->kref);
	*new_trans_hndl = old_trans_hndl;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_new_rkey			                                             */
/*---------------------------------------------------------------------------*/

static int xio_new_rkey(struct xio_rdma_transport *rdma_hndl, uint32_t *key)
{
	int i;

	if (!*key)
		return 0;

	for (i = 0; i < rdma_hndl->peer_rkey_tbl_size; i++) {
		if (rdma_hndl->peer_rkey_tbl[i].old_rkey == *key) {
			*key = rdma_hndl->peer_rkey_tbl[i].new_rkey;
			return 0;
		}
	}
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_update_task		                                             */
/*---------------------------------------------------------------------------*/
static int xio_rdma_update_task(struct xio_transport_base *trans_hndl,
				struct xio_task *task)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_hndl;
	XIO_TO_RDMA_TASK(task, rdma_task);
	unsigned int i;

	for (i = 0; i < rdma_task->req_recv_num_sge; i++) {
		if (xio_new_rkey(rdma_hndl, &rdma_task->req_recv_sge[i].stag))
			return -1;
	}

	for (i = 0; i < rdma_task->req_read_num_sge; i++) {
		if (xio_new_rkey(rdma_hndl, &rdma_task->req_read_sge[i].stag))
			return -1;
	}

	for (i = 0; i < rdma_task->write_num_sge; i++) {
		if (xio_new_rkey(rdma_hndl, &rdma_task->req_write_sge[i].stag))
			return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_accept		                                             */
/*---------------------------------------------------------------------------*/
static int xio_rdma_accept(struct xio_transport_base *transport)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	int				retval;
	struct rdma_conn_param		cm_params;

	memset(&cm_params, 0, sizeof(cm_params));
	/*
	 * Limit the responder resources requested by the remote
	 * to our capabilities.  Note that the kernel swaps
	 * req->responder_resources and req->initiator_depth, so
	 * that req->responder_resources is actually the active
	 * side's initiator depth.
	 */
	if (rdma_hndl->client_responder_resources >
	    rdma_hndl->tcq->dev->device_attr.max_qp_rd_atom)
		cm_params.responder_resources =
			rdma_hndl->tcq->dev->device_attr.max_qp_rd_atom;
	else
		cm_params.responder_resources =
			rdma_hndl->client_responder_resources;

	/*
	 * Note: if this side of the connection is never going to
	 * use RDMA read operations, then initiator_depth can be set
	 * to 0 here.
	 */
	if (rdma_hndl->client_initiator_depth >
	    rdma_hndl->tcq->dev->device_attr.max_qp_init_rd_atom)
		cm_params.initiator_depth =
			rdma_hndl->tcq->dev->device_attr.max_qp_init_rd_atom;
	else
		cm_params.initiator_depth = rdma_hndl->client_initiator_depth;

	/* "accept" the connection */
	retval = rdma_accept(rdma_hndl->cm_id, &cm_params);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_accept failed. (errno=%d %m)\n", errno);
		return -1;
	}
	rdma_hndl->client_responder_resources = cm_params.responder_resources;
	rdma_hndl->client_initiator_depth = cm_params.initiator_depth;

	TRACE_LOG("rdma transport: [accept] handle:%p\n", rdma_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_reject		                                             */
/*---------------------------------------------------------------------------*/
static int xio_rdma_reject(struct xio_transport_base *transport)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	int				retval;

	/* "reject" the connection */
	retval = rdma_reject(rdma_hndl->cm_id, NULL, 0);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_reject failed. (errno=%d %m)\n", errno);
		return -1;
	}
	DEBUG_LOG("rdma transport: [reject] handle:%p\n", rdma_hndl);

	return 0;
}

static int xio_rdma_do_connect(struct xio_transport_base *trans_hndl,
			       const char *out_if_addr)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_hndl;
	union xio_sockaddr		sa;
	int				retval = 0;

	/* resolve the portal_uri */
	if (xio_uri_to_ss(trans_hndl->portal_uri, &sa.sa_stor) == -1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n",
			  trans_hndl->portal_uri);
		return -1;
	}

	/* create cm id */
	retval = rdma_create_id(rdma_hndl->cm_channel->cm_channel,
				&rdma_hndl->cm_id,
				rdma_hndl, RDMA_PS_TCP);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("rdma_create id failed. (errno=%d %m)\n", errno);
		goto exit1;
	}

	if (out_if_addr) {
		union xio_sockaddr if_sa;

		if (xio_host_port_to_ss(out_if_addr,
					&if_sa.sa_stor) == -1) {
			xio_set_error(XIO_E_ADDR_ERROR);
			ERROR_LOG("outgoing interface [%s] resolving failed\n",
				  out_if_addr);
			goto exit2;
		}
		retval = rdma_bind_addr(rdma_hndl->cm_id, &if_sa.sa);
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("rdma_bind_addr failed. (errno=%d %m)\n",
				  errno);
			goto exit2;
		}
	}
	retval = rdma_resolve_addr(rdma_hndl->cm_id, NULL, &sa.sa,
				   ADDR_RESOLVE_TIMEOUT);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("rdma_resolve_addr failed. (errno=%d %m)\n", errno);
		goto exit2;
	}

	return 0;

exit2:
	TRACE_LOG("call rdma_destroy_id\n");
	rdma_destroy_id(rdma_hndl->cm_id);
exit1:
	rdma_hndl->cm_id = NULL;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_connect		                                             */
/*---------------------------------------------------------------------------*/
static int xio_rdma_connect(struct xio_transport_base *trans_hndl,
			    const char *portal_uri, const char *out_if_addr)
{
	trans_hndl->is_client = 1;

	if (!portal_uri) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n", portal_uri);
		goto exit1;
	}

	/* allocate memory for portal_uri */
	trans_hndl->portal_uri = strdup(portal_uri);
	if (trans_hndl->portal_uri == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("calloc failed. %m\n");
		goto exit1;
	}

	if (xio_rdma_do_connect(trans_hndl, out_if_addr) < 0)
		goto exit2;

	return 0;

exit2:
	free(trans_hndl->portal_uri);
	trans_hndl->portal_uri = NULL;

exit1:
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_listen							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_listen(struct xio_transport_base *transport,
			   const char *portal_uri,
			   uint16_t *src_port, int backlog)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	union xio_sockaddr	sa;
	int			retval = 0;
	uint16_t		sport;

	/* resolve the portal_uri */
	if (xio_uri_to_ss(portal_uri, &sa.sa_stor) == -1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		DEBUG_LOG("address [%s] resolving failed\n", portal_uri);
		return -1;
	}
	rdma_hndl->base.is_client = 0;
	/*is_server = 1; */

	/* create cm id */
	retval = rdma_create_id(rdma_hndl->cm_channel->cm_channel,
				&rdma_hndl->cm_id,
				rdma_hndl, RDMA_PS_TCP);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_create id failed. (errno=%d %m)\n", errno);
		goto exit1;
	}

	retval = rdma_bind_addr(rdma_hndl->cm_id, &sa.sa);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_bind_addr failed. (errno=%d %m)\n", errno);
		goto exit2;
	}

	/* 0 == maximum backlog */
	retval  = rdma_listen(rdma_hndl->cm_id, backlog);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("rdma_listen failed. (errno=%d %m)\n", errno);
		goto exit2;
	}

	sport = ntohs(rdma_get_src_port(rdma_hndl->cm_id));
	if (src_port)
		*src_port = sport;

	rdma_hndl->state = XIO_STATE_LISTEN;
	DEBUG_LOG("listen on [%s] src_port:%d\n", portal_uri, sport);

	return 0;

exit2:
	TRACE_LOG("call rdma_destroy_id\n");
	rdma_destroy_id(rdma_hndl->cm_id);
exit1:
	rdma_hndl->cm_id = NULL;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_enable_fork_support                                              */
/*---------------------------------------------------------------------------*/
static int xio_rdma_enable_fork_support(void)
{
	int   retval;
	setenv("RDMAV_FORK_SAFE", "YES", 1);
	setenv("RDMAV_HUGEPAGES_SAFE", "YES", 1);
	retval = ibv_fork_init();
	if (retval) {
		ERROR_LOG("ibv_fork_init failed (errno=%d %s)\n",
			  retval, strerror(retval));
		xio_set_error(errno);
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_set_opt                                                          */
/*---------------------------------------------------------------------------*/
static int xio_rdma_set_opt(void *xio_obj,
			    int optname, const void *optval, int optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		VALIDATE_SZ(sizeof(int));
		rdma_options.enable_mem_pool = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		VALIDATE_SZ(sizeof(int));
		rdma_options.enable_dma_latency = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		rdma_options.max_in_iovsz = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		rdma_options.max_out_iovsz = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_ENABLE_FORK_INIT:
		return xio_rdma_enable_fork_support();
		break;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_get_opt                                                          */
/*---------------------------------------------------------------------------*/
static int xio_rdma_get_opt(void  *xio_obj,
			    int optname, void *optval, int *optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		*((int *)optval) = rdma_options.enable_mem_pool;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		*((int *)optval) = rdma_options.enable_dma_latency;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		*((int *)optval) = rdma_options.max_in_iovsz;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		*((int *)optval) = rdma_options.max_out_iovsz;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_RDMA_NUM_DEVICES:
		*((int *)optval) = rdma_num_devices;
		*optlen = sizeof(int);
		return 0;
		break;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_modify						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_transport_modify(struct xio_transport_base *trans_hndl,
				     struct xio_transport_attr *attr,
				     int attr_mask)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_hndl;
	int	ret;
	int     modified = 0;

	if (test_bits(XIO_TRANSPORT_ATTR_TOS, &attr_mask)) {
		ret = rdma_set_option(rdma_hndl->cm_id, RDMA_OPTION_ID,
				      RDMA_OPTION_ID_TOS,
				      &attr->tos, sizeof attr->tos);
		if (ret) {
			ERROR_LOG("set TOS option failed. %m\n");
			xio_set_error(errno);
			return -1;
		}
		set_bits(XIO_TRANSPORT_ATTR_TOS, &rdma_hndl->trans_attr_mask);
		rdma_hndl->trans_attr.tos = attr->tos;
		modified = 1;
	}

	if (modified)
		return 0;

	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_query						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_transport_query(struct xio_transport_base *trans_hndl,
				    struct xio_transport_attr *attr,
				    int attr_mask)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_hndl;
	int     queried = 0;

	if (test_bits(XIO_TRANSPORT_ATTR_TOS, &attr_mask)) {
		if (test_bits(XIO_TRANSPORT_ATTR_TOS,
			      &rdma_hndl->trans_attr_mask)) {
			attr->tos = rdma_hndl->trans_attr.tos;
			queried = 1;
		}
		else
			goto not_supported;
	}

	if (queried)
		return 0;

not_supported:
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*
 * To dynamically control C-states, open the file /dev/cpu_dma_latency and
 * write the maximum allowable latency to it. This will prevent C-states with
 * transition latencies higher than the specified value from being used, as
 * long as the file /dev/cpu_dma_latency is kept open.
 * Writing a maximum allowable latency of 0 will keep the processors in C0
 * (like using kernel parameter ―idle=poll), and writing 1 should force
 * the processors to C1 when idle. Higher values could also be written to
 * restrict the use of C-states with latency greater than the value written.
 *
 * http://en.community.dell.com/techcenter/extras/m/white_papers/20227764/download.aspx
 */

/*---------------------------------------------------------------------------*/
/* xio_set_cpu_latency							     */
/*---------------------------------------------------------------------------*/
static int xio_set_cpu_latency(int *fd)
{
	int32_t latency = 0;

	if (!rdma_options.enable_dma_latency)
		return 0;

	DEBUG_LOG("setting latency to %d us\n", latency);
	*fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (*fd < 0) {
		ERROR_LOG(
		 "open /dev/cpu_dma_latency %m - need root permissions\n");
		return -1;
	}
	if (write(*fd, &latency, sizeof(latency)) != sizeof(latency)) {
		ERROR_LOG(
		 "write to /dev/cpu_dma_latency %m - need root permissions\n");
		close(*fd);
		*fd = -1;
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_init							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_init(void)
{
	int	retval = 0;

	INIT_LIST_HEAD(&cm_list);

	spin_lock_init(&mngmt_lock);
	pthread_rwlock_init(&dev_lock, NULL);
	pthread_rwlock_init(&cm_lock, NULL);

	/* set cpu latency until process is down */
	xio_set_cpu_latency(&cdl_fd);

	retval = xio_device_thread_init();
	if (retval != 0) {
		ERROR_LOG("Failed to initialize devices thread\n");
		return;
	}

	retval = xio_device_list_init();
	if (retval != 0) {
		ERROR_LOG("Failed to initialize device list\n");
		return;
	}

	/* storage for all memory registrations */
	xio_mr_list_init();
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_init						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_transport_init(struct xio_transport *transport)
{
	pthread_once(&ctor_key_once, xio_rdma_init);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_release							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_release(void)
{
	if (cdl_fd >= 0)
		close(cdl_fd);

	/* free all redundant registered memory */
	xio_mr_list_free();

	xio_device_thread_stop();

	/* free devices */
	xio_device_list_release();

	if (!list_empty(&cm_list))
		ERROR_LOG("cm_channel memory leakage\n");

	pthread_rwlock_destroy(&dev_lock);
	pthread_rwlock_destroy(&cm_lock);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_release		                                     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_transport_release(struct xio_transport *transport)
{
	if (ctor_key_once == PTHREAD_ONCE_INIT)
		return;

	pthread_once(&dtor_key_once, xio_rdma_release);
}


/*---------------------------------------------------------------------------*/
/* xio_is_valid_in_req							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_is_valid_in_req(struct xio_msg *msg)
{
	struct xio_vmsg		*vmsg = &msg->in;
	struct xio_sg_table_ops	*sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	int32_t			nents, max_nents;
	unsigned int		i;
	int			mr_found = 0;

	sgtbl		= xio_sg_table_get(vmsg);
	sgtbl_ops	= (struct xio_sg_table_ops *)
				xio_sg_table_ops_get(vmsg->sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > rdma_options.max_in_iovsz) ||
	    (nents > max_nents))
		return 0;

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if ((vmsg->header.iov_base != NULL)  &&
	    (vmsg->header.iov_len == 0))
		return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if (sge_addr(sgtbl_ops, sge) == NULL) {
			if (sge_mr(sgtbl_ops, sge))
				return 0;
		} else {
			if (sge_length(sgtbl_ops, sge)  == 0)
				return 0;
		}
	}
	if (mr_found != nents && mr_found)
		return 0;

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_is_valid_out_msg						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_is_valid_out_msg(struct xio_msg *msg)
{
	struct xio_vmsg		*vmsg = &msg->out;
	struct xio_sg_table_ops *sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	int32_t			nents, max_nents;
	unsigned int		i;
	int			mr_found = 0;


	sgtbl		= xio_sg_table_get(&msg->out);
	sgtbl_ops	= (struct xio_sg_table_ops *)
				xio_sg_table_ops_get(msg->out.sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > rdma_options.max_out_iovsz) ||
	    (nents > max_nents))
		return 0;

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if ((vmsg->header.iov_base == NULL)  &&
	    (vmsg->header.iov_len != 0))
		return 0;

	if (vmsg->header.iov_len > (size_t)g_options.max_inline_hdr)
		return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if ((sge_addr(sgtbl_ops, sge) == NULL) ||
		    (sge_length(sgtbl_ops, sge)  == 0))
			return 0;
	}

	if (mr_found != nents && mr_found)
		return 0;

	return 1;
}

/* task pools management */
/*---------------------------------------------------------------------------*/
/* xio_rdma_get_pools_ops						     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_get_pools_ops(
			struct xio_transport_base *trans_hndl,
			struct xio_tasks_pool_ops **initial_pool_ops,
			struct xio_tasks_pool_ops **primary_pool_ops)
{
	*initial_pool_ops = &initial_tasks_pool_ops;
	*primary_pool_ops = &primary_tasks_pool_ops;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_set_pools_cls						     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_set_pools_cls(
			struct xio_transport_base *trans_hndl,
			struct xio_tasks_pool_cls *initial_pool_cls,
			struct xio_tasks_pool_cls *primary_pool_cls)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)trans_hndl;

	if (initial_pool_cls)
		rdma_hndl->initial_pool_cls = *initial_pool_cls;
	if (primary_pool_cls)
		rdma_hndl->primary_pool_cls = *primary_pool_cls;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_constructor					     */
/*---------------------------------------------------------------------------*/
void xio_rdma_transport_constructor(void)
{
	/* Mellanox OFED's User Manual */
	setenv("RDMAV_HUGEPAGES_SAFE", "1", 0);
	setenv("MLX_QP_ALLOC_TYPE", "PREFER_CONTIG", 0);
	setenv("MLX_CQ_ALLOC_TYPE", "PREFER_CONTIG", 0);


	/* Mellanox OFED's User Manual */
	/*
	setenv("MLX_QP_ALLOC_TYPE","PREFER_CONTIG", 1);
	setenv("MLX_CQ_ALLOC_TYPE","ALL", 1);
	setenv("MLX_MR_ALLOC_TYPE","ALL", 1);
	*/
	if (0)
		xio_rdma_enable_fork_support();

	xio_device_list_check();

	spin_lock_init(&dev_list_lock);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_transport_destructor					     */
/*---------------------------------------------------------------------------*/
void xio_rdma_transport_destructor(void)
{
	ctor_key_once = PTHREAD_ONCE_INIT;
	dtor_key_once = PTHREAD_ONCE_INIT;
}

struct xio_transport xio_rdma_transport = {
	.name			= "rdma",
	.ctor			= xio_rdma_transport_constructor,
	.dtor			= xio_rdma_transport_destructor,
	.init			= xio_rdma_transport_init,
	.release		= xio_rdma_transport_release,
	.context_shutdown	= xio_rdma_context_shutdown,
	.open			= xio_rdma_open,
	.connect		= xio_rdma_connect,
	.listen			= xio_rdma_listen,
	.accept			= xio_rdma_accept,
	.reject			= xio_rdma_reject,
	.close			= xio_rdma_close,
	.dup2			= xio_rdma_dup2,
	.update_task		= xio_rdma_update_task,
	.send			= xio_rdma_send,
	.poll			= xio_rdma_poll,
	.set_opt		= xio_rdma_set_opt,
	.get_opt		= xio_rdma_get_opt,
	.cancel_req		= xio_rdma_cancel_req,
	.cancel_rsp		= xio_rdma_cancel_rsp,
	.get_pools_setup_ops	= xio_rdma_get_pools_ops,
	.set_pools_cls		= xio_rdma_set_pools_cls,
	.modify			= xio_rdma_transport_modify,
	.query			= xio_rdma_transport_query,

	.validators_cls.is_valid_in_req  = xio_rdma_is_valid_in_req,
	.validators_cls.is_valid_out_msg = xio_rdma_is_valid_out_msg,
};

/*---------------------------------------------------------------------------*/
/* xio_rdma_get_transport_func_list						     */
/*---------------------------------------------------------------------------*/
struct xio_transport *  xio_rdma_get_transport_func_list() {
	return &xio_rdma_transport;
}
