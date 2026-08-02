#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "looper.h"

#ifndef ALOOPER_POLL_WAKE
#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#endif

/* ATL's host libutils::Looper has an Android-specific C++ ABI and cannot be
 * safely called from a glibc process.  GameActivity only needs the small NDK
 * looper contract, so keep one lightweight pipe-backed looper per thread. */
struct registration {
	int fd;
	int ident;
	int events;
	Looper_callbackFunc callback;
	void *data;
};

struct atl_looper {
	int wake_read;
	int wake_write;
	int refs;
	bool polling;
	pthread_mutex_t mutex;
	struct registration registrations[32];
	int registration_count;
};

static __thread struct atl_looper *thread_looper;

static struct atl_looper *new_looper(void)
{
	int pipe_fds[2];
	if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0)
		return NULL;
	struct atl_looper *looper = calloc(1, sizeof(*looper));
	if (!looper) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return NULL;
	}
	looper->wake_read = pipe_fds[0];
	looper->wake_write = pipe_fds[1];
	looper->refs = 1;
	pthread_mutex_init(&looper->mutex, NULL);
	return looper;
}

ALooper *ALooper_forThread(void)
{
	if (!thread_looper)
		thread_looper = new_looper();
	return thread_looper;
}

ALooper *ALooper_prepare(int opts)
{
	(void)opts;
	return ALooper_forThread();
}

void ALooper_acquire(ALooper *opaque)
{
	struct atl_looper *looper = opaque;
	if (looper)
		__atomic_add_fetch(&looper->refs, 1, __ATOMIC_RELAXED);
}

void ALooper_release(ALooper *opaque)
{
	struct atl_looper *looper = opaque;
	if (looper && __atomic_sub_fetch(&looper->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		close(looper->wake_read);
		close(looper->wake_write);
		pthread_mutex_destroy(&looper->mutex);
		free(looper);
	}
}

int ALooper_addFd(ALooper *opaque, int fd, int ident, int events,
                  Looper_callbackFunc callback, void *data)
{
	struct atl_looper *looper = opaque;
	if (!looper || fd < 0 || looper->registration_count >= 32)
		return 0;
	pthread_mutex_lock(&looper->mutex);
	struct registration *registration =
		&looper->registrations[looper->registration_count++];
	*registration = (struct registration){fd, ident, events, callback, data};
	pthread_mutex_unlock(&looper->mutex);
	return 1;
}

int ALooper_removeFd(ALooper *opaque, int fd)
{
	struct atl_looper *looper = opaque;
	if (!looper)
		return 0;
	pthread_mutex_lock(&looper->mutex);
	for (int i = 0; i < looper->registration_count; ++i) {
		if (looper->registrations[i].fd == fd) {
			looper->registrations[i] =
				looper->registrations[--looper->registration_count];
			pthread_mutex_unlock(&looper->mutex);
			return 1;
		}
	}
	pthread_mutex_unlock(&looper->mutex);
	return 0;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
	struct atl_looper *looper = ALooper_forThread();
	if (!looper)
		return 0;
	struct pollfd poll_fds[33];
	struct registration registrations[32];
	poll_fds[0] = (struct pollfd){looper->wake_read, POLLIN, 0};
	pthread_mutex_lock(&looper->mutex);
	int count = looper->registration_count;
	for (int i = 0; i < count; ++i) {
		registrations[i] = looper->registrations[i];
		poll_fds[i + 1] = (struct pollfd){registrations[i].fd,
		                                 registrations[i].events, 0};
	}
	pthread_mutex_unlock(&looper->mutex);
	looper->polling = true;
	int result = poll(poll_fds, (nfds_t)count + 1, timeoutMillis);
	looper->polling = false;
	if (result <= 0)
		return result;
	if (poll_fds[0].revents) {
		char buffer[64];
		while (read(looper->wake_read, buffer, sizeof(buffer)) > 0) {}
		return ALOOPER_POLL_WAKE;
	}
	for (int i = 0; i < count; ++i) {
		if (!poll_fds[i + 1].revents)
			continue;
		if (outFd) *outFd = registrations[i].fd;
		if (outEvents) *outEvents = poll_fds[i + 1].revents;
		if (outData) *outData = registrations[i].data;
		if (registrations[i].callback)
			return registrations[i].callback(registrations[i].fd,
			                                 poll_fds[i + 1].revents,
			                                 registrations[i].data);
		return registrations[i].ident;
	}
	return ALOOPER_POLL_CALLBACK;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
	return ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
}

void ALooper_wake(ALooper *opaque)
{
	struct atl_looper *looper = opaque;
	if (looper) {
		const uint8_t byte = 1;
		(void)write(looper->wake_write, &byte, sizeof(byte));
	}
}

bool ALooper_isPolling(ALooper *opaque)
{
	struct atl_looper *looper = opaque;
	return looper && looper->polling;
}
