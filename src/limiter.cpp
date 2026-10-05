#include "limiter.hpp"
#include "lru.cpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <algorithm>
#include <random>

static Bucket*		table = nullptr;
ControlBlock*		ctrl = nullptr;
static uint64_t		g_hash_seed = 0;

uint64_t	secure_hash(const char* str)
{
	uint64_t	hash;

	hash = g_hash_seed;
	while (*str)
	{
		hash ^= (uint8_t)(*str++);
		hash *= 1099511628211ULL;
	}
	return hash;
}

bool	init_limiter(int32_t max_tokens, uint64_t refill_ms, const char** err_msg)
{
	int		shm_fd;
	size_t	i;

	if (table)
		return true;

	if (g_hash_seed == 0)
		g_hash_seed = std::chrono::steady_clock::now().time_since_epoch().count();

	shm_fd = shm_open("/node_rate_limiter_shm", O_CREAT | O_RDWR, 0666);
	if (shm_fd < 0)
	{
		*err_msg = "shm_open failed";
		return false;
	}

	size_t total_size = TABLE_SIZE * sizeof(Bucket) + sizeof(ControlBlock);
	if (ftruncate(shm_fd, total_size) < 0)
	{
		*err_msg = "ftruncate failed";
		close(shm_fd);
		return false;
	}

	table = (Bucket*)mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);

	if (table == MAP_FAILED)
	{
		*err_msg = "mmap failed";
		return false;
	}

	ctrl = (ControlBlock*)((char*)table + TABLE_SIZE * sizeof(Bucket));

	i = 0;
	while (i < TABLE_SIZE)
	{
		table[i].ip_hash.store(0, std::memory_order_relaxed);
		table[i].toks.store(max_tokens, std::memory_order_relaxed);
		table[i].ts.store(0, std::memory_order_relaxed);
		i++;
	}

	ctrl->lru_lock.clear(std::memory_order_relaxed);
	ctrl->lru_head.store(0, std::memory_order_relaxed);
	ctrl->lru_tail.store(TABLE_SIZE - 1, std::memory_order_relaxed);
	i = 0;
	while (i < TABLE_SIZE)
	{
		ctrl->lru_next[i].store(i + 1, std::memory_order_relaxed);
		i++;
	}
	ctrl->lru_next[TABLE_SIZE - 1].store(SIZE_MAX, std::memory_order_relaxed);

	return true;
}

bool	consume_token(const char* ip_str, int32_t max_tokens, uint64_t refill_ms)
{
	uint64_t		ip_h;
	size_t			idx;
	size_t			attempts;
	Bucket*			b;
	uint64_t		stored_hash;
	uint64_t		expected;
	uint64_t		now;
	uint64_t		last;
	int32_t			toks;
	int32_t			add;
	int32_t			updated;

	if (!table || !ip_str)
		return false;

	ip_h = secure_hash(ip_str);
	idx = ip_h % TABLE_SIZE;
	attempts = 0;

	now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()
	).count();

	while (attempts < 100)
	{
		b = &table[idx];
		stored_hash = b->ip_hash.load(std::memory_order_acquire);

		if (stored_hash == 0)
		{
			expected = 0;
			if (b->ip_hash.compare_exchange_strong(expected, ip_h, std::memory_order_acq_rel))
			{
				move_lru_to_tail(idx);
				break;
			}
			continue;
		}

		if (stored_hash == ip_h)
			break;

		last = b->ts.load(std::memory_order_acquire);
		if (last > 0 && (now - last) > refill_ms)
		{
			if (b->ip_hash.compare_exchange_strong(stored_hash, ip_h, std::memory_order_acq_rel))
			{
				b->toks.store(max_tokens, std::memory_order_release);
				b->ts.store(now, std::memory_order_release);
				move_lru_to_tail(idx);
				break;
			}
		}

		idx = (idx + 1) % TABLE_SIZE;
		attempts++;
	}

	b = &table[idx];
	last = b->ts.load(std::memory_order_acquire);
	toks = b->toks.load(std::memory_order_acquire);
	add = (now - last) / refill_ms;

	if (add > 0)
	{
		updated = std::min(max_tokens, toks + add);
		if (b->ts.compare_exchange_strong(last, now))
		{
			toks = b->toks.load(std::memory_order_acquire);
			b->toks.store(std::min(max_tokens, toks + add), std::memory_order_release);
		}
	}

	while (toks > 0)
	{
		if (b->toks.compare_exchange_weak(toks, toks - 1, std::memory_order_acq_rel))
		{
			move_lru_to_tail(idx);
			return true;
		}
	}

	return false;
}

void	cleanup_limiter()
{
	if (table && table != MAP_FAILED)
	{
		munmap(table, TABLE_SIZE * sizeof(Bucket) + sizeof(ControlBlock));
		table = nullptr;
		ctrl = nullptr;
	}
	shm_unlink("/node_rate_limiter_shm");
}