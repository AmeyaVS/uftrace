/*
 * mcount() handling routines for ftrace
 *
 * Copyright (C) 2014-2015, LG Electronics, Namhyung Kim <namhyung.kim@lge.com>
 *
 * Released under the GPL v2.
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <gelf.h>

/* This should be defined before #include "utils.h" */
#define PR_FMT     "mcount"
#define PR_DOMAIN  DBG_MCOUNT

#include "libmcount/mcount.h"
#include "mcount-arch.h"
#include "utils/utils.h"
#include "utils/symbol.h"
#include "utils/filter.h"
#include "utils/compiler.h"

uint64_t mcount_threshold;  /* nsec */
struct symtabs symtabs;
int shmem_bufsize = SHMEM_BUFFER_SIZE;
bool mcount_setup_done;
bool mcount_finished;

pthread_key_t mtd_key;
TLS struct mcount_thread_data mtd;

static int pfd = -1;
static int mcount_rstack_max = MCOUNT_RSTACK_MAX;
static char *mcount_exename;

#ifndef DISABLE_MCOUNT_FILTER
static int mcount_depth = MCOUNT_DEFAULT_DEPTH;
static bool mcount_enabled = true;
static enum filter_mode mcount_filter_mode = FILTER_MODE_NONE;

static struct rb_root mcount_triggers = RB_ROOT;
#endif /* DISABLE_MCOUNT_FILTER */

uint64_t mcount_gettime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int gettid(struct mcount_thread_data *mtdp)
{
	if (!mtdp->tid)
		mtdp->tid = syscall(SYS_gettid);

	return mtdp->tid;
}

static const char *session_name(void)
{
	static char session[16 + 1];
	static uint64_t session_id;
	int fd;

	if (!session_id) {
		fd = open("/dev/urandom", O_RDONLY);
		if (fd < 0)
			pr_err("cannot open urandom file");

		if (read(fd, &session_id, sizeof(session_id)) != 8)
			pr_err("reading from urandom");

		close(fd);

		snprintf(session, sizeof(session), "%016"PRIx64, session_id);
	}
	return session;
}

void ftrace_send_message(int type, void *data, size_t len)
{
	struct ftrace_msg msg = {
		.magic = FTRACE_MSG_MAGIC,
		.type = type,
		.len = len,
	};
	struct iovec iov[2] = {
		{ .iov_base = &msg, .iov_len = sizeof(msg), },
		{ .iov_base = data, .iov_len = len, },
	};

	if (pfd < 0)
		return;

	len += sizeof(msg);
	if (writev(pfd, iov, 2) != (ssize_t)len)
		pr_err("writing shmem name to pipe");
}


#define SHMEM_SESSION_FMT  "/ftrace-%s-%d-%03d" /* session-id, tid, seq */

static struct mcount_shmem_buffer *allocate_shmem_buffer(char *buf, size_t size,
							 int tid, int idx)
{
	int fd;
	struct mcount_shmem_buffer *buffer = NULL;

	snprintf(buf, size, SHMEM_SESSION_FMT, session_name(), tid, idx);

	fd = shm_open(buf, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		pr_dbg("failed to open shmem buffer: %s\n", buf);
		goto out;
	}

	if (ftruncate(fd, shmem_bufsize) < 0) {
		pr_dbg("failed to resizing shmem buffer: %s\n", buf);
		goto out;
	}

	buffer = mmap(NULL, shmem_bufsize, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	if (buffer == MAP_FAILED) {
		pr_dbg("failed to mmap shmem buffer: %s\n", buf);
		buffer = NULL;
		goto out;
	}

	/* mark it's a new buffer */
	buffer->flag = SHMEM_FL_NEW;

	close(fd);

out:
	return buffer;
}

void prepare_shmem_buffer(struct mcount_thread_data *mtdp)
{
	char buf[128];
	int idx;
	int tid = gettid(mtdp);
	struct mcount_shmem *shmem = &mtdp->shmem;

	pr_dbg2("preparing shmem buffers\n");

	shmem->nr_buf = 2;
	shmem->buffer = xcalloc(sizeof(*shmem->buffer), 2);

	for (idx = 0; idx < shmem->nr_buf; idx++) {
		shmem->buffer[idx] = allocate_shmem_buffer(buf, sizeof(buf),
							   tid, idx);
		if (shmem->buffer[idx] == NULL)
			pr_err("mmap shmem buffer");
	}

	/* set idx 0 as current buffer */
	snprintf(buf, sizeof(buf), SHMEM_SESSION_FMT, session_name(), tid, 0);
	ftrace_send_message(FTRACE_MSG_REC_START, buf, strlen(buf));
	shmem->curr = 0;
}

#define BUFFER_FLAGS  (SHMEM_FL_WRITTEN | SHMEM_FL_NEW)

static void get_new_shmem_buffer(struct mcount_thread_data *mtdp)
{
	char buf[128];
	struct mcount_shmem *shmem = &mtdp->shmem;
	int idx = shmem->seqnum % shmem->nr_buf;
	struct mcount_shmem_buffer *curr_buf = shmem->buffer[idx];

	/*
	 * It's not a new buffer, check ftrace record already
	 * consumed it.
	 */
	if (!(curr_buf->flag & BUFFER_FLAGS)) {
		struct mcount_shmem_buffer **new_buffer;
		int nr_buf = shmem->nr_buf + 1;
		int i;

		for (i = 1; i < shmem->nr_buf; i++) {
			idx = (shmem->seqnum + i) % shmem->nr_buf;

			if (shmem->buffer[idx]->flag & BUFFER_FLAGS) {
				shmem->seqnum += i;

				curr_buf = shmem->buffer[idx];
				goto reuse;
			}
		}

		new_buffer = realloc(shmem->buffer, sizeof(*new_buffer) * nr_buf);
		if (new_buffer == NULL) {
out_err:
			shmem->losts++;
			shmem->curr = -1;
			return;
		}

		/*
		 * it already free'd the old buffer, keep the new buffer
		 * regardless of allocation failure.
		 */
		shmem->buffer = new_buffer;

		idx = nr_buf - 1;
		curr_buf = allocate_shmem_buffer(buf, sizeof(buf),
						 gettid(mtdp), idx);
		if (curr_buf == NULL)
			goto out_err;

		shmem->buffer[idx] = curr_buf;

		shmem->seqnum = ROUND_UP(shmem->seqnum, nr_buf);
		shmem->seqnum += idx;

		shmem->nr_buf = nr_buf;
	}

reuse:
	snprintf(buf, sizeof(buf), SHMEM_SESSION_FMT,
		 session_name(), gettid(mtdp), idx);

	/*
	 * Start a new buffer and clear the flags.
	 * See record_mmap_file().
	 */
	__sync_fetch_and_and(&curr_buf->flag, ~BUFFER_FLAGS);

	shmem->curr = idx;
	curr_buf->size = 0;

	pr_dbg2("new buffer: [%d] %s\n", idx, buf);
	ftrace_send_message(FTRACE_MSG_REC_START, buf, strlen(buf));

	if (shmem->losts) {
		struct ftrace_ret_stack *frstack = (void *)curr_buf->data;

		frstack->time   = 0;
		frstack->type   = FTRACE_LOST;
		frstack->unused = FTRACE_UNUSED;
		frstack->more   = 0;
		frstack->addr   = shmem->losts;

		ftrace_send_message(FTRACE_MSG_LOST, &shmem->losts,
				    sizeof(shmem->losts));

		curr_buf->size = sizeof(*frstack);
		shmem->losts = 0;
	}
}

static void finish_shmem_buffer(struct mcount_thread_data *mtdp)
{
	char buf[64];
	struct mcount_shmem *shmem = &mtdp->shmem;
	int idx = shmem->seqnum % shmem->nr_buf;

	if (shmem->curr == -1)
		return;

	snprintf(buf, sizeof(buf), SHMEM_SESSION_FMT,
		 session_name(), gettid(mtdp), idx);

	pr_dbg2("done buffer: [%d] %s\n", idx, buf);
	ftrace_send_message(FTRACE_MSG_REC_END, buf, strlen(buf));

	shmem->curr = -1;
	shmem->seqnum++;
}

static void clear_shmem_buffer(struct mcount_thread_data *mtdp)
{
	struct mcount_shmem *shmem = &mtdp->shmem;

	pr_dbg2("releasing all shmem buffers\n");

	munmap(shmem->buffer[0], shmem_bufsize);
	munmap(shmem->buffer[1], shmem_bufsize);

	free(shmem->buffer);
	shmem->buffer = NULL;
	shmem->seqnum = 0;
}

static void shmem_finish(struct mcount_thread_data *mtdp)
{
	struct mcount_shmem *shmem = &mtdp->shmem;
	unsigned seq = shmem->seqnum;

	finish_shmem_buffer(mtdp);
	/* force update seqnum to call finish on both buffer */
	if (seq == shmem->seqnum)
		shmem->seqnum++;
	shmem->curr = shmem->seqnum % shmem->nr_buf;
	finish_shmem_buffer(mtdp);

	clear_shmem_buffer(mtdp);
}

static int record_ret_stack(struct mcount_thread_data *mtdp,
			    enum ftrace_ret_stack_type type,
			    struct mcount_ret_stack *mrstack, bool more)
{
	struct ftrace_ret_stack *frstack;
	uint64_t timestamp = mrstack->start_time;
	struct mcount_shmem *shmem = &mtdp->shmem;
	const size_t maxsize = (size_t)shmem_bufsize - sizeof(**shmem->buffer);
	struct mcount_shmem_buffer *curr_buf = shmem->buffer[shmem->curr];

	if (unlikely(shmem->curr == -1 ||
		     curr_buf->size + sizeof(*frstack) > maxsize)) {
		finish_shmem_buffer(mtdp);
		get_new_shmem_buffer(mtdp);

		if (shmem->curr == -1) {
			shmem->losts++;
			return -1;
		}

		curr_buf = shmem->buffer[shmem->curr];
	}

	if (type == FTRACE_EXIT)
		timestamp = mrstack->end_time;

	frstack = (void *)(curr_buf->data + curr_buf->size);

	frstack->time   = timestamp;
	frstack->type   = type;
	frstack->unused = FTRACE_UNUSED;
	frstack->more   = more;
	frstack->depth  = mrstack->depth;
	frstack->addr   = mrstack->child_ip;

	curr_buf->size += sizeof(*frstack);
	mrstack->flags |= MCOUNT_FL_WRITTEN;

	pr_dbg3("rstack[%d] %s %lx\n", mrstack->depth,
	       type == FTRACE_ENTRY? "ENTRY" : "EXIT ", mrstack->child_ip);
	return 0;
}

static void record_argument(struct mcount_thread_data *mtdp,
			    struct list_head *args_spec,
			    struct mcount_regs *regs)
{
	struct mcount_shmem *shmem = &mtdp->shmem;
	const size_t maxsize = (size_t)shmem_bufsize - sizeof(**shmem->buffer);
	struct mcount_shmem_buffer *curr_buf = shmem->buffer[shmem->curr];
	struct ftrace_arg_spec *spec;
	size_t size = 0;
	void *ptr;
	char buf[64];
	int nr_strarg = 0;

	list_for_each_entry(spec, args_spec, list) {
		if (spec->fmt == ARG_FMT_STR) {
			ptr = (void *)mcount_get_arg(regs, spec);
			if (ptr) {
				size_t len;

				strncpy(buf, ptr, sizeof(buf));
				buf[sizeof(buf) - 1] = '\0';
				len = strlen(buf);

				/* store 2-byte length before string */
				size += ALIGN(len + 2, 4);
			}
			else {
				buf[0] = '\0';
				size += 8;
			}
			nr_strarg++;
		}
		else
			size += ALIGN(spec->size, 4);
	}

	assert(size < maxsize);

	if (unlikely(curr_buf->size + size > maxsize)) {
		finish_shmem_buffer(mtdp);
		get_new_shmem_buffer(mtdp);

		if (shmem->curr == -1)
			return;

		curr_buf = shmem->buffer[shmem->curr];
	}

	ptr = (void *)(curr_buf->data + curr_buf->size);
	list_for_each_entry(spec, args_spec, list) {
		long val;

		val = mcount_get_arg(regs, spec);
		if (spec->fmt == ARG_FMT_STR) {
			unsigned short len;

			if (val) {
				if (nr_strarg > 1) {
					strncpy(buf, (void *)val, sizeof(buf));
					buf[sizeof(buf) - 1] = '\0';
				}
				len = strlen(buf);

				memcpy(ptr, &len, sizeof(len));
				strcpy(ptr + 2, buf);
				ptr += ALIGN(len + 2, 4);
			}
			else {
				len = 4;
				memcpy(ptr, &len, sizeof(len));
				memset(ptr + 2, 0xff, 4);
				ptr += 8;
			}
		}
		else {
			memcpy(ptr, &val, spec->size);
			ptr += ALIGN(spec->size, 4);
		}
	}

	curr_buf->size += ALIGN(size, 8);
	pr_dbg3("%s %zd bytes\n", "ARGUMENT", size);
}

int record_trace_data(struct mcount_thread_data *mtdp,
		      struct mcount_ret_stack *mrstack,
		      struct list_head *args_spec,
		      struct mcount_regs *regs)
{
	struct mcount_ret_stack *non_written_mrstack = NULL;
	struct ftrace_ret_stack *frstack;
	size_t size;
	int count = 0;

#define SKIP_FLAGS  (MCOUNT_FL_NORECORD | MCOUNT_FL_DISABLED)

	if (mrstack < mtdp->rstack)
		return 0;

	if (!(mrstack->flags & MCOUNT_FL_WRITTEN)) {
		non_written_mrstack = mrstack;

		if (!(non_written_mrstack->flags & SKIP_FLAGS))
			count++;

		while (non_written_mrstack > mtdp->rstack) {
			struct mcount_ret_stack *prev = non_written_mrstack - 1;

			if (prev->flags & MCOUNT_FL_WRITTEN)
				break;

			if (!(prev->flags & SKIP_FLAGS))
				count++;

			non_written_mrstack = prev;
		}
	}

	if (mrstack->end_time)
		count++;  /* for exit */

	size = count * sizeof(*frstack);

	pr_dbg3("task %d recorded %zd bytes (record count = %d)\n",
		gettid(mtdp), size, count);

	while (non_written_mrstack && non_written_mrstack < mrstack) {
		if (!(non_written_mrstack->flags & SKIP_FLAGS)) {
			if (record_ret_stack(mtdp, FTRACE_ENTRY,
					     non_written_mrstack, false)) {
				mtdp->shmem.losts += count - 1;
				return 0;
			}

			size -= sizeof(*frstack);
			count--;
		}
		non_written_mrstack++;
	}

	if (!(mrstack->flags & (MCOUNT_FL_WRITTEN | SKIP_FLAGS))) {
		bool more = false;

		if (regs && args_spec && !list_empty(args_spec))
			more = true;

		if (record_ret_stack(mtdp, FTRACE_ENTRY, non_written_mrstack, more))
			return 0;

		size -= sizeof(*frstack);

		if (more)
			record_argument(mtdp, args_spec, regs);
	}

	if (mrstack->end_time) {
		if (record_ret_stack(mtdp, FTRACE_EXIT, mrstack, false))
			return 0;

		size -= sizeof(*frstack);
	}

	assert(size == 0);
	return 0;
}

static void record_proc_maps(char *dirname, const char *sess_id)
{
	int ifd, ofd, len;
	char buf[4096];

	ifd = open("/proc/self/maps", O_RDONLY);
	if (ifd < 0)
		pr_err("cannot open proc maps file");

	snprintf(buf, sizeof(buf), "%s/sid-%s.map", dirname, sess_id);

	ofd = open(buf, O_WRONLY | O_CREAT, 0644);
	if (ofd < 0)
		pr_err("cannot open for writing maps file");

	while ((len = read(ifd, buf, sizeof(buf))) > 0) {
		if (write(ofd, buf, len) != len)
			pr_err("write proc maps failed");
	}

	close(ifd);
	close(ofd);
}

static void send_session_msg(struct mcount_thread_data *mtdp, const char *sess_id)
{
	struct ftrace_msg_sess sess = {
		.task = {
			.time = mcount_gettime(),
			.pid = getpid(),
			.tid = gettid(mtdp),
		},
		.namelen = strlen(mcount_exename),
	};
	struct ftrace_msg msg = {
		.magic = FTRACE_MSG_MAGIC,
		.type = FTRACE_MSG_SESSION,
		.len = sizeof(sess) + sess.namelen,
	};
	struct iovec iov[3] = {
		{ .iov_base = &msg, .iov_len = sizeof(msg), },
		{ .iov_base = &sess, .iov_len = sizeof(sess), },
		{ .iov_base = mcount_exename, .iov_len = sess.namelen, },
	};
	int len = sizeof(msg) + msg.len;

	if (pfd < 0)
		return;

	memcpy(sess.sid, sess_id, sizeof(sess.sid));

	if (writev(pfd, iov, 3) != len)
		pr_err("write tid info failed");
}

/* to be used by pthread_create_key() */
static void mtd_dtor(void *arg)
{
	struct mcount_thread_data *mtdp = arg;

	free(mtdp->rstack);
}

static void mcount_init_file(void)
{
	char *dirname = getenv("FTRACE_DIR");

	/* This is for the case of library-only tracing */
	if (!mcount_setup_done)
		__monstartup(0, ~0);

	if (pthread_key_create(&mtd_key, mtd_dtor))
		pr_err("cannot create shmem key");

	if (dirname == NULL)
		dirname = FTRACE_DIR_NAME;

	send_session_msg(&mtd, session_name());
	record_proc_maps(dirname, session_name());
}

void mcount_prepare(void)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;
	struct ftrace_msg_task tmsg = {
		.pid = getpid(),
		.tid = gettid(&mtd),
	};

#ifndef DISABLE_MCOUNT_FILTER
	mtd.filter.depth  = mcount_depth;
	mtd.enable_cached = mcount_enabled;
#endif
	mtd.rstack = xmalloc(mcount_rstack_max * sizeof(*mtd.rstack));

	pthread_once(&once_control, mcount_init_file);
	prepare_shmem_buffer(&mtd);

	pthread_setspecific(mtd_key, &mtd);

	/* time should be get after session message sent */
	tmsg.time = mcount_gettime();

	ftrace_send_message(FTRACE_MSG_TID, &tmsg, sizeof(tmsg));
}

#ifndef DISABLE_MCOUNT_FILTER
/* update filter state from trigger result */
enum filter_result mcount_entry_filter_check(struct mcount_thread_data *mtdp,
					     unsigned long child,
					     struct ftrace_trigger *tr)
{
	pr_dbg3("<%d> enter %lx\n", mtdp->idx, child);

	if (mtdp->idx >= mcount_rstack_max)
		pr_err_ns("too deeply nested calls: %d\n", mtdp->idx);

	/* save original depth to restore at exit time */
	mtdp->filter.saved_depth = mtdp->filter.depth;

	/* already filtered by notrace option */
	if (mtdp->filter.out_count > 0)
		return FILTER_OUT;

	ftrace_match_filter(&mcount_triggers, child, tr);

	pr_dbg3(" tr->flags: %lx, filter mode, count: [%d] %d/%d\n",
		tr->flags, mcount_filter_mode, mtdp->filter.in_count,
		mtdp->filter.out_count);

	if (tr->flags & TRIGGER_FL_FILTER) {
		if (tr->fmode == FILTER_MODE_IN)
			mtdp->filter.in_count++;
		else if (tr->fmode == FILTER_MODE_OUT)
			mtdp->filter.out_count++;

		/* apply default filter depth when match */
		mtdp->filter.depth = mcount_depth;
	}
	else {
		/* not matched by filter */
		if (mcount_filter_mode == FILTER_MODE_IN &&
		    mtdp->filter.in_count == 0)
			return FILTER_OUT;
	}

	if (tr->flags & TRIGGER_FL_DEPTH)
		mtdp->filter.depth = tr->depth;

	if (tr->flags & TRIGGER_FL_TRACE_ON)
		mcount_enabled = true;

	if (tr->flags & TRIGGER_FL_TRACE_OFF)
		mcount_enabled = false;

	if (!mcount_enabled)
		return FILTER_IN;

	/*
	 * it can be < 0 in case it is called from plthook_entry()
	 * which in turn is called libcygprof.so.
	 */
	if (mtdp->filter.depth <= 0)
		return FILTER_OUT;

	mtdp->filter.depth--;
	return FILTER_IN;
}

/* save current filter state to rstack */
void mcount_entry_filter_record(struct mcount_thread_data *mtdp,
				struct mcount_ret_stack *rstack,
				struct ftrace_trigger *tr,
				struct mcount_regs *regs)
{
	if (tr->flags & TRIGGER_FL_FILTER) {
		if (tr->fmode == FILTER_MODE_IN)
			rstack->flags |= MCOUNT_FL_FILTERED;
		else
			rstack->flags |= MCOUNT_FL_NOTRACE;
	}

	if (mtdp->filter.out_count > 0 ||
	    (mtdp->filter.in_count == 0 && mcount_filter_mode == FILTER_MODE_IN))
		rstack->flags |= MCOUNT_FL_NORECORD;

	rstack->filter_depth = mtdp->filter.saved_depth;

	if (!(rstack->flags & MCOUNT_FL_NORECORD)) {
		mtdp->record_idx++;

		if (!mcount_enabled) {
			rstack->flags |= MCOUNT_FL_DISABLED;
		}
		else if (tr->flags & TRIGGER_FL_ARGUMENT) {
			record_trace_data(mtdp, rstack, tr->pargs, regs);
		}
		else if (tr->flags & TRIGGER_FL_RECOVER) {
			record_trace_data(mtdp, rstack, tr->pargs, regs);
			mcount_restore();
			*rstack->parent_loc = (unsigned long) mcount_return;
			rstack->flags |= MCOUNT_FL_RECOVER;
		}

		if (mtdp->enable_cached != mcount_enabled) {
			/*
			 * Flush existing rstack when mcount_enabled is off
			 * (i.e. disabled).  Note that changing to enabled is
			 * already handled in record_trace_data() on exit path
			 * using the MCOUNT_FL_DISALBED flag.
			 */
			if (!mcount_enabled)
				record_trace_data(mtdp, rstack, NULL, regs);

			mtdp->enable_cached = mcount_enabled;
		}
	}
}

/* restore filter state from rstack */
void mcount_exit_filter_record(struct mcount_thread_data *mtdp,
			       struct mcount_ret_stack *rstack)
{
	pr_dbg3("<%d> exit  %lx\n", mtdp->idx, rstack->child_ip);

	if (rstack->flags & MCOUNT_FL_FILTERED)
		mtdp->filter.in_count--;
	else if (rstack->flags & MCOUNT_FL_NOTRACE)
		mtdp->filter.out_count--;

	if (rstack->flags & MCOUNT_FL_RECOVER)
		mcount_reset();

	mtdp->filter.depth = rstack->filter_depth;

	if (!(rstack->flags & MCOUNT_FL_NORECORD)) {
		if (mtdp->record_idx > 0)
			mtdp->record_idx--;

		if (rstack->end_time - rstack->start_time > mcount_threshold ||
		    rstack->flags & MCOUNT_FL_WRITTEN) {
			if (mcount_enabled &&
			    record_trace_data(mtdp, rstack, NULL, NULL) < 0)
				pr_err("error during record");
		}
	}
}

#else /* DISABLE_MCOUNT_FILTER */
enum filter_result mcount_entry_filter_check(struct mcount_thread_data *mtdp,
					     unsigned long child,
					     struct ftrace_trigger *tr)
{
	if (mtdp->idx >= mcount_rstack_max)
		pr_err_ns("too deeply nested calls: %d\n", mtdp->idx);

	return FILTER_IN;
}

void mcount_entry_filter_record(struct mcount_thread_data *mtdp,
				struct mcount_ret_stack *rstack,
				struct ftrace_trigger *tr,
				struct mcount_regs *regs)
{
	mtdp->record_idx++;
}

void mcount_exit_filter_record(struct mcount_thread_data *mtdp,
			       struct mcount_ret_stack *rstack)
{
	mtdp->record_idx--;

	if (rstack->end_time - rstack->start_time > mcount_threshold ||
	    rstack->flags & MCOUNT_FL_WRITTEN) {
		if (record_trace_data(mtdp, rstack, NULL, NULL) < 0)
			pr_err("error during record");
	}
}

#endif /* DISABLE_MCOUNT_FILTER */

__weak unsigned long *mcount_arch_parent_location(struct symtabs *symtabs,
						  unsigned long *parent_loc,
						  unsigned long child_ip)
{
	return parent_loc;
}

int mcount_entry(unsigned long *parent_loc, unsigned long child,
		 struct mcount_regs *regs)
{
	enum filter_result filtered;
	struct mcount_thread_data *mtdp;
	struct mcount_ret_stack *rstack;
	struct ftrace_trigger tr = {
		.flags = 0,
	};

	/*
	 * If an executable has its own malloc(), following recursion could occur
	 *
	 * mcount_entry -> mcount_prepare -> xmalloc -> mcount_entry -> ...
	 */
	if (unlikely(mcount_should_stop()))
		return -1;

	mtd.recursion_guard = true;

	/* Access the mtd through TSD pointer to reduce TLS overhead */
	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp))) {
		mcount_prepare();

		mtdp = get_thread_data();
		assert(mtdp);
	}

	filtered = mcount_entry_filter_check(mtdp, child, &tr);
	if (filtered == FILTER_OUT) {
		mtdp->recursion_guard = false;
		return -1;
	}

	/* fixup the parent_loc in an arch-dependant way (if needed) */
	parent_loc = mcount_arch_parent_location(&symtabs, parent_loc, child);

	rstack = &mtdp->rstack[mtdp->idx++];

	rstack->depth      = mtdp->record_idx;
	rstack->dyn_idx    = MCOUNT_INVALID_DYNIDX;
	rstack->parent_loc = parent_loc;
	rstack->parent_ip  = *parent_loc;
	rstack->child_ip   = child;
	rstack->start_time = mcount_gettime();
	rstack->end_time   = 0;
	rstack->flags      = 0;

	/* hijack the return address */
	*parent_loc = (unsigned long)mcount_return;

	mcount_entry_filter_record(mtdp, rstack, &tr, regs);
	mtdp->recursion_guard = false;
	return 0;
}

unsigned long mcount_exit(void)
{
	struct mcount_thread_data *mtdp;
	struct mcount_ret_stack *rstack;
	unsigned long retaddr;

	mtdp = get_thread_data();
	assert(mtdp);

	mtdp->recursion_guard = true;

	rstack = &mtdp->rstack[mtdp->idx - 1];

	rstack->end_time = mcount_gettime();
	mcount_exit_filter_record(mtdp, rstack);

	retaddr = rstack->parent_ip;

	compiler_barrier();

	mtdp->idx--;
	mtdp->recursion_guard = false;

	return retaddr;
}

static void mcount_finish(void)
{
	mcount_finished = true;

	shmem_finish(&mtd);
	mtd_dtor(&mtd);
	pthread_key_delete(mtd_key);

	if (pfd != -1) {
		close(pfd);
		pfd = -1;
	}
}

static int cygprof_entry(unsigned long parent, unsigned long child)
{
	enum filter_result filtered;
	struct mcount_thread_data *mtdp;
	struct mcount_ret_stack *rstack;
	struct ftrace_trigger tr = {
		.flags = 0,
	};

	if (unlikely(mcount_should_stop()))
		return -1;

	mtd.recursion_guard = true;

	/* Access the mtd through TSD pointer to reduce TLS overhead */
	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp))) {
		mcount_prepare();

		mtdp = get_thread_data();
		assert(mtdp);
	}

	filtered = mcount_entry_filter_check(mtdp, child, &tr);

	rstack = &mtdp->rstack[mtdp->idx++];

	rstack->depth      = mtdp->record_idx;
	rstack->dyn_idx    = MCOUNT_INVALID_DYNIDX;
	rstack->parent_ip  = parent;
	rstack->child_ip   = child;
	rstack->end_time   = 0;

	if (filtered == FILTER_IN) {
		rstack->start_time = mcount_gettime();
		rstack->flags      = 0;
	}
	else {
		rstack->start_time = 0;
		rstack->flags      = MCOUNT_FL_NORECORD;
	}

	mcount_entry_filter_record(mtdp, rstack, &tr, NULL);
	mtdp->recursion_guard = false;
	return 0;
}

static void cygprof_exit(unsigned long parent, unsigned long child)
{
	struct mcount_thread_data *mtdp;
	struct mcount_ret_stack *rstack;

	if (unlikely(mcount_should_stop()))
		return;

	mtd.recursion_guard = true;

	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp))) {
		mcount_prepare();

		mtdp = get_thread_data();
		assert(mtdp);
	}

	rstack = &mtdp->rstack[mtdp->idx - 1];

	if (!(rstack->flags & MCOUNT_FL_NORECORD))
		rstack->end_time = mcount_gettime();

	mcount_exit_filter_record(mtdp, rstack);

	compiler_barrier();

	mtdp->idx--;
	mtdp->recursion_guard = false;
}

static void atfork_prepare_handler(void)
{
	struct ftrace_msg_task tmsg = {
		.time = mcount_gettime(),
		.pid = getpid(),
	};

	ftrace_send_message(FTRACE_MSG_FORK_START, &tmsg, sizeof(tmsg));
}

static void atfork_child_handler(void)
{
	struct mcount_thread_data *mtdp;
	struct ftrace_msg_task tmsg = {
		.time = mcount_gettime(),
		.pid = getppid(),
		.tid = getpid(),
	};

	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp))) {
		mcount_prepare();

		mtdp = get_thread_data();
		assert(mtdp);
	}

	mtd.tid = 0;

	clear_shmem_buffer(&mtd);
	prepare_shmem_buffer(&mtd);

	ftrace_send_message(FTRACE_MSG_FORK_END, &tmsg, sizeof(tmsg));
}

static void build_debug_domain(char *dbg_domain_str)
{
	int i, len;

	if (dbg_domain_str == NULL)
		return;

	len = strlen(dbg_domain_str);
	for (i = 0; i < len; i += 2) {
		const char *pos;
		char domain = dbg_domain_str[i];
		int level = dbg_domain_str[i+1] - '0';
		int d;

		pos = strchr(DBG_DOMAIN_STR, domain);
		if (pos == NULL)
			continue;

		d = pos - DBG_DOMAIN_STR;
		dbg_domain[d] = level;
	}
}

/*
 * external interfaces
 */
void __visible_default __monstartup(unsigned long low, unsigned long high)
{
	char *pipefd_str;
	char *logfd_str;
	char *debug_str;
	char *bufsize_str;
	char *maxstack_str;
	char *threshold_str;
	char *color_str;
	struct stat statbuf;

	if (mcount_setup_done || mtd.recursion_guard)
		return;

	mtd.recursion_guard = true;

	outfp = stdout;
	logfp = stderr;

	pipefd_str = getenv("FTRACE_PIPE");
	logfd_str = getenv("FTRACE_LOGFD");
	debug_str = getenv("FTRACE_DEBUG");
	bufsize_str = getenv("FTRACE_BUFFER");
	maxstack_str = getenv("FTRACE_MAX_STACK");
	color_str = getenv("FTRACE_COLOR");
	threshold_str = getenv("FTRACE_THRESHOLD");

	if (logfd_str) {
		int fd = strtol(logfd_str, NULL, 0);

		/* minimal sanity check */
		if (!fstat(fd, &statbuf)) {
			logfp = fdopen(fd, "a");
			setvbuf(logfp, NULL, _IOLBF, 1024);
		}
	}

	if (debug_str) {
		debug = strtol(debug_str, NULL, 0);
		build_debug_domain(getenv("FTRACE_DEBUG_DOMAIN"));
	}

	pr_dbg("initializing mcount library\n");

	if (color_str)
		setup_color(strtol(color_str, NULL, 0));

	if (pipefd_str) {
		pfd = strtol(pipefd_str, NULL, 0);

		/* minimal sanity check */
		if (fstat(pfd, &statbuf) < 0 || !S_ISFIFO(statbuf.st_mode)) {
			pr_dbg("ignore invalid pipe fd: %d\n", pfd);
			pfd = -1;
		}
	}

	if (bufsize_str)
		shmem_bufsize = strtol(bufsize_str, NULL, 0);

	mcount_exename = read_exename();
	load_symtabs(&symtabs, NULL, mcount_exename);

#ifndef DISABLE_MCOUNT_FILTER
	ftrace_setup_filter(getenv("FTRACE_FILTER"), &symtabs, NULL,
			    &mcount_triggers, &mcount_filter_mode);

	ftrace_setup_trigger(getenv("FTRACE_TRIGGER"), &symtabs, NULL,
			     &mcount_triggers);

	ftrace_setup_argument(getenv("FTRACE_ARGUMENT"), &symtabs, NULL,
			      &mcount_triggers);

	if (getenv("FTRACE_DEPTH"))
		mcount_depth = strtol(getenv("FTRACE_DEPTH"), NULL, 0);

	if (getenv("FTRACE_DISABLED"))
		mcount_enabled = false;
#endif /* DISABLE_MCOUNT_FILTER */

	if (maxstack_str)
		mcount_rstack_max = strtol(maxstack_str, NULL, 0);

	if (threshold_str)
		mcount_threshold = strtoull(threshold_str, NULL, 0);

	if (getenv("FTRACE_PLTHOOK")) {
		setup_dynsym_indexes(&symtabs);

#ifndef DISABLE_MCOUNT_FILTER
		ftrace_setup_filter(getenv("FTRACE_FILTER"), &symtabs, "PLT",
				    &mcount_triggers, &mcount_filter_mode);

		ftrace_setup_trigger(getenv("FTRACE_TRIGGER"), &symtabs, "PLT",
				    &mcount_triggers);

		ftrace_setup_argument(getenv("FTRACE_ARGUMENT"), &symtabs, "PLT",
				      &mcount_triggers);
#endif /* DISABLE_MCOUNT_FILTER */

		if (hook_pltgot(mcount_exename) < 0)
			pr_dbg("error when hooking plt: skipping...\n");
		else
			plthook_setup(&symtabs);
	}

	pthread_atfork(atfork_prepare_handler, NULL, atfork_child_handler);

	compiler_barrier();

	mcount_setup_done = true;
	mtd.recursion_guard = false;
}

void __visible_default mcleanup(void)
{
	mcount_finish();
	destroy_dynsym_indexes();

#ifndef DISABLE_MCOUNT_FILTER
	ftrace_cleanup_filter(&mcount_triggers);
#endif
}

void __visible_default mcount_restore(void)
{
	int idx;
	struct mcount_thread_data *mtdp;

	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp)))
		return;

	for (idx = mtdp->idx - 1; idx >= 0; idx--)
		*mtdp->rstack[idx].parent_loc = mtdp->rstack[idx].parent_ip;
}

void __visible_default mcount_reset(void)
{
	int idx;
	struct mcount_thread_data *mtdp;

	mtdp = get_thread_data();
	if (unlikely(check_thread_data(mtdp)))
		return;

	for (idx = mtdp->idx - 1; idx >= 0; idx--)
		*mtdp->rstack[idx].parent_loc = (unsigned long)mcount_return;
}

void __visible_default __cyg_profile_func_enter(void *child, void *parent)
{
	cygprof_entry((unsigned long)parent, (unsigned long)child);
}

void __visible_default __cyg_profile_func_exit(void *child, void *parent)
{
	cygprof_exit((unsigned long)parent, (unsigned long)child);
}

/*
 * Initializer and Finalizer
 */
static void __attribute__((constructor))
mcount_init(void)
{
	if (!mcount_setup_done)
		__monstartup(0UL, ~0UL);
}

static void __attribute__((destructor))
mcount_fini(void)
{
	_mcleanup();
}
