/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/abd.h>

#include <libaio.h>
#include <linux/fs.h>

/*
 * The value is taken from SPDK. It does not scale (and perhaps should) with
 * number of vdevs in the system, we have one queue for all vdevs.
 */
#define	AIO_QUEUE_DEPTH	128

/*
 * Virtual device vector for disks accessed from userland using linux aio(7) API
 */

typedef struct vdev_disk_aio {
	int vda_fd;
} vdev_disk_aio_t;

typedef struct aio_task {
	zio_t *zio;
	void *buf;
	struct iocb iocb;
} aio_task_t;

/*
 * AIO context used for submitting AIOs and polling.
 *
 * This is currently global (per whole vdev disk aio backend) and could be
 * made per vdev if poller thread becomes a bottleneck. Disadvantage of doing
 * so is that we would need to create n poller threads (a poller for each
 * vdev) and couldn't use userspace polling (not implemented yet).
 */
io_context_t io_ctx;
volatile boolean_t stop_polling;
volatile uintptr_t poller_tid = 0;

/*
 * We probably can't do anything better from userland than opening the device
 * to prevent it from going away. So hold and rele are noops.
 */
static void
vdev_disk_aio_hold(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static void
vdev_disk_aio_rele(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static int
vdev_disk_aio_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
	vdev_disk_aio_t *vda;
	unsigned short isrot = 0;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Reopen the device if it's not currently open.  Otherwise,
	 * just update the physical size of the device.
	 */
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vda = vd->vdev_tsd;
		goto skip_open;
	}

	vda = kmem_zalloc(sizeof (vdev_disk_aio_t), KM_SLEEP);

	/*
	 * We always open the files from the root of the global zone, even if
	 * we're in a local zone.  If the user has gotten to this point, the
	 * administrator has already decided that the pool should be available
	 * to local zone users, so the underlying devices should be as well.
	 */
	ASSERT(vd->vdev_path != NULL && vd->vdev_path[0] == '/');
	vda->vda_fd = open(vd->vdev_path,
	    ((spa_mode(vd->vdev_spa) & FWRITE) != 0) ? O_RDWR|O_DIRECT :
	    O_RDONLY|O_DIRECT);

	if (vda->vda_fd < 0) {
		kmem_free(vda, sizeof (vdev_disk_aio_t));
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(errno));
	}
	vd->vdev_tsd = vda;

skip_open:
	if (ioctl(vda->vda_fd, BLKSSZGET, ashift) != 0) {
		(void) close(vda->vda_fd);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(errno));
	}
	if (ioctl(vda->vda_fd, BLKGETSIZE64, psize) != 0) {
		(void) close(vda->vda_fd);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(errno));
	}
	if (ioctl(vda->vda_fd, BLKROTATIONAL, &isrot) != 0) {
		(void) close(vda->vda_fd);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(errno));
	}

	*ashift = highbit64(MAX(*ashift, SPA_MINBLOCKSIZE)) - 1;
	*max_psize = *psize;
	vd->vdev_nonrot = !isrot;

	return (0);
}

static void
vdev_disk_aio_close(vdev_t *vd)
{
	vdev_disk_aio_t *vda = vd->vdev_tsd;

	if (vd->vdev_reopening || vda == NULL)
		return;

	(void) close(vda->vda_fd);

	vd->vdev_delayed_close = B_FALSE;
	kmem_free(vda, sizeof (vdev_disk_aio_t));
	vd->vdev_tsd = NULL;
}

/*
 * Process a single result from asynchronous IO.
 */
static void
vdev_disk_aio_done(aio_task_t *task, unsigned long res)
{
	zio_t *zio = task->zio;

	if (zio->io_type == ZIO_TYPE_IOCTL) {
		if (res != 0) {
			zio->io_error = (SET_ERROR(-res));
		}
	} else {
		if (zio->io_type == ZIO_TYPE_READ)
			abd_return_buf_copy(zio->io_abd, task->buf,
			    zio->io_size);
		else if (zio->io_type == ZIO_TYPE_WRITE)
			abd_return_buf(zio->io_abd, task->buf, zio->io_size);
		else
			ASSERT(0);

		if (res < 0)
			zio->io_error = (SET_ERROR(-res));
		else if (res != zio->io_size)
			zio->io_error = (SET_ERROR(ENOSPC));

	}
	/*
	 * Perf optimisation: For reads there is checksum verify pipeline
	 * stage which is CPU intensive and could delay next poll considerably
	 * hence it is executed asynchronously, however for other operations
	 * (write and ioctl) it is faster to finish zio directly (synchronously)
	 * than to dispatch the work to a separate thread.
	 *
	 * TODO: Verify the assumption above by real measurement.
	 */
	if (zio->io_type == ZIO_TYPE_READ)
		zio_interrupt(zio);
	else
		zio_execute(zio);

	kmem_free(task, sizeof (aio_task_t));
}

/*
 * Poll for asynchronous IO done events from one vdev.
 */
static void
vdev_disk_aio_poll(void *arg)
{
	struct io_event *events;
	struct timespec timeout;
	int nr;

	/* allocated on heap not to exceed recommended frame size */
	events = kmem_alloc(sizeof (struct io_event) * AIO_QUEUE_DEPTH,
	    KM_SLEEP);

	while (!stop_polling) {
		timeout.tv_sec = 0;
		timeout.tv_nsec = 100000000;  // 100ms

		/*
		 * TODO: implement userspace polling to further boost
		 * performance of AIO as done in this patch:
		 * https://www.spinics.net/lists/fio/msg00869.html
		 */
		nr = io_getevents(io_ctx, 1, AIO_QUEUE_DEPTH, events,
		    &timeout);

		if (nr < 0) {
			int error = -nr;

			/* all errors except EINTR are unrecoverable */
			if (error == EINTR) {
				continue;
			} else {
				fprintf(stderr,
				    "Failed when polling for AIO events: %d\n",
				    error);
				break;
			}
		}
		ASSERT3P(nr, <=, AIO_QUEUE_DEPTH);

		for (int i = 0; i < nr; i++) {
			vdev_disk_aio_done(events[i].data, events[i].res);
		}
	}

	kmem_free(events, sizeof (struct io_event) * AIO_QUEUE_DEPTH);
	poller_tid = 0;
	thread_exit();
}

/*
 * Check and submit asynchronous IO.
 */
static void
vdev_disk_aio_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_disk_aio_t *vda = vd->vdev_tsd;
	aio_task_t *task;
	struct iocb *iocb;
	int error;

	/*
	 * Check operation type.
	 */
	switch (zio->io_type) {
	case ZIO_TYPE_IOCTL:
		if (!vdev_readable(vd)) {
			zio->io_error = (SET_ERROR(ENXIO));
			zio_interrupt(zio);
			return;
		}
		if (zio->io_cmd != DKIOCFLUSHWRITECACHE) {
			zio->io_error = (SET_ERROR(ENOTSUP));
			zio_execute(zio);
			return;
		}
		// XXX is it used?
		if (zfs_nocacheflush) {
			zio_execute(zio);
			return;
		}
		break;

	case ZIO_TYPE_WRITE:
		break;
	case ZIO_TYPE_READ:
		break;
	default:
		zio->io_error = (SET_ERROR(ENOTSUP));
		zio_interrupt(zio);
		break;
	}

	/*
	 * Prepare AIO command control block.
	 */
	task = kmem_alloc(sizeof (aio_task_t), KM_SLEEP);
	task->zio = zio;
	task->buf = NULL;
	iocb = &task->iocb;

	switch (zio->io_type) {
	case ZIO_TYPE_IOCTL:
		io_prep_fsync(iocb, vda->vda_fd);
		break;
	case ZIO_TYPE_WRITE:
		task->buf = abd_borrow_buf_copy(zio->io_abd, zio->io_size);
		io_prep_pwrite(iocb, vda->vda_fd, task->buf, zio->io_size,
		    zio->io_offset);
		break;
	case ZIO_TYPE_READ:
		task->buf = abd_borrow_buf(zio->io_abd, zio->io_size);
		io_prep_pread(iocb, vda->vda_fd, task->buf, zio->io_size,
		    zio->io_offset);
		break;
	default:
		ASSERT(0);
	}

	iocb->data = task;

	/*
	 * Submit async IO.
	 * XXX What happens if AIO_QUEUE_DEPTH is exceeded?
	 */
	error = io_submit(io_ctx, 1, &iocb);
	if (error == 0) {
		/* no error but the control block was not submitted */
		zio->io_error = (SET_ERROR(EAGAIN));
		zio_interrupt(zio);
	} else if (error < 0) {
		zio->io_error = (SET_ERROR(-error));
		zio_interrupt(zio);
	}
}

/* ARGSUSED */
static void
vdev_disk_zio_done(zio_t *zio)
{
	/*
	 * This callback is used to trigger device removal or do another
	 * smart things in case that zio ends up with EIO error.
	 * As of now nothing implemented here.
	 */
}

/*
 * Create AIO context and poller thread.
 *
 * Any failure triggers assert as recovering from error in context which this
 * function is called from, would be too difficult.
 */
void
vdev_disk_aio_init(void)
{
	int err;

	/*
	 * TODO: code in fio aio plugin suggests that for new kernels we can
	 * pass INTMAX as limit here and use max limit allowed by the kernel.
	 */
	err = io_setup(AIO_QUEUE_DEPTH, &io_ctx);
	if (err != 0) {
		fprintf(stderr, "Failed to initialize AIO context: %d\n", -err);
		ASSERT3P(0, ==, -err);
		return;
	}

	stop_polling = B_FALSE;
	poller_tid = (uintptr_t)thread_create(NULL, 0, vdev_disk_aio_poll,
	    NULL, 0, &p0, TS_RUN, 0);
}

/*
 * Waits for poller thread to exit and destroys AIO context.
 *
 * TODO: The current algorithm for poller thread exit is rough and full of
 * sleeps.
 */
void
vdev_disk_aio_fini(void)
{
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = 100000000;  // 100ms

	stop_polling = B_TRUE;
	while (poller_tid != 0) {
		nanosleep(&ts, NULL);
	}
	(void) io_destroy(io_ctx);
	io_ctx = NULL;
}

vdev_ops_t vdev_disk_ops = {
	vdev_disk_aio_open,
	vdev_disk_aio_close,
	vdev_default_asize,
	vdev_disk_aio_start,
	vdev_disk_zio_done,
	NULL,
	NULL,
	vdev_disk_aio_hold,
	vdev_disk_aio_rele,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};