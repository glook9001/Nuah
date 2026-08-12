#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <memory.h>
#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>

struct pthread_bridge_metrics {
	uint64_t mapping_probes;
	uint64_t mincore_calls;
	uint64_t mincore_failures;
	uint64_t mutex_tagged;
	uint64_t mutex_probes;
	uint64_t cond_tagged;
	uint64_t cond_probes;
	uint64_t sem_tagged;
	uint64_t sem_probes;
	uint64_t rwlock_tagged;
	uint64_t rwlock_probes;
	uint64_t mmap_calls;
	uint64_t mmap_failures;
	uint64_t munmap_calls;
	uint64_t munmap_failures;
	uint64_t cond_wait_calls;
	uint64_t sem_wait_calls;
	uint64_t rwlock_wait_calls;
	uint64_t wait_time_ns;
	uint64_t wait_over_1ms;
	uint64_t wait_over_16ms;
	uint64_t wait_time_max_ns;
	uint64_t wait_timeouts;
};

static struct pthread_bridge_metrics pthread_bridge_metrics;
static int pthread_bridge_metrics_enabled;
static int pthread_bridge_slow_trace_enabled;
static uint64_t pthread_bridge_slow_trace_count;
static __thread const char *pthread_bridge_wait_kind;
static __thread void *pthread_bridge_wait_caller;
static inline void pthread_bridge_metric_inc(uint64_t *counter);
static void pthread_bridge_metrics_report(void);
static void pthread_bridge_metrics_signal(int signal_number);

// when __GLIBC__ (or some glibc specific symbol) is not defined, we're assuming musl; can't check to be sure because 🤡

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_RECURSIVE}}
#endif

#ifndef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_ERRORCHECK}}
#endif

typedef struct {
	union {
		struct {
			unsigned int count;
#ifdef __LP64__
			int __reserved[3];
#endif
		} bionic;
		sem_t *glibc;
	};
} bionic_sem_t;

typedef struct {
	union {
		struct {
			uint32_t flags;
			void* stack_base;
			size_t stack_size;
			size_t guard_size;
			int32_t sched_policy;
			int32_t sched_priority;
#ifdef __LP64__
			char __reserved[16];
#endif
		} bionic;
		pthread_attr_t *glibc;
	};
} bionic_attr_t;

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[10];
#else
		int32_t __private[1];
#endif
		pthread_mutex_t *glibc;
	};
} bionic_mutex_t;

static bool is_mapped(void *mem, const size_t sz);

/* mmap() returns page-aligned addresses, so the low bit is available as a
 * private initialized marker in the out-of-line host mutex pointer. Android
 * owns the surrounding bionic_mutex_t bytes and never observes this pointer;
 * only this wrapper dereferences it. The old path called mincore() for every
 * lock/unlock to rediscover the same mapping, which is expensive in Roblox's
 * hot worker and looper paths. */
#define BIONIC_MUTEX_MAPPED_TAG ((uintptr_t)1)

static inline pthread_mutex_t *mutex_native(const bionic_mutex_t *mutex)
{
	return (pthread_mutex_t *)((uintptr_t)mutex->glibc &
	                           ~BIONIC_MUTEX_MAPPED_TAG);
}

static inline bool mutex_is_mapped(const bionic_mutex_t *mutex)
{
	const uintptr_t raw = (uintptr_t)mutex->glibc;
	if (raw & BIONIC_MUTEX_MAPPED_TAG) {
		pthread_bridge_metric_inc(&pthread_bridge_metrics.mutex_tagged);
		return true;
	}
	pthread_bridge_metric_inc(&pthread_bridge_metrics.mutex_probes);
	return mutex->glibc && is_mapped(mutex->glibc, sizeof(*mutex));
}

static inline void mutex_set_native(bionic_mutex_t *mutex,
					    pthread_mutex_t *native)
{
	if (native == (pthread_mutex_t *)MAP_FAILED) {
		mutex->glibc = NULL;
		return;
	}
	assert(((uintptr_t)native & BIONIC_MUTEX_MAPPED_TAG) == 0);
	mutex->glibc = (pthread_mutex_t *)((uintptr_t)native |
						  BIONIC_MUTEX_MAPPED_TAG);
}

#define INIT_MUTEX_IF_NOT_MAPPED(x, init) \
	do { if (!mutex_is_mapped(x)) init(x); } while (0)

typedef struct {
	union {
		long __private;
		pthread_mutexattr_t *glibc;
	};
} bionic_mutexattr_t;

typedef struct {
	union {
		#if defined(__LP64__)
		  int32_t __private[14];
		#else
		  int32_t __private[10];
		#endif
		pthread_rwlock_t *glibc;
	};
} bionic_rwlock_t;

static const struct {
	bionic_mutex_t bionic;
	pthread_mutex_t glibc;
} bionic_mutex_init_map[] = {
	{ .bionic = {{{ ((PTHREAD_MUTEX_NORMAL & 3) << 14) }}}, .glibc = PTHREAD_MUTEX_INITIALIZER },
	{ .bionic = {{{ ((PTHREAD_MUTEX_RECURSIVE & 3) << 14) }}}, .glibc = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP },
	{ .bionic = {{{ ((PTHREAD_MUTEX_ERRORCHECK & 3) << 14) }}}, .glibc = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP },
};

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[12];
#else
		int32_t __private[1];
#endif
		pthread_cond_t *glibc;
	};
} bionic_cond_t;

typedef struct {
	union {
		long __private;
		pthread_condattr_t *glibc;
	};
} bionic_condattr_t;

typedef int bionic_key_t;
_Static_assert(sizeof(bionic_key_t) == sizeof(pthread_key_t), "bionic_key_t and pthread_key_t size mismatch");

typedef int bionic_once_t;
_Static_assert(sizeof(bionic_once_t) == sizeof(pthread_once_t), "bionic_once_t and pthread_once_t size mismatch");

typedef long bionic_pthread_t; // seems to be 32bit on 32bit musl, 64bit everywhere else, which is why long happens to work
_Static_assert(sizeof(bionic_pthread_t) == sizeof(pthread_t), "bionic_pthread_t and pthread_t size mismatch");

typedef uint64_t bionic_rwlockattr_t;
_Static_assert(sizeof(bionic_rwlockattr_t) == sizeof(pthread_rwlockattr_t), "bionic_rwlockattr_t and pthread_rwlockattr_t size mismatch");

struct bionic_pthread_cleanup_t {
	union {
		struct bionic_pthread_cleanup_t *prev;
#ifdef __GLIBC__
		__pthread_unwind_buf_t *glibc;
#else
		struct __ptcb *musl;
#endif
	};
	void (*routine)(void*);
	void *arg;
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

/*
 * The bridge is normally completely silent.  Set NUAH_PTHREAD_TRACE=1 for a
 * process-level summary at exit.  This is deliberately a counter-only probe:
 * it does not print from hot paths and it does not change synchronization
 * semantics.  The timing counters are enabled only in trace mode, so normal
 * launches do not pay for clock_gettime().
 */
static inline void pthread_bridge_metric_inc(uint64_t *counter)
{
	if (__builtin_expect(pthread_bridge_metrics_enabled, 0))
		__atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static inline void pthread_bridge_metric_add(uint64_t *counter, uint64_t value)
{
	if (__builtin_expect(pthread_bridge_metrics_enabled, 0))
		__atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static inline void pthread_bridge_metric_max(uint64_t *counter, uint64_t value)
{
	if (__builtin_expect(!pthread_bridge_metrics_enabled, 1))
		return;
	uint64_t current = __atomic_load_n(counter, __ATOMIC_RELAXED);
	while (current < value &&
	       !__atomic_compare_exchange_n(counter, &current, value, false,
	                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		/* current is refreshed by compare_exchange_n. */
	}
}

static uint64_t pthread_bridge_clock_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)ts.tv_nsec;
}

static uint64_t pthread_bridge_wait_begin(uint64_t *counter,
                                         const char *kind,
                                         void *caller)
{
	if (__builtin_expect(!pthread_bridge_metrics_enabled, 1))
		return 0;
	pthread_bridge_wait_kind = kind;
	pthread_bridge_wait_caller = caller;
	pthread_bridge_metric_inc(counter);
	return pthread_bridge_clock_ns();
}

static void pthread_bridge_wait_end(uint64_t started, int result)
{
	if (__builtin_expect(!pthread_bridge_metrics_enabled || !started, 1))
		return;
	const uint64_t elapsed = pthread_bridge_clock_ns() - started;
	pthread_bridge_metric_add(&pthread_bridge_metrics.wait_time_ns, elapsed);
	pthread_bridge_metric_max(&pthread_bridge_metrics.wait_time_max_ns, elapsed);
	if (elapsed >= UINT64_C(1000000))
		pthread_bridge_metric_inc(&pthread_bridge_metrics.wait_over_1ms);
	if (elapsed >= UINT64_C(16000000))
		pthread_bridge_metric_inc(&pthread_bridge_metrics.wait_over_16ms);
	if (result == ETIMEDOUT)
		pthread_bridge_metric_inc(&pthread_bridge_metrics.wait_timeouts);
	if (pthread_bridge_slow_trace_enabled && elapsed >= UINT64_C(16000000)) {
		const uint64_t sequence = __atomic_fetch_add(
		    &pthread_bridge_slow_trace_count, 1, __ATOMIC_RELAXED);
		/* Keep the diagnostic bounded: it is only for identifying the Roblox
		 * call sites behind the long waits, never a production logging path. */
		if (sequence < 256) {
			Dl_info info = {0};
			const bool resolved = pthread_bridge_wait_caller &&
			                     dladdr(pthread_bridge_wait_caller, &info) != 0;
			const uintptr_t module_offset =
				resolved && info.dli_fbase
					? (uintptr_t)pthread_bridge_wait_caller -
					      (uintptr_t)info.dli_fbase
					: 0;
			fprintf(stderr,
			        "bionic pthread slow-wait kind=%s elapsed_ms=%.3f "
			        "caller=%p module=%s offset=0x%llx result=%d\n",
			        pthread_bridge_wait_kind ? pthread_bridge_wait_kind :
			                                    "unknown",
			        (double)elapsed / 1000000.0, pthread_bridge_wait_caller,
			        resolved && info.dli_fname ? info.dli_fname : "unknown",
			        (unsigned long long)module_offset, result);
		}
	}
}

static void *pthread_bridge_mmap(size_t size)
{
	void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
	                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	pthread_bridge_metric_inc(&pthread_bridge_metrics.mmap_calls);
	if (memory == MAP_FAILED)
		pthread_bridge_metric_inc(&pthread_bridge_metrics.mmap_failures);
	return memory;
}

static int pthread_bridge_munmap(void *memory, size_t size)
{
	const int result = munmap(memory, size);
	pthread_bridge_metric_inc(&pthread_bridge_metrics.munmap_calls);
	if (result != 0)
		pthread_bridge_metric_inc(&pthread_bridge_metrics.munmap_failures);
	return result;
}

static void pthread_bridge_metrics_report(void)
{
	if (!pthread_bridge_metrics_enabled)
		return;
	fprintf(stderr,
	        "bionic pthread metrics: probes=%llu mincore=%llu failed=%llu "
	        "tagged{mutex=%llu cond=%llu sem=%llu rwlock=%llu} "
	        "fallback{mutex=%llu cond=%llu sem=%llu rwlock=%llu} "
	        "maps=%llu map_failed=%llu unmaps=%llu unmap_failed=%llu "
	        "waits{cond=%llu sem=%llu rwlock=%llu >1ms=%llu >16ms=%llu "
	        "timeouts=%llu total_ms=%.3f max_ms=%.3f}\n",
	        (unsigned long long)pthread_bridge_metrics.mapping_probes,
	        (unsigned long long)pthread_bridge_metrics.mincore_calls,
	        (unsigned long long)pthread_bridge_metrics.mincore_failures,
	        (unsigned long long)pthread_bridge_metrics.mutex_tagged,
	        (unsigned long long)pthread_bridge_metrics.cond_tagged,
	        (unsigned long long)pthread_bridge_metrics.sem_tagged,
	        (unsigned long long)pthread_bridge_metrics.rwlock_tagged,
	        (unsigned long long)pthread_bridge_metrics.mutex_probes,
	        (unsigned long long)pthread_bridge_metrics.cond_probes,
	        (unsigned long long)pthread_bridge_metrics.sem_probes,
	        (unsigned long long)pthread_bridge_metrics.rwlock_probes,
	        (unsigned long long)pthread_bridge_metrics.mmap_calls,
	        (unsigned long long)pthread_bridge_metrics.mmap_failures,
	        (unsigned long long)pthread_bridge_metrics.munmap_calls,
	        (unsigned long long)pthread_bridge_metrics.munmap_failures,
	        (unsigned long long)pthread_bridge_metrics.cond_wait_calls,
	        (unsigned long long)pthread_bridge_metrics.sem_wait_calls,
	        (unsigned long long)pthread_bridge_metrics.rwlock_wait_calls,
	        (unsigned long long)pthread_bridge_metrics.wait_over_1ms,
	        (unsigned long long)pthread_bridge_metrics.wait_over_16ms,
	        (unsigned long long)pthread_bridge_metrics.wait_timeouts,
	        (double)pthread_bridge_metrics.wait_time_ns / 1000000.0,
	        (double)pthread_bridge_metrics.wait_time_max_ns / 1000000.0);
}

static void pthread_bridge_metrics_signal(int signal_number)
{
	if (signal_number == SIGUSR1)
		pthread_bridge_metrics_report();
}

__attribute__((constructor)) static void pthread_bridge_metrics_init(void)
{
	const char *trace = getenv("NUAH_PTHREAD_TRACE");
	const char *slow = getenv("NUAH_PTHREAD_SLOW_TRACE");
	const bool trace_enabled =
		trace && *trace && !(trace[0] == '0' && trace[1] == '\0');
	pthread_bridge_slow_trace_enabled =
		slow && *slow && !(slow[0] == '0' && slow[1] == '\0');
	if (!trace_enabled && !pthread_bridge_slow_trace_enabled)
		return;
	pthread_bridge_metrics_enabled = 1;
	atexit(pthread_bridge_metrics_report);
	/* Nuah's native child exits with _exit(), so atexit is not guaranteed. */
	signal(SIGUSR1, pthread_bridge_metrics_signal);
}

// For checking, if our glibc version is mapped to memory.
// Used for sanity checking and static initialization below.
#define IS_MAPPED(x) is_mapped(x->glibc, sizeof(*x))

// For handling static initialization.
#define INIT_IF_NOT_MAPPED(x, init) do { if (!IS_MAPPED(x)) init(x); } while(0)

static bool is_mapped(void *mem, const size_t sz)
{
	pthread_bridge_metric_inc(&pthread_bridge_metrics.mapping_probes);
	const size_t ps = sysconf(_SC_PAGESIZE);
	assert(ps > 0);
	unsigned char vec[(sz + ps - 1) / ps];
	pthread_bridge_metric_inc(&pthread_bridge_metrics.mincore_calls);
	const bool mapped = !mincore(mem, sz, vec);
	if (!mapped)
		pthread_bridge_metric_inc(&pthread_bridge_metrics.mincore_failures);
	return mapped;
}

#define BIONIC_SYNC_MAPPED_TAG ((uintptr_t)1)

static inline sem_t *sem_native(const bionic_sem_t *sem)
{
	return (sem_t *)((uintptr_t)sem->glibc & ~BIONIC_SYNC_MAPPED_TAG);
}

static inline bool sem_is_mapped(const bionic_sem_t *sem)
{
	const uintptr_t raw = (uintptr_t)sem->glibc;
	if (raw & BIONIC_SYNC_MAPPED_TAG) {
		pthread_bridge_metric_inc(&pthread_bridge_metrics.sem_tagged);
		return true;
	}
	pthread_bridge_metric_inc(&pthread_bridge_metrics.sem_probes);
	return sem->glibc && is_mapped(sem->glibc, sizeof(*sem));
}

static inline void sem_set_native(bionic_sem_t *sem, sem_t *native)
{
	if (native == (sem_t *)MAP_FAILED) {
		sem->glibc = NULL;
		return;
	}
	assert(((uintptr_t)native & BIONIC_SYNC_MAPPED_TAG) == 0);
	sem->glibc = (sem_t *)((uintptr_t)native | BIONIC_SYNC_MAPPED_TAG);
}

static inline pthread_rwlock_t *rwlock_native(const bionic_rwlock_t *rwlock)
{
	return (pthread_rwlock_t *)((uintptr_t)rwlock->glibc &
	                            ~BIONIC_SYNC_MAPPED_TAG);
}

static inline bool rwlock_is_mapped(const bionic_rwlock_t *rwlock)
{
	const uintptr_t raw = (uintptr_t)rwlock->glibc;
	if (raw & BIONIC_SYNC_MAPPED_TAG) {
		pthread_bridge_metric_inc(&pthread_bridge_metrics.rwlock_tagged);
		return true;
	}
	pthread_bridge_metric_inc(&pthread_bridge_metrics.rwlock_probes);
	return rwlock->glibc && is_mapped(rwlock->glibc, sizeof(*rwlock));
}

static inline void rwlock_set_native(bionic_rwlock_t *rwlock,
					     pthread_rwlock_t *native)
{
	if (native == (pthread_rwlock_t *)MAP_FAILED) {
		rwlock->glibc = NULL;
		return;
	}
	assert(((uintptr_t)native & BIONIC_SYNC_MAPPED_TAG) == 0);
	rwlock->glibc = (pthread_rwlock_t *)((uintptr_t)native |
						    BIONIC_SYNC_MAPPED_TAG);
}

static inline pthread_cond_t *cond_native(const bionic_cond_t *cond)
{
	return (pthread_cond_t *)((uintptr_t)cond->glibc &
	                          ~BIONIC_SYNC_MAPPED_TAG);
}

static inline bool cond_is_mapped(const bionic_cond_t *cond)
{
	const uintptr_t raw = (uintptr_t)cond->glibc;
	if (raw & BIONIC_SYNC_MAPPED_TAG) {
		pthread_bridge_metric_inc(&pthread_bridge_metrics.cond_tagged);
		return true;
	}
	pthread_bridge_metric_inc(&pthread_bridge_metrics.cond_probes);
	return cond->glibc && is_mapped(cond->glibc, sizeof(*cond));
}

static inline void cond_set_native(bionic_cond_t *cond, pthread_cond_t *native)
{
	if (native == (pthread_cond_t *)MAP_FAILED) {
		cond->glibc = NULL;
		return;
	}
	assert(((uintptr_t)native & BIONIC_SYNC_MAPPED_TAG) == 0);
	cond->glibc = (pthread_cond_t *)((uintptr_t)native |
						BIONIC_SYNC_MAPPED_TAG);
}

void bionic___pthread_cleanup_push(struct bionic_pthread_cleanup_t *c, void (*routine)(void*), void *arg)
{
	assert(c && routine);
#ifdef __GLIBC__
	c->glibc = pthread_bridge_mmap(sizeof(*c->glibc));
	c->routine = routine;
	c->arg = arg;

	int not_first_call;
	if ((not_first_call = sigsetjmp((struct __jmp_buf_tag*)(void*)c->glibc->__cancel_jmp_buf, 0))) {
		routine(arg);
		__pthread_unwind_next(c->glibc);
	}

	__pthread_register_cancel(c->glibc);
#else
	c->musl = pthread_bridge_mmap(sizeof(struct __ptcb));
	c->routine = routine;
	c->arg = arg;
	_pthread_cleanup_push(c->musl, routine, arg);
#endif
}

void bionic___pthread_cleanup_pop(struct bionic_pthread_cleanup_t *c, int execute)
{
#ifdef __GLIBC__
	assert(c && IS_MAPPED(c)); // TODO - analogically for musl?
	__pthread_unregister_cancel(c->glibc);

	if (execute)
		c->routine(c->arg);

	pthread_bridge_munmap(c->glibc, sizeof(*c->glibc));
#else
	_pthread_cleanup_pop(c->musl, execute);
	pthread_bridge_munmap(c->musl, sizeof(struct __ptcb));
#endif
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* sem */

int bionic_sem_destroy(bionic_sem_t *sem)
{
	assert(sem);
	int ret = 0;
	if (sem_is_mapped(sem)) {
		sem_t *native = sem_native(sem);
		ret = sem_destroy(native);
		sem->glibc = NULL;
		pthread_bridge_munmap(native, sizeof(*native));
	}
	return ret;
}

static void default_sem_init(bionic_sem_t *sem)
{
	// Apparently some android apps (hearthstone) do not call sem_init()
	assert(sem);
	sem_t *native = pthread_bridge_mmap(sizeof(*native));
	sem_set_native(sem, native);
	if (sem->glibc)
		memset(sem_native(sem), 0, sizeof(*native));
}

int bionic_sem_init(bionic_sem_t *sem, int pshared, unsigned int value)
{
	assert(sem);
	// From SEM_INIT(3)
	// Initializing a semaphore that has already been initialized results in underined behavior.
	*sem = (bionic_sem_t){0};
	sem_t *native = pthread_bridge_mmap(sizeof(*native));
	sem_set_native(sem, native);
	return sem->glibc ? sem_init(sem_native(sem), pshared, value) : ENOMEM;
}

int bionic_sem_post(bionic_sem_t *sem)
{
	assert(sem);
	if (!sem_is_mapped(sem))
		default_sem_init(sem);
	return sem->glibc ? sem_post(sem_native(sem)) : ENOMEM;
}

int bionic_sem_wait(bionic_sem_t *sem)
{
	assert(sem);
	if (!sem_is_mapped(sem))
		default_sem_init(sem);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.sem_wait_calls, "sem", __builtin_return_address(0));
	const int result = sem->glibc ? sem_wait(sem_native(sem)) : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

int bionic_sem_trywait(bionic_sem_t *sem)
{
	assert(sem);
	if (!sem_is_mapped(sem))
		default_sem_init(sem);
	return sem->glibc ? sem_trywait(sem_native(sem)) : ENOMEM;
}

int bionic_sem_timedwait(bionic_sem_t *sem, const struct timespec *abs_timeout)
{
	assert(sem && abs_timeout);
	if (!sem_is_mapped(sem))
		default_sem_init(sem);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.sem_wait_calls, "sem", __builtin_return_address(0));
	const int result = sem->glibc
		                   ? sem_timedwait(sem_native(sem), abs_timeout)
		                   : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* rwlock */

static bool pthread_bridge_rwlock_writer_policy(void)
{
	const char *value = getenv("NUAH_PTHREAD_RWLOCK_POLICY");
	return !value || !*value || strcmp(value, "writer") == 0;
}

static int pthread_bridge_rwlock_init_native(
	pthread_rwlock_t *native, const bionic_rwlockattr_t *attr)
{
	/* An explicit Android attr must remain untouched. For the common default
	 * attr, prefer writers like Android's lock implementation; glibc's default
	 * reader preference can leave a data-model writer behind a reader stream. */
	if (attr || !pthread_bridge_rwlock_writer_policy())
		return pthread_rwlock_init(native, (pthread_rwlockattr_t *)attr);
	pthread_rwlockattr_t host_attr;
	if (pthread_rwlockattr_init(&host_attr) != 0)
		return pthread_rwlock_init(native, NULL);
	int result = pthread_rwlockattr_setkind_np(
		&host_attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	if (result == 0)
		result = pthread_rwlock_init(native, &host_attr);
	else
		result = pthread_rwlock_init(native, NULL);
	pthread_rwlockattr_destroy(&host_attr);
	return result;
}

int bionic_pthread_rwlock_destroy(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	int ret = 0;
	if (rwlock_is_mapped(rwlock)) {
		pthread_rwlock_t *native = rwlock_native(rwlock);
		ret = pthread_rwlock_destroy(native);
		rwlock->glibc = NULL;
		pthread_bridge_munmap(native, sizeof(*native));
	}
	return ret;

}

static void default_rwlock_init(bionic_rwlock_t *rwlock)
{
	// Apparently some android apps/libs (Qt5) do not call pthread_rwlock_init()
	assert(rwlock);
	pthread_rwlock_t *native = pthread_bridge_mmap(sizeof(*native));
	rwlock_set_native(rwlock, native);
	if (rwlock->glibc)
		(void)pthread_bridge_rwlock_init_native(rwlock_native(rwlock), NULL);
}

int bionic_pthread_rwlock_init(bionic_rwlock_t *restrict rwlock, const bionic_rwlockattr_t *restrict attr)
{
	assert(rwlock);
	*rwlock = (bionic_rwlock_t){0};
	pthread_rwlock_t *native = pthread_bridge_mmap(sizeof(*native));
	rwlock_set_native(rwlock, native);
	return rwlock->glibc ? pthread_bridge_rwlock_init_native(
		                       rwlock_native(rwlock), attr)
	                     : ENOMEM;
}

int bionic_pthread_rwlock_rdlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	if (!rwlock_is_mapped(rwlock))
		default_rwlock_init(rwlock);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.rwlock_wait_calls, "rwlock",
		__builtin_return_address(0));
	const int result = rwlock->glibc
		                   ? pthread_rwlock_rdlock(rwlock_native(rwlock))
		                   : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

int bionic_pthread_rwlock_unlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	if (!rwlock_is_mapped(rwlock))
		default_rwlock_init(rwlock);
	return rwlock->glibc ? pthread_rwlock_unlock(rwlock_native(rwlock)) : ENOMEM;
}

int bionic_pthread_rwlock_wrlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	if (!rwlock_is_mapped(rwlock))
		default_rwlock_init(rwlock);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.rwlock_wait_calls, "rwlock",
		__builtin_return_address(0));
	const int result = rwlock->glibc
		                   ? pthread_rwlock_wrlock(rwlock_native(rwlock))
		                   : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* attr */

int bionic_pthread_attr_destroy(bionic_attr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_attr_destroy(attr->glibc);
		pthread_bridge_munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_attr_init(bionic_attr_t *attr)
{
	assert(attr);
	// From PTHREAD_ATTR_INIT(3)
	// Calling `pthread_attr_init` on a thread attributes object that has already been initialized results in ud.
	*attr = (bionic_attr_t){0};
	attr->glibc = pthread_bridge_mmap(sizeof(*attr->glibc));
	return pthread_attr_init(attr->glibc);
}

int bionic_pthread_getattr_np(bionic_pthread_t thread, bionic_attr_t *attr)
{
	assert(thread && attr);
	*attr = (bionic_attr_t){0};
	attr->glibc = pthread_bridge_mmap(sizeof(*attr->glibc));
	return pthread_getattr_np((pthread_t)thread, attr->glibc);
}

int bionic_pthread_attr_settstack(bionic_attr_t *attr, void *stackaddr, size_t stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setstack(attr->glibc, stackaddr, stacksize);
}

int bionic_pthread_attr_getstack(const bionic_attr_t *attr, void *stackaddr, size_t *stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getstack(attr->glibc, stackaddr, stacksize);
}

int bionic_pthread_attr_setstacksize(bionic_attr_t *attr, size_t stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setstacksize(attr->glibc, stacksize);
}

int bionic_pthread_attr_getstacksize(const bionic_attr_t *attr, size_t *stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getstacksize(attr->glibc, stacksize);
}

int bionic_pthread_attr_setschedpolicy(bionic_attr_t *attr, int policy)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setschedpolicy(attr->glibc, policy);
}

int bionic_pthread_attr_getschedpolicy(bionic_attr_t *attr, int *policy)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getschedpolicy(attr->glibc, policy);
}

int bionic_pthread_attr_setschedparam(bionic_attr_t *attr, const struct sched_param *param)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setschedparam(attr->glibc, param);
}

int bionic_pthread_attr_getschedparam(bionic_attr_t *attr, struct sched_param *param)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getschedparam(attr->glibc, param);
}

int bionic_pthread_attr_setdetachstate(bionic_attr_t *attr, int detachstate)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setdetachstate(attr->glibc, detachstate);
}

int bionic_pthread_attr_getdetachstate(bionic_attr_t *attr, int *detachstate)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getdetachstate(attr->glibc, detachstate);
}

int bionic_pthread_create(bionic_pthread_t *thread, const bionic_attr_t *attr, void* (*start)(void*), void *arg)
{
	assert(thread && (!attr || IS_MAPPED(attr)));
	return pthread_create((pthread_t*)thread, (attr ? attr->glibc : NULL), start, arg);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutexattr */

int bionic_pthread_mutexattr_settype(bionic_mutexattr_t *attr, int type)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_mutexattr_settype(attr->glibc, type);
}

int bionic_pthread_mutexattr_destroy(bionic_mutexattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_mutexattr_destroy(attr->glibc);
		pthread_bridge_munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_mutexattr_init(bionic_mutexattr_t *attr)
{
	assert(attr);
	// From PTHREAD_MUTEXATTR_INIT(3)
	// The results of initializing an already initialized mutex attributes object are undefined.
	*attr = (bionic_mutexattr_t){0};
	attr->glibc = pthread_bridge_mmap(sizeof(*attr->glibc));
	return pthread_mutexattr_init(attr->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutex */

static void default_pthread_mutex_init(bionic_mutex_t *mutex)
{
	assert(mutex);
	/*
	 * Android's static pthread mutex initializer is defined in terms of the
	 * type bits, while the remaining bytes are implementation/padding data.
	 * Comparing the complete 40-byte object made valid Roblox mutexes fail to
	 * match whenever that padding differed between the APK and this shim.
	 * Match the stable type word only; the native pthread object is private to
	 * the host and is created below.
	 */
	const uint32_t type_word = (uint32_t)mutex->__private[0];

	for (size_t i = 0; i < ARRAY_SIZE(bionic_mutex_init_map); i++) {
		if (memcmp(&bionic_mutex_init_map[i].bionic.__private[0],
		           &type_word, sizeof(type_word)))
			continue;

		pthread_mutex_t *native = pthread_bridge_mmap(sizeof(*native));
		mutex_set_native(mutex, native);
		if (mutex->glibc)
			memcpy(mutex_native(mutex), &bionic_mutex_init_map[i].glibc,
			       sizeof(bionic_mutex_init_map[i].glibc));
		return;
	}

	/*
	 * Some Android releases add flag bits to the static initializer.  If the
	 * type is not one of the three layouts above, treating it as a normal
	 * mutex is safer than aborting the entire runtime.  Explicit
	 * pthread_mutex_init() calls still preserve their requested attributes.
	 */
	pthread_mutex_t *native = pthread_bridge_mmap(sizeof(*native));
	mutex_set_native(mutex, native);
	if (mutex->glibc)
		memset(mutex_native(mutex), 0, sizeof(*native));
	fprintf(stderr,
	        "bionic pthread: unknown static mutex initializer 0x%08x; using normal mutex\n",
	        type_word);
}

int bionic_pthread_mutex_destroy(bionic_mutex_t *mutex)
{
	assert(mutex);
	int ret = 0;
	if (mutex_is_mapped(mutex)) {
		pthread_mutex_t *native = mutex_native(mutex);
		ret = pthread_mutex_destroy(native);
		pthread_bridge_munmap(native, sizeof(*native));
		mutex->glibc = NULL;
	}
	return ret;
}

int bionic_pthread_mutex_init(bionic_mutex_t *mutex, const bionic_mutexattr_t *attr)
{
	assert(mutex && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_MUTEX_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*mutex = (bionic_mutex_t){0};
	pthread_mutex_t *native = pthread_bridge_mmap(sizeof(*native));
	mutex_set_native(mutex, native);
	return mutex->glibc ? pthread_mutex_init(mutex_native(mutex),
	                                          (attr ? attr->glibc : NULL))
	                    : ENOMEM;
}

int
bionic_pthread_mutex_lock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_MUTEX_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return mutex->glibc ? pthread_mutex_lock(mutex_native(mutex)) : ENOMEM;
}

int bionic_pthread_mutex_trylock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_MUTEX_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return mutex->glibc ? pthread_mutex_trylock(mutex_native(mutex)) : ENOMEM;
}

int bionic_pthread_mutex_unlock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_MUTEX_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return mutex->glibc ? pthread_mutex_unlock(mutex_native(mutex)) : ENOMEM;
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* condattr */

int bionic_pthread_condattr_destroy(bionic_condattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_condattr_destroy(attr->glibc);
		pthread_bridge_munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_condattr_init(bionic_condattr_t *attr)
{
	*attr = (bionic_condattr_t){0};
	attr->glibc = pthread_bridge_mmap(sizeof(*attr->glibc));
	return pthread_condattr_init(attr->glibc);
}

int bionic_pthread_condattr_setclock(bionic_condattr_t *attr, clockid_t clock_id)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_condattr_setclock(attr->glibc, clock_id);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* cond */

static void default_pthread_cond_init(bionic_cond_t *cond)
{
	assert(cond);
	pthread_cond_t *native = pthread_bridge_mmap(sizeof(*native));
	cond_set_native(cond, native);
	if (cond->glibc)
		memset(cond_native(cond), 0, sizeof(*native));
}

int bionic_pthread_cond_destroy(bionic_cond_t *cond)
{
	assert(cond);
	int ret = 0;
	if (cond_is_mapped(cond)) {
		pthread_cond_t *native = cond_native(cond);
		ret = pthread_cond_destroy(native);
		pthread_bridge_munmap(native, sizeof(*native));
		cond->glibc = NULL;
	}
	return ret;
}

int bionic_pthread_cond_init(bionic_cond_t *cond, const bionic_condattr_t *attr)
{
	// SUS // assert(cond && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_COND_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*cond = (bionic_cond_t){0};
	pthread_cond_t *native = pthread_bridge_mmap(sizeof(*native));
	cond_set_native(cond, native);
	return cond->glibc ? pthread_cond_init(cond_native(cond),
	                                       (attr ? attr->glibc : NULL))
	                    : ENOMEM;
}

int bionic_pthread_cond_broadcast(bionic_cond_t *cond)
{
	assert(cond);
	if (!cond_is_mapped(cond))
		default_pthread_cond_init(cond);
	return cond->glibc ? pthread_cond_broadcast(cond_native(cond)) : ENOMEM;
}

int bionic_pthread_cond_signal(bionic_cond_t *cond)
{
	assert(cond);
	if (!cond_is_mapped(cond))
		default_pthread_cond_init(cond);
	return cond->glibc ? pthread_cond_signal(cond_native(cond)) : ENOMEM;
}

int
bionic_pthread_cond_wait(bionic_cond_t *cond, bionic_mutex_t *mutex) {
	assert(cond && mutex);
	if (!cond_is_mapped(cond))
		default_pthread_cond_init(cond);
	INIT_MUTEX_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.cond_wait_calls, "cond",
		__builtin_return_address(0));
	const int result = (cond->glibc && mutex->glibc)
		                   ? pthread_cond_wait(cond_native(cond),
		                                       mutex_native(mutex))
		                   : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

int bionic_pthread_cond_timedwait(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abs_timeout)
{
	assert(cond && mutex);
	if (!cond_is_mapped(cond))
		default_pthread_cond_init(cond);
	INIT_MUTEX_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	const uint64_t started = pthread_bridge_wait_begin(
		&pthread_bridge_metrics.cond_wait_calls, "cond",
		__builtin_return_address(0));
	const int result = (cond->glibc && mutex->glibc)
		                   ? pthread_cond_timedwait(cond_native(cond),
		                                             mutex_native(mutex),
		                                             abs_timeout)
		                   : ENOMEM;
	pthread_bridge_wait_end(started, result);
	return result;
}

int bionic_pthread_cond_timedwait_relative_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *reltime)
{
	assert(cond && mutex && reltime);
	struct timespec tv;
	clock_gettime(CLOCK_REALTIME, &tv);
	tv.tv_sec += reltime->tv_sec;
	tv.tv_nsec += reltime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	assert(cond && mutex && abstime);
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	tv.tv_sec += abstime->tv_sec;
	tv.tv_nsec += abstime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	return bionic_pthread_cond_timedwait_monotonic_np(cond, mutex, abstime);
}

int bionic_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
	return pthread_atfork(prepare, parent, child);
}
