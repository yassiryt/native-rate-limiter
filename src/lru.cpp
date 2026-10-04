#include "limiter.hpp"

static inline void lru_lock_acquire()
{
	while (ctrl->lru_lock.test_and_set(std::memory_order_acquire))
		;
}

static inline void lru_lock_release()
{
	ctrl->lru_lock.clear(std::memory_order_release);
}

void lru_init()
{
	ctrl->lru_lock.clear(std::memory_order_relaxed);
	ctrl->lru_head.store(0, std::memory_order_relaxed);
	ctrl->lru_tail.store(TABLE_SIZE - 1, std::memory_order_relaxed);
	for (size_t i = 0; i < TABLE_SIZE; ++i)
		ctrl->lru_next[i].store(i + 1, std::memory_order_relaxed);
	ctrl->lru_next[TABLE_SIZE - 1].store(SIZE_MAX, std::memory_order_relaxed);
}

void move_lru_to_tail(size_t idx)
{
	lru_lock_acquire();

	size_t next_idx = ctrl->lru_next[idx].load(std::memory_order_relaxed);
	if (next_idx == SIZE_MAX && idx == ctrl->lru_tail.load(std::memory_order_relaxed))
	{
		lru_lock_release();
		return;
	}

	size_t prev = SIZE_MAX;
	size_t cur = ctrl->lru_head.load(std::memory_order_relaxed);
	while (cur != SIZE_MAX && cur != idx)
	{
		prev = cur;
		cur = ctrl->lru_next[cur].load(std::memory_order_relaxed);
	}

	if (prev != SIZE_MAX)
		ctrl->lru_next[prev].store(ctrl->lru_next[idx].load(std::memory_order_relaxed), std::memory_order_relaxed);
	else
		ctrl->lru_head.store(ctrl->lru_next[idx].load(std::memory_order_relaxed), std::memory_order_relaxed);

	ctrl->lru_next[ctrl->lru_tail.load(std::memory_order_relaxed)].store(idx, std::memory_order_relaxed);
	ctrl->lru_tail.store(idx, std::memory_order_relaxed);
	ctrl->lru_next[idx].store(SIZE_MAX, std::memory_order_relaxed);

	lru_lock_release();
}

size_t get_lru_head()
{
	return ctrl->lru_head.load(std::memory_order_acquire);
}

size_t get_lru_tail()
{
	return ctrl->lru_tail.load(std::memory_order_acquire);
}