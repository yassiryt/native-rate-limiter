#include "limiter.hpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static constexpr uint64_t SHM_MAGIC = 0x4e524c5f76320001ULL;
static constexpr uint32_t SHM_VERSION = 2;
static constexpr const char* DEFAULT_SHM_NAME = "/node_rate_limiter_shm";
static Bucket* table = nullptr;
ControlBlock* ctrl = nullptr;
static size_t mapped_size = 0;

static uint64_t now_ms()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

static uint64_t secure_hash(const char* str)
{
	uint64_t hash = ctrl->hash_seed.load(std::memory_order_relaxed);
	while (*str)
	{
		hash ^= static_cast<uint8_t>(*str++);
		hash *= 1099511628211ULL;
	}
	return hash == 0 ? 1 : hash;
}

static void lock_data()
{
	while (ctrl->data_lock.test_and_set(std::memory_order_acquire))
		;
}

static void unlock_data()
{
	ctrl->data_lock.clear(std::memory_order_release);
}

static bool valid_config(const LimiterConfig& config, const char** err_msg)
{
	if (config.max_tokens <= 0)
		*err_msg = "maxTokens must be greater than zero";
	else if (config.refill_ms == 0)
		*err_msg = "windowMs must be greater than zero";
	else if (config.table_size != TABLE_SIZE)
		*err_msg = "table_size must equal TABLE_SIZE";
	else
		return true;
	return false;
}

bool init_limiter(const LimiterConfig& config, const char** err_msg)
{
	if (table)
		return true;
	if (!valid_config(config, err_msg))
		return false;

	const char* shm_name = std::getenv("RATE_LIMITER_SHM_NAME");
	if (!shm_name || shm_name[0] != '/')
		shm_name = DEFAULT_SHM_NAME;
	int shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0660);
	bool created = true;
	if (shm_fd < 0 && errno == EEXIST)
	{
		shm_fd = shm_open(shm_name, O_RDWR, 0660);
		created = false;
	}
	if (shm_fd < 0)
	{
		*err_msg = "shm_open failed";
		return false;
	}
	mapped_size = TABLE_SIZE * sizeof(Bucket) + sizeof(ControlBlock);
	if (created && ftruncate(shm_fd, static_cast<off_t>(mapped_size)) < 0)
	{
		static char error[128];
		std::snprintf(error, sizeof(error), "ftruncate failed (%zu): %s", mapped_size,
			std::strerror(errno));
		*err_msg = error;
		close(shm_fd);
		return false;
	}
	void* mapped = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (mapped == MAP_FAILED)
	{
		*err_msg = "mmap failed";
		return false;
	}

	table = static_cast<Bucket*>(mapped);
	ctrl = reinterpret_cast<ControlBlock*>(static_cast<char*>(mapped)
		+ TABLE_SIZE * sizeof(Bucket));
	bool needs_init = ctrl->magic.load(std::memory_order_acquire) != SHM_MAGIC
		|| ctrl->version.load(std::memory_order_acquire) != SHM_VERSION;
	if (needs_init)
	{
		while (ctrl->init_lock.test_and_set(std::memory_order_acquire))
			;
		if (ctrl->magic.load(std::memory_order_acquire) != SHM_MAGIC
			|| ctrl->version.load(std::memory_order_acquire) != SHM_VERSION)
		{
			ctrl->data_lock.clear(std::memory_order_relaxed);
			ctrl->max_tokens.store(config.max_tokens, std::memory_order_relaxed);
			ctrl->refill_ms.store(config.refill_ms, std::memory_order_relaxed);
			ctrl->hash_seed.store(now_ms() ^ reinterpret_cast<uintptr_t>(ctrl),
				std::memory_order_relaxed);
			ctrl->used_buckets.store(0, std::memory_order_relaxed);
			ctrl->total_requests.store(0, std::memory_order_relaxed);
			ctrl->accepted_requests.store(0, std::memory_order_relaxed);
			ctrl->rejected_requests.store(0, std::memory_order_relaxed);
			ctrl->evictions.store(0, std::memory_order_relaxed);
			for (size_t i = 0; i < TABLE_SIZE; ++i)
			{
				table[i].ip_hash.store(0, std::memory_order_relaxed);
				table[i].toks.store(0, std::memory_order_relaxed);
				table[i].ts.store(0, std::memory_order_relaxed);
				table[i].last_access = 0;
			}
			lru_init();
			ctrl->version.store(SHM_VERSION, std::memory_order_relaxed);
			ctrl->magic.store(SHM_MAGIC, std::memory_order_release);
		}
		ctrl->init_lock.clear(std::memory_order_release);
	}

	if (ctrl->max_tokens.load(std::memory_order_acquire) != config.max_tokens
		|| ctrl->refill_ms.load(std::memory_order_acquire) != config.refill_ms)
	{
		*err_msg = "shared-memory limiter is already configured differently";
		munmap(table, mapped_size);
		table = nullptr;
		ctrl = nullptr;
		mapped_size = 0;
		return false;
	}
	return true;
}

bool init_limiter(int32_t max_tokens, uint64_t refill_ms, const char** err_msg)
{
	LimiterConfig config;
	config.max_tokens = max_tokens;
	config.refill_ms = refill_ms;
	return init_limiter(config, err_msg);
}

bool consume_token(const char* ip_str, int32_t max_tokens, uint64_t refill_ms)
{
	if (!table || !ip_str || !*ip_str)
		return false;
	if (max_tokens != ctrl->max_tokens.load(std::memory_order_acquire)
		|| refill_ms != ctrl->refill_ms.load(std::memory_order_acquire))
		return false;

	lock_data();
	ctrl->total_requests.fetch_add(1, std::memory_order_relaxed);
	uint64_t hash = secure_hash(ip_str);
	size_t start = hash % TABLE_SIZE;
	size_t idx = start;
	size_t free_idx = SIZE_MAX;
	size_t now = now_ms();

	for (size_t attempts = 0; attempts < TABLE_SIZE; ++attempts)
	{
		Bucket& bucket = table[idx];
		uint64_t stored = bucket.ip_hash.load(std::memory_order_relaxed);
		if (stored == hash)
		{
			free_idx = idx;
			break;
		}
		if (stored == 0 && free_idx == SIZE_MAX)
			free_idx = idx;
		idx = (idx + 1) % TABLE_SIZE;
	}
	if (free_idx == SIZE_MAX)
	{
		free_idx = lru_evict();
		if (free_idx == SIZE_MAX)
		{
			ctrl->rejected_requests.fetch_add(1, std::memory_order_relaxed);
			unlock_data();
			return false;
		}
		ctrl->evictions.fetch_add(1, std::memory_order_relaxed);
		ctrl->used_buckets.fetch_sub(1, std::memory_order_relaxed);
	}
	Bucket& bucket = table[free_idx];
	if (bucket.ip_hash.load(std::memory_order_relaxed) != hash)
	{
		if (bucket.ip_hash.load(std::memory_order_relaxed) == 0)
			ctrl->used_buckets.fetch_add(1, std::memory_order_relaxed);
		bucket.ip_hash.store(hash, std::memory_order_relaxed);
		bucket.toks.store(max_tokens, std::memory_order_relaxed);
		bucket.ts.store(now, std::memory_order_relaxed);
	}
	else
	{
		uint64_t last = bucket.ts.load(std::memory_order_relaxed);
		uint64_t elapsed = now > last ? now - last : 0;
		uint64_t add = elapsed / refill_ms;
		if (add > 0)
		{
			bucket.toks.store(std::min<int64_t>(max_tokens,
				static_cast<int64_t>(bucket.toks.load(std::memory_order_relaxed)) + add),
				std::memory_order_relaxed);
			bucket.ts.store(last + add * refill_ms, std::memory_order_relaxed);
		}
	}
	bucket.last_access = now;
	lru_touch(free_idx);
	int32_t tokens = bucket.toks.load(std::memory_order_relaxed);
	if (tokens <= 0)
	{
		ctrl->rejected_requests.fetch_add(1, std::memory_order_relaxed);
		unlock_data();
		return false;
	}
	bucket.toks.store(tokens - 1, std::memory_order_relaxed);
	ctrl->accepted_requests.fetch_add(1, std::memory_order_relaxed);
	unlock_data();
	return true;
}

void cleanup_limiter()
{
	if (table && table != MAP_FAILED)
		munmap(table, mapped_size);
	table = nullptr;
	ctrl = nullptr;
	mapped_size = 0;
}
