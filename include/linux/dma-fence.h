/*
 * Fence mechanism for dma-buf to allow for asynchronous dma access
 *
 * Copyright (C) 2012 Texas Instruments
 * Author: Rob Clark <rob.clark@linaro.org>
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

#ifndef __DMA_FENCE_H__
#define __DMA_FENCE_H__

#include <linux/err.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/dma-buf.h>

struct dma_fence;
struct dma_fence_ops;
struct dma_fence_cb;

/**
 * struct dma_fence - software synchronization primitive
 * @refcount: refcount for this fence
 * @ops: dma_fence_ops associated with this fence
 * @event_queue: event queue used for signaling fence
 * @priv: fence specific private data
 * @flags: A mask of DMA_FENCE_FLAG_* defined below
 *
 * DMA_FENCE_FLAG_NEED_SW_SIGNAL - enable_signaling has been called
 * DMA_FENCE_FLAG_SIGNALED - fence is already signaled
 *
 * Read Documentation/dma-buf-synchronization.txt for usage.
 */
struct dma_fence {
	struct kref refcount;
	const struct dma_fence_ops *ops;
	wait_queue_head_t event_queue;
	void *priv;
	unsigned long flags;
};
#define DMA_FENCE_FLAG_SIGNALED BIT(0)
#define DMA_FENCE_FLAG_NEED_SW_SIGNAL BIT(1)

typedef int (*dma_fence_func_t)(struct dma_fence_cb *cb, void *priv);

/**
 * struct dma_fence_cb - callback for dma_fence_add_callback
 * @base: wait_queue_t added to event_queue
 * @func: dma_fence_func_t to call
 * @fence: fence this dma_fence_cb was used on
 *
 * This struct will be initialized by dma_fence_add_callback, additional
 * data can be passed along by embedding dma_fence_cb in another struct.
 */
struct dma_fence_cb {
	wait_queue_t base;
	dma_fence_func_t func;
	struct dma_fence *fence;
};

/**
 * struct dma_fence_ops - operations implemented for dma-fence
 * @enable_signaling: enable software signaling of fence
 * @release: [optional] called on destruction of fence
 *
 * Notes on enable_signaling:
 * For fence implementations that have the capability for hw->hw
 * signaling, they can implement this op to enable the necessary
 * irqs, or insert commands into cmdstream, etc.  This is called
 * in the first wait() or add_callback() path to let the fence
 * implementation know that there is another driver waiting on
 * the signal (ie. hw->sw case).
 *
 * This function can be called called from atomic context, but not
 * from irq context, so normal spinlocks can be used.
 *
 * A return value of false indicates the fence already passed,
 * or some failure occured that made it impossible to enable
 * signaling. True indicates succesful enabling.
 *
 * Calling dma_fence_signal before enable_signaling is called allows
 * for a tiny race window in which enable_signaling is called during,
 * before, or after dma_fence_signal. To fight this, it is recommended
 * that before enable_signaling returns true an extra reference is
 * taken on the dma_fence, to be released when the fence is signaled.
 * This will mean dma_fence_signal will still be called twice, but
 * the second time will be a noop since it was already signaled.
 *
 * Notes on release:
 * Can be NULL, this function allows additional commands to run on
 * destruction of the dma_fence. Can be called from irq context.
 */

struct dma_fence_ops {
	bool (*enable_signaling)(struct dma_fence *fence);
	void (*release)(struct dma_fence *fence);
};

struct dma_fence *dma_fence_create(void *priv);

/**
 * __dma_fence_init - Initialize a custom dma_fence.
 * @fence:	[in]	the fence to initialize
 * @ops:	[in]	the dma_fence_ops for operations on this fence
 * @priv:	[in]	the value to use for the priv member
 *
 * Initializes an allocated fence, the caller doesn't have to keep its
 * refcount after committing with this fence, but it will need to hold a
 * refcount again if dma_fence_ops.enable_signaling gets called. This can
 * be used for other implementing other types of dma_fence.
 */
static inline void
__dma_fence_init(struct dma_fence *fence,
		 const struct dma_fence_ops *ops, void *priv)
{
	WARN_ON(!ops || !ops->enable_signaling);

	kref_init(&fence->refcount);
	fence->ops = ops;
	fence->priv = priv;
	fence->flags = 0UL;
	init_waitqueue_head(&fence->event_queue);
}

/**
 * dma_fence_is_signaled - Return an indication if the fence is signaled yet.
 * @fence:	[in]	the fence to check
 *
 * Returns true if the fence was already signaled, false if not. Since this
 * function doesn't enable signaling, it is not guaranteed to ever return true
 * if dma_fence_add_callback or dma_fence_wait haven't been called before.
 *
 * It's recommended for seqno fences to call dma_fence_signal when the
 * operation is complete, it makes it possible to prevent issues from
 * wraparound between time of issue and time of use by checking the return
 * value of this function before calling hardware-specific wait instructions.
 */
static inline bool
dma_fence_is_signaled(struct dma_fence *fence)
{
	rmb();
	return !!(fence->flags & DMA_FENCE_FLAG_SIGNALED);
}

void dma_fence_get(struct dma_fence *fence);
void dma_fence_put(struct dma_fence *fence);

int dma_fence_signal(struct dma_fence *fence);
long dma_fence_wait(struct dma_fence *fence, bool intr);
long dma_fence_wait_timeout(struct dma_fence *fence, bool intr, signed long);
int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
			   dma_fence_func_t func, void *priv);
bool dma_fence_remove_callback(struct dma_fence *fence,
			       struct dma_fence_cb *cb);

#endif /* __DMA_FENCE_H__ */
