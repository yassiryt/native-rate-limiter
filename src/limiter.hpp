#ifndef LIMITER_HPP
# define LIMITER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class EvictionPolicy { LRU, FIFO };

struct alignas(64)	Bucket{
	std::atomic<uint64_t>	ip_hash;
	std::atomic<uint64_t>	ts;
	std::atomic<int32_t>	toks;
	uint64_t				last_access;
};

const size_t	TABLE_SIZE = 65536;

struct ControlBlock {
	std::atomic_flag		lru_lock = ATOMIC_FLAG_INIT;
	std::atomic<size_t>		lru_head;
	std::atomic<size_t>		lru_tail;
	std::atomic<size_t>		lru_next[TABLE_SIZE];
};

struct LimiterConfig {
	int32_t			max_tokens = 10;
	uint64_t		refill_ms = 1000;
	EvictionPolicy	policy = EvictionPolicy::LRU;
	size_t			table_size = TABLE_SIZE;
};

extern ControlBlock* ctrl;

bool	init_limiter(int32_t max_tokens, uint64_t refill_ms, const char** err_msg);
bool	init_limiter(const LimiterConfig& config, const char** err_msg);
bool	consume_token(const char* ip_str, int32_t max_tokens, uint64_t refill_ms);
void	cleanup_limiter();

struct LimiterStats {
	size_t total_requests = 0;
	size_t tokens_consumed = 0;
	size_t evictions = 0;
};

void	lru_init();
size_t	lru_get_head();
size_t	lru_get_tail();
void	lru_touch(size_t idx);

#endif