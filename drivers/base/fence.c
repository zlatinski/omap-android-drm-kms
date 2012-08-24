/*
 * Fence mechanism for dma-buf and to allow for asynchronous dma access
 *
 * Copyright (C) 2012 Canonical Ltd
 * Copyright (C) 2012 Texas Instruments
 *
 * Authors:
 * Rob Clark <rob.clark@linaro.org>
 * Maarten Lankhorst <maarten.lankhorst@canonical.com>
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

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/fence.h>

static int __fence_signal(struct fence *fence)
{
	if (fence->flags & FENCE_FLAG_SIGNALED)
		return -EINVAL;

	fence->flags |= FENCE_FLAG_SIGNALED;
	__wake_up_locked_key(&fence->event_queue, TASK_NORMAL,
			     &fence->event_queue);
	return 0;
}

/**
 * fence_signal - signal completion of a fence
 * @fence: the fence to signal
 *
 * Signal completion for software callbacks on a fence, this will unblock
 * fence_wait() calls and run all the callbacks added with
 * fence_add_callback(). Can be called multiple times, but since a fence
 * can only go from unsignaled to signaled state, it will only be effective
 * the first time.
 */
int fence_signal(struct fence *fence)
{
	unsigned long flags;
	int ret;

	if (WARN_ON(!fence))
		return -EINVAL;

	spin_lock_irqsave(&fence->event_queue.lock, flags);
	ret = __fence_signal(fence);
	spin_unlock_irqrestore(&fence->event_queue.lock, flags);

	return ret;
}
EXPORT_SYMBOL(fence_signal);

static void release_fence(struct kref *kref)
{
	struct fence *fence =
			container_of(kref, struct fence, refcount);

	BUG_ON(waitqueue_active(&fence->event_queue));

	if (fence->ops->release)
		fence->ops->release(fence);

	kfree(fence);
}

/**
 * fence_put - decreases refcount of the fence
 * @fence:	[in]	fence to reduce refcount of
 */
void fence_put(struct fence *fence)
{
	if (WARN_ON(!fence))
		return;
	kref_put(&fence->refcount, release_fence);
}
EXPORT_SYMBOL(fence_put);

/**
 * fence_get - increases refcount of the fence
 * @fence:	[in]	fence to increase refcount of
 */
void fence_get(struct fence *fence)
{
	if (WARN_ON(!fence))
		return;
	kref_get(&fence->refcount);
}
EXPORT_SYMBOL(fence_get);

static int
__fence_wake_func(wait_queue_t *wait, unsigned mode, int flags, void *key)
{
	struct fence_cb *cb =
			container_of(wait, struct fence_cb, base);

	__remove_wait_queue(key, wait);
	return cb->func(cb, wait->private);
}

/**
 * fence_enable_sw_signaling - enable signaling on fence
 * @fence:	[in]	the fence to enable
 *
 * this will request for sw signaling to be enabled, to make the fence
 * complete as soon as possible
 */
void fence_enable_sw_signaling(struct fence *fence)
{
	unsigned long flags;
	spin_lock_irqsave(&fence->event_queue.lock, flags);

	if (!(fence->flags & (FENCE_FLAG_SIGNALED |
			      FENCE_FLAG_NEED_SW_SIGNAL))) {
		fence->flags |= FENCE_FLAG_NEED_SW_SIGNAL;

		spin_unlock_irqrestore(&fence->event_queue.lock, flags);

		if (!fence->ops->enable_signaling(fence))
			fence_signal(fence);
	} else
		spin_unlock_irqrestore(&fence->event_queue.lock, flags);
}
EXPORT_SYMBOL(fence_enable_sw_signaling);

/**
 * fence_add_callback - add a callback to be called when the fence
 * is signaled
 * @fence:	[in]	the fence to wait on
 * @cb:		[in]	the callback to register
 * @func:	[in]	the function to call
 * @priv:	[in]	the argument to pass to function
 *
 * cb will be initialized by fence_add_callback, no initialization
 * by the caller is required. Any number of callbacks can be registered
 * to a fence, but a callback can only be registered to one fence at a time.
 *
 * Note that the callback can be called from an atomic context.  If
 * fence is already signaled, this function will return -ENOENT (and
 * *not* call the callback)
 *
 * Add a software callback to the fence. Same restrictions apply to
 * refcount as it does to fence_wait, however the caller doesn't need to
 * keep a refcount to fence afterwards: when software access is enabled,
 * the creator of the fence is required to keep the fence alive until
 * after it signals with fence_signal. The callback itself can be called
 * from irq context.
 *
 */
int fence_add_callback(struct fence *fence, struct fence_cb *cb,
		       fence_func_t func, void *priv)
{
	unsigned long flags;
	int ret;

	if (WARN_ON(!fence || !func))
		return -EINVAL;

	spin_lock_irqsave(&fence->event_queue.lock, flags);

	if (!(fence->flags & (FENCE_FLAG_SIGNALED |
			      FENCE_FLAG_NEED_SW_SIGNAL))) {
		bool enabled;

		fence->flags |= FENCE_FLAG_NEED_SW_SIGNAL;

		/* Drop lock here, else there might be lock inversion when
		 * enable_signaling takes a lock that's held when
		 * fence_signal is called.
		 */
		spin_unlock_irqrestore(&fence->event_queue.lock, flags);
		enabled = fence->ops->enable_signaling(fence);
		spin_lock_irqsave(&fence->event_queue.lock, flags);

		if (!enabled)
			__fence_signal(fence);
	}

	if (fence->flags & FENCE_FLAG_SIGNALED)
		ret = -ENOENT;
	else {
		ret = 0;

		cb->base.flags = 0;
		cb->base.func = __fence_wake_func;
		cb->base.private = priv;
		cb->fence = fence;
		cb->func = func;
		__add_wait_queue(&fence->event_queue, &cb->base);
	}
	spin_unlock_irqrestore(&fence->event_queue.lock, flags);

	return ret;
}
EXPORT_SYMBOL(fence_add_callback);

/**
 * fence_remove_callback - remove a callback from the signaling list
 * @fence:	[in]	the fence to wait on
 * @cb:		[in]	the callback to remove
 *
 * Remove a previously queued callback from the fence. This function returns
 * true is the callback is succesfully removed, or false if the fence has
 * already been signaled.
 *
 * *WARNING*:
 * Cancelling a callback should only be done if you really know what you're
 * doing, since deadlocks and race conditions could occur all too easily. For
 * this reason, it should only ever be done on hardware lockup recovery,
 * with a reference held to the fence.
 */
bool
fence_remove_callback(struct fence *fence, struct fence_cb *cb)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&fence->event_queue.lock, flags);

	ret = !(fence->flags & FENCE_FLAG_SIGNALED);
	if (ret)
		__remove_wait_queue(&fence->event_queue, &cb->base);
	spin_unlock_irqrestore(&fence->event_queue.lock, flags);

	return ret;
}
EXPORT_SYMBOL(fence_remove_callback);

/**
 * fence_default_wait - default sleep until the fence gets signaled
 * or until timeout elapses
 * @fence:	[in]	the fence to wait on
 * @intr:	[in]	if true, do an interruptible wait
 * @timeout:	[in]	timeout value in jiffies, or MAX_SCHEDULE_TIMEOUT
 *
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or the
 * remaining timeout in jiffies on success.
 */
long
fence_default_wait(struct fence *fence, bool intr, signed long timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;
	long ret = timeout;

	spin_lock_irqsave(&fence->event_queue.lock, flags);

	if (!(fence->flags & FENCE_FLAG_SIGNALED)) {
		bool enable = false;

		if (intr && signal_pending(current)) {
			ret = -ERESTARTSYS;
			goto out;
		}

		if (!(fence->flags & FENCE_FLAG_NEED_SW_SIGNAL)) {
			fence->flags |= FENCE_FLAG_NEED_SW_SIGNAL;
			enable = true;
		}

		__add_wait_queue(&fence->event_queue, &wait);
		while (!(fence->flags & FENCE_FLAG_SIGNALED) && ret > 0) {
			if (intr)
				set_current_state(TASK_INTERRUPTIBLE);
			else
				set_current_state(TASK_UNINTERRUPTIBLE);
			spin_unlock_irqrestore(&fence->event_queue.lock,
					       flags);

			if (enable) {
				if (!fence->ops->enable_signaling(fence))
					fence_signal(fence);
				enable = false;
			}

			ret = schedule_timeout(ret);

			spin_lock_irqsave(&fence->event_queue.lock, flags);
			if (ret > 0 && intr && signal_pending(current))
				ret = -ERESTARTSYS;
		}
		__remove_wait_queue(&fence->event_queue, &wait);
		__set_current_state(TASK_RUNNING);
	}
out:
	spin_unlock_irqrestore(&fence->event_queue.lock, flags);
	return ret;
}
EXPORT_SYMBOL(fence_default_wait);

static bool sw_enable_signaling(struct fence *fence)
{
	/* fence_create sets needs_sw_signal,
	 * so this should never be called
	 */
	WARN_ON_ONCE(1);
	return true;
}

static const struct fence_ops sw_fence_ops = {
	.enable_signaling = sw_enable_signaling,
};

/**
 * fence_create - create a simple sw-only fence
 * @priv:	[in]	the value to use for the priv member
 *
 * This fence only supports signaling from/to CPU.  Other implementations
 * of fence can be used to support hardware to hardware signaling, if
 * supported by the hardware, and use the fence_helper_* functions for
 * compatibility with other devices that only support sw signaling.
 */
struct fence *fence_create(void *priv)
{
	struct fence *fence;

	fence = kmalloc(sizeof(struct fence), GFP_KERNEL);
	if (!fence)
		return NULL;

	__fence_init(fence, &sw_fence_ops, priv);
	fence->flags |= FENCE_FLAG_NEED_SW_SIGNAL;

	return fence;
}
EXPORT_SYMBOL(fence_create);
