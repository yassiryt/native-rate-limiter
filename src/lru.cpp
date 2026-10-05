#include "limiter.hpp"

void lru_init()
{
	ctrl->lru_head.store(0, std::memory_order_relaxed);
	ctrl->lru_tail.store(TABLE_SIZE - 1, std::memory_order_relaxed);
	for (size_t i = 0; i < TABLE_SIZE; ++i)
		ctrl->lru_next[i].store(i + 1, std::memory_order_relaxed);
	ctrl->lru_next[TABLE_SIZE - 1].store(SIZE_MAX, std::memory_order_relaxed);
}

void lru_touch(size_t idx)
{
	if (idx == ctrl->lru_tail.load(std::memory_order_relaxed))
		return;

	size_t prev = SIZE_MAX;
	size_t cur = ctrl->lru_head.load(std::memory_order_relaxed);
	while (cur != SIZE_MAX && cur != idx)
	{
		prev = cur;
		cur = ctrl->lru_next[cur].load(std::memory_order_relaxed);
	}
	if (cur != idx)
		return;

	size_t next = ctrl->lru_next[idx].load(std::memory_order_relaxed);
	if (prev == SIZE_MAX)
		ctrl->lru_head.store(next, std::memory_order_relaxed);
	else
		ctrl->lru_next[prev].store(next, std::memory_order_relaxed);

	ctrl->lru_next[ctrl->lru_tail.load(std::memory_order_relaxed)].store(idx,
		std::memory_order_relaxed);
	ctrl->lru_tail.store(idx, std::memory_order_relaxed);
	ctrl->lru_next[idx].store(SIZE_MAX, std::memory_order_relaxed);
}

size_t lru_evict()
{
	size_t idx = ctrl->lru_head.load(std::memory_order_relaxed);
	if (idx == SIZE_MAX)
		return SIZE_MAX;
	lru_touch(idx);
	return idx;
}

size_t lru_get_head()
{
	return ctrl->lru_head.load(std::memory_order_acquire);
}

size_t lru_get_tail()
{
	return ctrl->lru_tail.load(std::memory_order_acquire);
}
