#include "first.h"

#include "fdevent_impl.h"
#include "fdevent.h"
#include "buffer.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef FDEVENT_USE_POLL

# ifdef HAVE_POLL_H
#  include <poll.h>
# else
#  include <sys/poll.h>
# endif

__attribute_cold__
static void fdevent_poll_free(fdevents *ev) {
	free(ev->pollfds);
	if (ev->unused.ptr) free(ev->unused.ptr);
}

static int fdevent_poll_event_del(fdevents *ev, fdnode *fdn) {
	int fd = fdn->fd;
	int k = fdn->fde_ndx;
	if ((uint32_t)k >= ev->used || ev->pollfds[k].fd != fd) return (errno = EINVAL, -1);

		ev->pollfds[k].fd = -1;
		/* ev->pollfds[k].events = 0; */
		/* ev->pollfds[k].revents = 0; */

		if (ev->unused.size == ev->unused.used) {
			ev->unused.size += 16;
			ev->unused.ptr = realloc(ev->unused.ptr, sizeof(*(ev->unused.ptr)) * ev->unused.size);
			force_assert(NULL != ev->unused.ptr);
		}

		ev->unused.ptr[ev->unused.used++] = k;

	return 0;
}

static int fdevent_poll_event_set(fdevents *ev, fdnode *fdn, int events) {
	int fd = fdn->fd;
	int k = fdn->fde_ndx;

      #ifndef POLLRDHUP
	events &= ~FDEVENT_RDHUP;
      #endif

	if (k >= 0) {
		if ((uint32_t)k >= ev->used || ev->pollfds[k].fd != fd) return (errno = EINVAL, -1);
		ev->pollfds[k].events = events;
		return 0;
	}

	if (ev->unused.used > 0) {
		k = ev->unused.ptr[--ev->unused.used];

	} else {
		if (ev->size == ev->used) {
			ev->size += 16;
			ev->pollfds = realloc(ev->pollfds, sizeof(*ev->pollfds) * ev->size);
			force_assert(NULL != ev->pollfds);
		}

		k = ev->used++;
	}

	fdn->fde_ndx = k;
	ev->pollfds[k].fd = fd;
	ev->pollfds[k].events = events;

	return 0;
}

static int fdevent_poll_poll(fdevents *ev, int timeout_ms) {
    struct pollfd * const restrict pfds = ev->pollfds;
    fdnode ** const fdarray = ev->fdarray;
    const int n = poll(pfds, ev->used, timeout_ms);
    for (int i = 0, m = 0; m < n; ++i) {
        if (0 == pfds[i].revents) continue;
        fdnode *fdn = fdarray[pfds[i].fd];
        if (0 == ((uintptr_t)fdn & 0x3))
            (*fdn->handler)(fdn->ctx, pfds[i].revents);
        ++m;
    }
    return n;
}

__attribute_cold__
int fdevent_poll_init(fdevents *ev) {
	force_assert(POLLIN    == FDEVENT_IN);
	force_assert(POLLPRI   == FDEVENT_PRI);
	force_assert(POLLOUT   == FDEVENT_OUT);
	force_assert(POLLERR   == FDEVENT_ERR);
	force_assert(POLLHUP   == FDEVENT_HUP);
	force_assert(POLLNVAL  == FDEVENT_NVAL);
      #ifdef POLLRDHUP
	force_assert(POLLRDHUP == FDEVENT_RDHUP);
      #endif

	ev->type      = FDEVENT_HANDLER_POLL;
	ev->event_set = fdevent_poll_event_set;
	ev->event_del = fdevent_poll_event_del;
	ev->poll      = fdevent_poll_poll;
	ev->free      = fdevent_poll_free;
	return 0;
}

#endif
