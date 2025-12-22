/*
 * Text clock, angelo@kernel-space.org
 */

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SH_MEM_TC	"/shmtc.01"
#define FILE_PATH_LOCK	"/tmp/tc-lock"

struct tc_time {
	int active;
	int hrs;
	int min;
	int sec;
};

static int active;
static int shm_fd;
static void *sh_ptr;
static struct tc_time *g_tct;

#define DEBUG 1

#ifdef DEBUG

#define FILE_PATH_DBG "/tmp/log-tc"
static FILE *fdbg;

static int debug_init(int cmd)
{
	const char *fn = cmd ? FILE_PATH_DBG "-cmd" : FILE_PATH_DBG;

	fdbg = fopen(fn, "w");
	if (!fdbg) {
		fprintf(stderr, "cannot create debug file\n");
		return -1;
	}

	return 0;
}

static void dbg(char *msg, ...)
{
	va_list l;

	va_start(l, msg);
	vfprintf(fdbg, msg, l);
	va_end(l);

	fflush(fdbg);
}
#else
static void dbg(char *msg, ...) {}
#endif /* DEBUG */

static int prg_lock()
{
	FILE *fl;
	struct stat statbuf;

	if (stat(FILE_PATH_LOCK, &statbuf) == 0)
		return -1;

	fl = fopen(FILE_PATH_LOCK, "w+");
	if (!fl) {
		fprintf(stderr, "cannot create lock file\n");
		return -1;
	}
	fwrite(" ", 1, 1, fl);
	fclose(fl);

	return 0;
}

static void exit_clock()
{
	dbg("%s() called\n", __func__);

	munmap(sh_ptr, sizeof (struct tc_time)) ;
	close(shm_fd);
	shm_unlink(SH_MEM_TC);

#ifdef DEBUG
	fflush(fdbg);
	fclose(fdbg);
#endif
}

static void print_time(struct tc_time *tct)
{
	if (tct) {
		printf("%02d.%02d.%02d", tct->hrs, tct->min, tct->sec);
	} else {
		dbg("%s() cannot print, no tct\n", __func__);
	}
}

static void timer_signal_handler(int signum, siginfo_t *info, void *context)
{
	if (!g_tct || !g_tct->active)
		return;

	g_tct->sec -= 1;
	if (g_tct->sec < 0) {
		g_tct->sec = 59;
		g_tct->min--;
		if (g_tct->min < 0) {
			g_tct->hrs--;
			g_tct->min = 59;
			if (g_tct->hrs < 0) {
				g_tct->min = g_tct->sec = 0;
				g_tct->active = 0;
				return;
			}
		}
	}
}

static int setup_shared_memory(void)
{
	int ret;

	ret = shm_open(SH_MEM_TC, O_CREAT | O_RDWR, 0644);
	if (ret == -1)
		return ret;

	shm_fd = ret;

	if (ftruncate(shm_fd, sizeof(struct tc_time)) == -1) {
		fprintf(stderr, "cannot set shared object size\n");
		return -1;
	}

	sh_ptr = mmap(NULL, sizeof(struct tc_time),
		      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (sh_ptr == MAP_FAILED) {
		fprintf(stderr, "cannot set shared object size: %d\n", errno);
		return -1;
	}

	/* Setup */
	g_tct = (struct tc_time *)sh_ptr;
	memset(g_tct, 0, sizeof(g_tct));
	g_tct->hrs = 8;

	return 0;
}

static void execute_command(int cmd)
{
	int sfd;

	sfd = shm_open(SH_MEM_TC, O_RDWR, 0644);
	if (sfd < 0) {
		printf("bad-clock\n");
		return;
	}

	struct tc_time *tct = (struct tc_time *)
		mmap(NULL, sizeof(struct tc_time),
		     PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
	if (!tct || tct == MAP_FAILED)
		return;

	switch (cmd) {
	case 't':
		print_time(tct);
		break;
	case 'H':
		tct->hrs++;
		print_time(tct);
		break;
	case 'h':
		if (tct->hrs)
			tct->hrs--;
		print_time(tct);
		break;
	case 'M':
		tct->min = (tct->min == 59) ? 0 : tct->min + 1;
		print_time(tct);
		break;
	case 'm':
		tct->min = (tct->min == 0) ? 59 : tct->min - 1;
		print_time(tct);
		break;
	case 'r':
		tct->active = false;
		tct->hrs = 8; tct->min = 0; tct->sec = 0;
		print_time(tct);
		break;
	case 's':
		/* Toggle ON/OFF */
		tct->active = !tct->active;
		break;
	default:
		break;
	}
}

static void ctrl_c_handler(int sig)
{
	dbg("%s()\n", __func__);

	unlink(FILE_PATH_LOCK);

	if (sh_ptr && g_tct)
		exit_clock();

	exit(1);
}

static void segfault_handler(int sig)
{
	void *array[512];
	char **strings;
	int i, size;

	unlink(FILE_PATH_LOCK);
	shm_unlink(SH_MEM_TC);

	dbg("SEGMENTATION FAULT\n");

	size = backtrace(array, 10);
	strings = backtrace_symbols(array, size);

	for (i = 0; i < size; i++)
		dbg("%s\n", strings[i]);

	free(strings);
}

/* System sends this before kill, t can be handled at least to close files. */
static void x_signal_handler(int sig)
{
	unlink(FILE_PATH_LOCK);
	shm_unlink(SH_MEM_TC);

	dbg("EXITING FOR SIGNAL: %d\n", sig);
	exit(1);
}

int main(int argc, char **argv)
{
	timer_t sec_timer;
	struct sigaction sigact;
	struct sigevent sigevt;
	struct itimerspec alarm;

#ifdef DEBUG
	if (debug_init(argc > 1))
		exit(-1);

	dbg("textclock debug started, argc = %d\n", argc);
#endif

	if (argc == 1) {
		dbg("setting signal for ctrl_c\n");
		if (signal(SIGINT, ctrl_c_handler) == SIG_ERR) {
			dbg("cannot set ctrl_c handler, err: %d\n", errno);
			return errno;
		}
		dbg("setting signal for segfault\n");
		if (signal(SIGSEGV, segfault_handler) == SIG_ERR) {
			dbg("cannot set segv handler, err: %d\n", errno);
			return errno;
		}
		dbg("setting signals for x\n");
		if (signal(SIGQUIT, x_signal_handler) == SIG_ERR) {
			fprintf(stderr, "cannot set SIGQUIT handler, err: %d\n",
				errno);
			return errno;
		}
		if (signal(SIGTERM, x_signal_handler) == SIG_ERR) {
			fprintf(stderr, "cannot set SIGTERM handler, err: %d\n",
				errno);
			return errno;
		}

		if (prg_lock()) {
			printf("program already running, exiting.\n");
			dbg("setting signal for ctrl_c\n");
			return -1;
		}

		dbg("setting up shared memory\n");

		if (setup_shared_memory()) {
			fprintf(stderr, "cannot create shared memmory\n");
			return -1;
		}
	} else if (argc == 2) {
		execute_command(argv[1][0]);
		exit(0);
	} else {
		dbg("wrong arg number\n");
		exit(1);
	}

	dbg("creatig timer\n");

	sigemptyset(&sigact.sa_mask);
	sigact.sa_sigaction = timer_signal_handler;
	sigact.sa_flags = SA_SIGINFO | SA_RESTART;
	if (sigaction(SIGRTMIN + 0, &sigact, NULL))
		return errno;

	sigevt.sigev_notify = SIGEV_SIGNAL;
	sigevt.sigev_signo = SIGRTMIN + 0;
	sigevt.sigev_value.sival_ptr = NULL;

	if (timer_create(CLOCK_REALTIME, &sigevt, &sec_timer))
		return errno;

	alarm.it_value.tv_sec = 1;
	alarm.it_value.tv_nsec = 0L;
	alarm.it_interval.tv_sec = 1L;
	alarm.it_interval.tv_nsec = 0L;

	g_tct->active = false;

	/* starting timer */
	if (timer_settime(sec_timer, 0, &alarm, NULL))
		return errno;

	dbg("sleeping loop\n");

	for (;;) {
		sleep(1);
	}

	timer_delete(sec_timer);
	exit_clock();

	unlink(FILE_PATH_LOCK);

	return 0;
}
