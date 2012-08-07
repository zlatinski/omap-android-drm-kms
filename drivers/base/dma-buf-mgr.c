/*
 * Copyright (C) 2012 Canonical Ltd
 *
 * Based on ttm_bo.c which bears the following copyright notice,
 * but is dual licensed:
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */


#include <linux/dma-buf-mgr.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/slab.h>

static void dmabufmgr_backoff_reservation_locked(struct list_head *list)
{
	struct dmabufmgr_validate *entry;

	list_for_each_entry(entry, list, head) {
		struct dma_buf *bo = entry->bo;
		if (!entry->reserved)
			continue;
		entry->reserved = false;

		entry->num_fences = 0;

		atomic_set(&bo->reserved, 0);
		wake_up_all(&bo->event_queue);
	}
}

static int
dmabufmgr_wait_unreserved_locked(struct list_head *list,
				    struct dma_buf *bo)
{
	int ret;

	spin_unlock(&dma_buf_reserve_lock);
	ret = dma_buf_wait_unreserved(bo, true);
	spin_lock(&dma_buf_reserve_lock);
	if (unlikely(ret != 0))
		dmabufmgr_backoff_reservation_locked(list);
	return ret;
}

/**
 * dmabufmgr_backoff_reservation - cancel a reservation
 * @list:	[in]	a linked list of struct dmabufmgr_validate
 *
 * This function cancels a previous reservation done by
 * dmabufmgr_reserve_buffers. This is useful in case something
 * goes wrong between reservation and committing.
 *
 * Please read Documentation/dma-buf-synchronization.txt
 */
void
dmabufmgr_backoff_reservation(struct list_head *list)
{
	if (list_empty(list))
		return;

	spin_lock(&dma_buf_reserve_lock);
	dmabufmgr_backoff_reservation_locked(list);
	spin_unlock(&dma_buf_reserve_lock);
}
EXPORT_SYMBOL_GPL(dmabufmgr_backoff_reservation);

/**
 * dmabufmgr_reserve_buffers - reserve a list of dmabufmgr_validate
 * @list:	[in]	a linked list of struct dmabufmgr_validate
 *
 * Attempts to reserve a list of dmabufmgr_validate. This function does not
 * decrease or increase refcount on dmabufmgr_validate.
 *
 * When this command returns 0 (success), the following
 * dmabufmgr_validate members become valid:
 * num_fences, fences[0...num_fences-1]
 *
 * The caller will have to queue waits on those fences before calling
 * dmabufmgr_fence_buffer_objects, with either hardware specific methods,
 * dma_fence_add_callback will, or dma_fence_wait.
 *
 * As such, by incrementing refcount on dmabufmgr_validate before calling
 * dma_fence_add_callback, and making the callback decrement refcount on
 * dmabufmgr_validate, or releasing refcount if dma_fence_add_callback
 * failed, the dmabufmgr_validate will be freed when all the fences
 * have been signaled, and only after the last ref is released, which should
 * be after dmabufmgr_fence_buffer_objects. With proper locking, when the
 * list_head holding the list of dmabufmgr_validate's becomes empty it
 * indicates all fences for all dma-bufs have been signaled.
 *
 * Please read Documentation/dma-buf-synchronization.txt
 */
int
dmabufmgr_reserve_buffers(struct list_head *list)
{
	struct dmabufmgr_validate *entry;
	int ret;
	u32 val_seq;

	if (list_empty(list))
		return 0;

	list_for_each_entry(entry, list, head) {
		entry->reserved = false;
		entry->num_fences = 0;
	}

retry:
	spin_lock(&dma_buf_reserve_lock);
	val_seq = atomic_inc_return(&dma_buf_reserve_counter);

	list_for_each_entry(entry, list, head) {
		struct dma_buf *bo = entry->bo;

retry_this_bo:
		ret = dma_buf_reserve_locked(bo, true, true, true, val_seq);
		switch (ret) {
		case 0:
			break;
		case -EBUSY:
			ret = dmabufmgr_wait_unreserved_locked(list, bo);
			if (unlikely(ret != 0)) {
				spin_unlock(&dma_buf_reserve_lock);
				return ret;
			}
			goto retry_this_bo;
		case -EAGAIN:
			dmabufmgr_backoff_reservation_locked(list);
			spin_unlock(&dma_buf_reserve_lock);
			ret = dma_buf_wait_unreserved(bo, true);
			if (unlikely(ret != 0))
				return ret;
			goto retry;
		default:
			dmabufmgr_backoff_reservation_locked(list);
			spin_unlock(&dma_buf_reserve_lock);
			return ret;
		}

		entry->reserved = true;

		if (entry->shared &&
		    bo->fence_shared_count == DMA_BUF_MAX_SHARED_FENCE) {
			WARN_ON_ONCE(1);
			dmabufmgr_backoff_reservation_locked(list);
			spin_unlock(&dma_buf_reserve_lock);
			return -EINVAL;
		}

		if (!entry->shared && bo->fence_shared_count) {
			entry->num_fences = bo->fence_shared_count;

			BUILD_BUG_ON(sizeof(entry->fences) !=
				     sizeof(bo->fence_shared));

			memcpy(entry->fences, bo->fence_shared,
			       sizeof(bo->fence_shared));
		} else if (bo->fence_excl) {
			entry->num_fences = 1;
			entry->fences[0] = bo->fence_excl;
		} else
			entry->num_fences = 0;
	}
	spin_unlock(&dma_buf_reserve_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(dmabufmgr_reserve_buffers);

/**
 * dmabufmgr_wait_timeout - wait synchronously for fence completion of
 * all fences in the list
 * @list:	[in]	a linked list of struct dmabufmgr_validate
 * @intr:	[in]	perform an interruptible wait
 * @timeout:	[in]	relative timeout in jiffies, or MAX_SCHEDULE_TIMEOUT
 *
 * Since this function waits synchronously it is meant mostly for cases where
 * stalling is unimportant, or to speed up writing initial implementations.
 *
 * Can be called after dmabufmgr_reserve_buffers returned, but before
 * dmabufmgr_backoff_reservation or dmabufmgr_fence_buffer_objects.
 *
 * Returns < 0 on error, 0 if the wait timed out, or the remaining timeout in
 * jiffies on success.
 */
long
dmabufmgr_wait_timeout(struct list_head *list, bool intr, signed long timeout)
{
	struct dmabufmgr_validate *entry;
	long ret = timeout;
	int i;

	list_for_each_entry(entry, list, head) {
		for (i = 0; i < entry->num_fences; i++) {
			ret = dma_fence_wait_timeout(entry->fences[i],
						     intr, ret);

			if (ret <= 0)
				return ret;
		}
	}
	return ret;
}
EXPORT_SYMBOL_GPL(dmabufmgr_wait_timeout);

/**
 * dmabufmgr_fence_buffer_objects - commit a reservation with a new fence
 * @fence:	[in]	the fence that indicates completion
 * @list:	[in]	a linked list of struct dmabufmgr_validate
 *
 * This function should be called after a hardware command submission is
 * completed succesfully. The fence is used to indicate completion of
 * those commands.
 *
 * Please read Documentation/dma-buf-synchronization.txt
 */
void
dmabufmgr_fence_buffer_objects(struct dma_fence *fence, struct list_head *list)
{
	struct dmabufmgr_validate *entry;
	struct dma_buf *bo;

	if (list_empty(list) || WARN_ON(!fence))
		return;

	/* Until deferred fput hits mainline, release old things here */
	list_for_each_entry(entry, list, head) {
		bo = entry->bo;

		if (!entry->shared) {
			int i;
			for (i = 0; i < bo->fence_shared_count; ++i) {
				dma_fence_put(bo->fence_shared[i]);
				bo->fence_shared[i] = NULL;
			}
			bo->fence_shared_count = 0;
			if (bo->fence_excl) {
				dma_fence_put(bo->fence_excl);
				bo->fence_excl = NULL;
			}
		}

		entry->reserved = false;
	}

	spin_lock(&dma_buf_reserve_lock);

	list_for_each_entry(entry, list, head) {
		bo = entry->bo;

		dma_fence_get(fence);
		if (entry->shared)
			bo->fence_shared[bo->fence_shared_count++] = fence;
		else
			bo->fence_excl = fence;

		dma_buf_unreserve_locked(bo);
	}

	spin_unlock(&dma_buf_reserve_lock);
}
EXPORT_SYMBOL_GPL(dmabufmgr_fence_buffer_objects);

/**
 * dmabufmgr_validate_free - simple free function for dmabufmgr_validate
 * @ref:	[in]	pointer to dmabufmgr_validate::refcount to free
 *
 * Can be called when refcount drops to 0, but isn't required to be used.
 */
void dmabufmgr_validate_free(struct kref *ref)
{
	struct dmabufmgr_validate *val;
	val = container_of(ref, struct dmabufmgr_validate, refcount);
	list_del(&val->head);
	kfree(val);
}
EXPORT_SYMBOL_GPL(dmabufmgr_validate_free);
