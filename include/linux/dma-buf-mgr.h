/*
 * Header file for dma buffer sharing framework.
 *
 * Copyright(C) 2011 Linaro Limited. All rights reserved.
 * Author: Sumit Semwal <sumit.semwal@ti.com>
 *
 * Many thanks to linaro-mm-sig list, and specially
 * Arnd Bergmann <arnd@arndb.de>, Rob Clark <rob@ti.com> and
 * Daniel Vetter <daniel@ffwll.ch> for their support in creation and
 * refining of this idea.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __DMA_BUF_MGR_H__
#define __DMA_BUF_MGR_H__

#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/list.h>

/**
 * struct dmabufmgr_validate - reservation structure for a dma-buf
 * @head:	list entry
 * @refcount:	refcount
 * @reserved:	internal use: signals if reservation is succesful
 * @shared:	whether shared or exclusive access was requested
 * @bo:		pointer to a dma-buf to reserve
 * @priv:	pointer to user-specific data
 * @num_fences:	number of fences to wait on
 * @num_waits:	amount of waits queued
 * @fences:	fences to wait on
 * @wait:	dma_fence_cb that can be passed to dma_fence_add_callback
 *
 * Based on struct ttm_validate_buffer, but unrecognisably modified.
 * num_fences and fences are only valid after dmabufmgr_reserve_buffers
 * is called.
 */
struct dmabufmgr_validate {
	struct list_head head;
	struct kref refcount;

	bool reserved;
	bool shared;
	struct dma_buf *bo;
	void *priv;

	unsigned num_fences, num_waits;
	struct dma_fence *fences[DMA_BUF_MAX_SHARED_FENCE];
	struct dma_fence_cb wait[DMA_BUF_MAX_SHARED_FENCE];
};

/**
 * dmabufmgr_validate_init - initialize a dmabufmgr_validate struct
 * @val:	[in]	pointer to dmabufmgr_validate
 * @list:	[in]	pointer to list to append val to
 * @bo:		[in]	pointer to dma-buf
 * @priv:	[in]	pointer to user-specific data
 * @shared:	[in]	request shared or exclusive access
 *
 * Initialize a struct dmabufmgr_validate for use with dmabufmgr methods.
 * It sets the refcount to 1, and appends it to the list.
 */
static inline void
dmabufmgr_validate_init(struct dmabufmgr_validate *val,
			struct list_head *list, struct dma_buf *bo,
			void *priv, bool shared)
{
	kref_init(&val->refcount);
	list_add_tail(&val->head, list);
	val->bo = bo;
	val->priv = priv;
	val->shared = shared;
}

extern void dmabufmgr_validate_free(struct kref *ref);

/**
 * dmabufmgr_validate_get - increase refcount on a dmabufmgr_validate
 * @val:	[in]	pointer to dmabufmgr_validate
 */
static inline struct dmabufmgr_validate *
dmabufmgr_validate_get(struct dmabufmgr_validate *val)
{
	kref_get(&val->refcount);
	return val;
}

/**
 * dmabufmgr_validate_put - decrease refcount on a dmabufmgr_validate
 * @val:	[in]	pointer to dmabufmgr_validate
 *
 * Returns true if the caller removed last refcount on val,
 * false otherwise. This is a convenience function that destroys
 * dmabufmgr_validate with dmabufmgr_validate_free if refcount
 * falls to 0. If there are custom actions needed on freeing,
 * this function doesn't have to be used.
 *
 * Other dmabufmgr_* methods will not touch the refcount.
 */
static inline bool
dmabufmgr_validate_put(struct dmabufmgr_validate *val)
{
	return kref_put(&val->refcount, dmabufmgr_validate_free);
}

extern int
dmabufmgr_reserve_buffers(struct list_head *list);

extern void
dmabufmgr_backoff_reservation(struct list_head *list);

extern void
dmabufmgr_fence_buffer_objects(struct dma_fence *fence, struct list_head *list);

extern long
dmabufmgr_wait_timeout(struct list_head *list, bool intr, signed long timeout);

#endif /* __DMA_BUF_MGR_H__ */
