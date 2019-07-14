#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/inotify.h>
#include <libudev.h>

#include <wayland-client.h>
#include <wayland-util.h>

#include <sequential.h>
#include <os/buffer.h>
#include <os/file.h>
#include <client.h>

/*************************************************************
 *              event queue implementation                   *
 *************************************************************/
struct tw_event_source {
	struct wl_list link;
	struct epoll_event poll_event;
	struct tw_event event;
	void (* pre_hook) (struct tw_event_source *);
	void (* close)(struct tw_event_source *);
	int fd;//timer, or inotify fd
	union {
		int wd;//the actual watch id, we create one inotify per source.
		struct udev_monitor *mon;
	};

};

static void close_fd(struct tw_event_source *s)
{
	close(s->fd);
}


static struct tw_event_source*
alloc_event_source(struct tw_event *e, uint32_t mask, int fd)
{
	struct tw_event_source *event_source = malloc(sizeof(struct tw_event_source));
	wl_list_init(&event_source->link);
	event_source->event = *e;
	event_source->poll_event.data.ptr = event_source;
	event_source->poll_event.events = mask;
	event_source->fd = fd;
	event_source->pre_hook = NULL;
	event_source->close = close_fd;
	return event_source;
}

static inline void
destroy_event_source(struct tw_event_source *s)
{
	wl_list_remove(&s->link);
	if (s->close)
		s->close(s);
	free(s);
}

void
tw_event_queue_close(struct tw_event_queue *queue)
{
	struct tw_event_source *event_source, *next;
	wl_list_for_each_safe(event_source, next, &queue->head, link) {
		epoll_ctl(queue->pollfd, EPOLL_CTL_DEL, event_source->fd, NULL);
		destroy_event_source(event_source);
	}
	close(queue->pollfd);
}

void
tw_event_queue_run(struct tw_event_queue *queue)
{
	struct epoll_event events[32];
	struct tw_event_source *event_source;

	//poll->produce-event-or-timeout
	while (!queue->quit) {
		if (queue->wl_display) {
			wl_display_flush(queue->wl_display);
		}
		int count = epoll_wait(queue->pollfd, events, 32, -1);
		//right now if we run into any trouble, we just quit, I don't
		//think it is a good idea
		queue->quit = queue->quit && (count != -1);

		for (int i = 0; i < count; i++) {
			event_source = events[i].data.ptr;
			if (event_source->pre_hook)
				event_source->pre_hook(event_source);
			int output = event_source->event.cb(
				&event_source->event,
				event_source->fd);
			if (output == TW_EVENT_DEL)
				destroy_event_source(event_source);
		}

	}
	tw_event_queue_close(queue);
	return;
}

bool
tw_event_queue_init(struct tw_event_queue *queue)
{
	int fd = epoll_create1(EPOLL_CLOEXEC);
	if (fd == -1)
		return false;
	wl_list_init(&queue->head);

	queue->pollfd = fd;
	queue->quit = false;
	return true;
}


//////////////////////// INOTIFY //////////////////////////////

static void
read_inotify(struct tw_event_source *s)
{
	struct inotify_event events[100];
	read(s->fd, events, sizeof(events));
}

static void
close_inotify_watch(struct tw_event_source *s)
{
	inotify_rm_watch(s->fd, s->wd);
	close(s->fd);
}


bool
tw_event_queue_add_file(struct tw_event_queue *queue, const char *path,
			struct tw_event *e, uint32_t mask)
{
	if (!mask)
		mask = IN_MODIFY | IN_DELETE;
	if (!is_file_exist(path))
		return false;
	int fd = inotify_init1(IN_CLOEXEC);
	struct tw_event_source *s = alloc_event_source(e, EPOLLIN | EPOLLET, fd);
	s->close = close_inotify_watch;
	s->pre_hook = read_inotify;

	if (epoll_ctl(queue->pollfd, EPOLL_CTL_ADD, fd, &s->poll_event)) {
		destroy_event_source(s);
		return false;
	}
	s->wd = inotify_add_watch(fd, path, mask);
	return true;
}


//////////////////// GENERAL SOURCE ////////////////////////////

bool
tw_event_queue_add_source(struct tw_event_queue *queue, int fd,
			  struct tw_event *e, uint32_t mask)
{
	if (!mask)
		mask = EPOLLIN | EPOLLET;
	struct tw_event_source *s = alloc_event_source(e, mask, fd);
	wl_list_insert(&queue->head, &s->link);
	/* s->pre_hook = read_inotify; */

	if (epoll_ctl(queue->pollfd, EPOLL_CTL_ADD, fd, &s->poll_event)) {
		destroy_event_source(s);
		return false;
	}
	e->cb(e->data, s->fd);
	return true;
}

/////////////////////////// TIMER //////////////////////////////

static void
read_timer(struct tw_event_source *s)
{
	uint64_t nhit;
	read(s->fd, &nhit, 8);
}

bool
tw_event_queue_add_timer(struct tw_event_queue *queue,
			 const struct itimerspec *spec, struct tw_event *e)
{
	int fd;
	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (!fd)
		goto err;
	if (timerfd_settime(fd, 0, spec, NULL))
		goto err_settime;
	struct tw_event_source *s = alloc_event_source(e, EPOLLIN | EPOLLET, fd);
	s->pre_hook = read_timer;
	wl_list_insert(&queue->head, &s->link);
	//you ahve to read the timmer.
	if (epoll_ctl(queue->pollfd, EPOLL_CTL_ADD, fd, &s->poll_event))
		goto err_add;

	return true;

err_add:
	destroy_event_source(s);
err_settime:
	close(fd);
err:
	return false;
}

//////////////////////// WL_DISPLAY ////////////////////////////

static int
dispatch_wl_display(struct tw_event *e, int fd)
{
	(void)fd;
	struct tw_event_queue *queue = e->data;
	struct wl_display *display = queue->wl_display;
	while (wl_display_prepare_read(display) != 0)
		wl_display_dispatch_pending(display);
	wl_display_flush(display);
	if (wl_display_read_events(display) == -1)
		queue->quit = true;
	//this quit is kinda different
	if (wl_display_dispatch_pending(display) == -1)
		queue->quit = true;
	wl_display_flush(display);
	return TW_EVENT_NOOP;
}

bool
tw_event_queue_add_wl_display(struct tw_event_queue *queue, struct wl_display *display)
{
	int fd = wl_display_get_fd(display);
	queue->wl_display = display;
	struct tw_event dispatch_display = {
		.data = queue,
		.cb = dispatch_wl_display,
	};
	struct tw_event_source *s = alloc_event_source(&dispatch_display, EPOLLIN | EPOLLET, fd);
	//don't close wl_display in the end
	s->close = NULL;
	wl_list_insert(&queue->head, &s->link);

	if (epoll_ctl(queue->pollfd, EPOLL_CTL_ADD, fd, &s->poll_event)) {
		destroy_event_source(s);
		return false;
	}
	return true;
}
