/**************************************************************************
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

#include "ttm/ttm_execbuf_util.h"
#include "ttm/ttm_bo_driver.h"
#include "ttm/ttm_placement.h"
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/module.h>

static void ttm_eu_backoff_reservation_locked(struct list_head *list,
					      struct ttm_validate_buffer *entry,
					      struct reservation_ticket *ticket)
{
	list_for_each_entry_continue_reverse(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		object_unreserve(bo->resv, ticket);
	}
	reservation_ticket_fini(ticket);
}

void ttm_eu_backoff_reservation(struct reservation_ticket *ticket,
				struct list_head *list)
{
	struct ttm_validate_buffer *entry;
	struct ttm_bo_global *glob;

	if (list_empty(list))
		return;

	entry = list_first_entry(list, struct ttm_validate_buffer, head);
	glob = entry->bo->glob;
	spin_lock(&glob->lru_lock);

	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		ttm_bo_unreserve_ticket_locked(bo, ticket);
	}
	reservation_ticket_fini(ticket);
	spin_unlock(&glob->lru_lock);
}
EXPORT_SYMBOL(ttm_eu_backoff_reservation);

/*
 * Reserve buffers for validation.
 *
 * If a buffer in the list is marked for CPU access, we back off and
 * wait for that buffer to become free for GPU access.
 *
 * If a buffer is reserved for another validation, the validator with
 * the highest validation sequence backs off and waits for that buffer
 * to become unreserved. This prevents deadlocks when validating multiple
 * buffers in different orders.
 */

int ttm_eu_reserve_buffers(struct reservation_ticket *ticket,
			   struct list_head *list)
{
	struct ttm_bo_global *glob;
	struct ttm_validate_buffer *entry;
	int ret;

	if (list_empty(list))
		return 0;

	entry = list_first_entry(list, struct ttm_validate_buffer, head);
	glob = entry->bo->glob;

retry:
	reservation_ticket_init(ticket);

	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		ret = ttm_bo_reserve_nolru(bo, true, false, true, ticket);
		switch (ret) {
		case 0:
			break;
		case -EAGAIN:
			spin_lock(&glob->lru_lock);
			ttm_eu_backoff_reservation_locked(list, entry, ticket);
			spin_unlock(&glob->lru_lock);
			ret = ttm_bo_wait_unreserved(bo, true);
			if (unlikely(ret != 0))
				return ret;
			goto retry;
		default:
			spin_lock(&glob->lru_lock);
			ttm_eu_backoff_reservation_locked(list, entry, ticket);
			spin_unlock(&glob->lru_lock);
			return ret;
		}
	}

	spin_lock(&glob->lru_lock);
	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		entry->put_count = ttm_bo_del_from_lru(bo);
	}
	spin_unlock(&glob->lru_lock);

	list_for_each_entry(entry, list, head) {
		struct ttm_buffer_object *bo = entry->bo;

		if (entry->put_count) {
			ttm_bo_list_ref_sub(bo, entry->put_count, true);
			entry->put_count = 0;
		}
	}

	return 0;
}
EXPORT_SYMBOL(ttm_eu_reserve_buffers);

void ttm_eu_fence_buffer_objects(struct reservation_ticket *ticket,
				 struct list_head *list, void *sync_obj)
{
	struct ttm_validate_buffer *entry;
	struct ttm_buffer_object *bo;
	struct ttm_bo_global *glob;
	struct ttm_bo_device *bdev;
	struct ttm_bo_driver *driver;

	if (list_empty(list))
		return;

	bo = list_first_entry(list, struct ttm_validate_buffer, head)->bo;
	bdev = bo->bdev;
	driver = bdev->driver;
	glob = bo->glob;

	list_for_each_entry(entry, list, head) {
		bo = entry->bo;

		if (bo->sync_obj)
			driver->sync_obj_unref(&bo->sync_obj);

		bo->sync_obj = driver->sync_obj_ref(sync_obj);
	}

	spin_lock(&glob->lru_lock);
	list_for_each_entry(entry, list, head) {
		bo = entry->bo;
		ttm_bo_unreserve_ticket_locked(bo, ticket);
	}
	spin_unlock(&glob->lru_lock);
	reservation_ticket_fini(ticket);
}
EXPORT_SYMBOL(ttm_eu_fence_buffer_objects);
