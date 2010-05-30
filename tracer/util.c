#define _LARGEFILE64_SOURCE 1
#define _GNU_SOURCE 1

#include <sys/ptrace.h>

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/ptrace.h>
#include <linux/unistd.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "trace.h"
#include "util.h"
#include "errors.h"

static inline long min(long a, long b)
{
	return a < b ? a : b;
}

/* platform independent */

unsigned long get_eventmsg(trace_t *t)
{
	unsigned long msg;
	if (ptrace(PTRACE_GETEVENTMSG, t->pid, -1, &msg) != 0)
		 fatal_error("PTRACE_GETEVENTMSG failed: %s pid %d",
		             strerror(errno), t->pid);

	return msg;
}

void get_registers(trace_t *t)
{
	if ( ptrace(PTRACE_GETREGS, t->pid, 0, &t->regs) < 0 )
		fatal_error("error: getting registers failed for process %d", t->pid);
}

void set_registers(trace_t *t)
{
	if ( ptrace(PTRACE_SETREGS, t->pid, 0, &t->regs) < 0 )
		fatal_error("error: setting registers failed for process %d", t->pid);
}

void get_siginfo(pid_t pid, siginfo_t *info)
{
	if ( ptrace(PTRACE_GETSIGINFO, pid, 0, info) < 0 )
		fatal_error("error: getting signal info failed for process %d", pid);
}

void set_siginfo(pid_t pid, siginfo_t *info)
{
	if ( ptrace(PTRACE_SETSIGINFO, pid, 0, info) < 0 )
		fatal_error("error: setting signal info failed for process %d", pid);
}

#if defined(__i386__) || defined(__x86_64__)

uint64_t get_timestamp(void)
{
	uint32_t lo, hi;
	/* We cannot use "=A", since this would use %rax on x86_64 */
	__asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
	return (uint64_t)hi << 32 | lo;
}

#endif

#ifdef __i386__

#define SYSCALL_OPCODE_SIZE 2
#define TRAP_FLAG 0x00000100
#define RDTSC_OPCODE_SIZE 2
#define RDTSC_OPCODE "\x0f\x31"

unsigned long get_syscall(trace_t *t)
{
	return t->regs.orig_eax;
}

void set_syscall(trace_t *t, unsigned long val)
{
	t->regs.orig_eax = val;
}

unsigned long get_result(trace_t *t)
{
	return t->regs.eax;
}

void set_result(trace_t *t, unsigned long val)
{
	t->regs.eax = val;
}

unsigned long get_arg(trace_t *t, int number)
{
	switch (number)
	{
		case 0:
			return t->regs.ebx;
		case 1:
			return t->regs.ecx;
		case 2:
			return t->regs.edx;
		case 3:
			return t->regs.esi;
		case 4:
			return t->regs.edi;
		case 5:
			return t->regs.ebp;
		default:
			fatal_error("wrong argument number");
			return -1; /* not used */
	}
}

void set_arg(trace_t *t, int number, unsigned long val)
{
	switch (number)
	{
		case 0:
			t->regs.ebx = val; break;
		case 1:
			t->regs.ecx = val; break;
		case 2:
			t->regs.edx = val; break;
		case 3:
			t->regs.esi = val; break;
		case 4:
			t->regs.edi = val; break;
		case 5:
			t->regs.ebp = val; break;
		default:
			fatal_error("wrong argument number");
	}
}

int get_trap_flag(trace_t *t)
{
	return ( t->regs.eflags & TRAP_FLAG ) ? 1 : 0;
}

void set_trap_flag(trace_t *t, int val)
{
	if (val)
		t->regs.eflags |= TRAP_FLAG;
	else
		t->regs.eflags &=~ TRAP_FLAG;
}

void reset_syscall(trace_t *t)
{
	t->regs.eip -= SYSCALL_OPCODE_SIZE;
	t->regs.eax = t->regs.orig_eax;
}

int program_counter_at_tsc(trace_t *t)
{
	char opcode[2];
	memload(t->pid, opcode, (void *)t->regs.eip, 2);
	return (memcmp(opcode, RDTSC_OPCODE, 2) == 0) ? 1:0;
}

void emulate_tsc(trace_t *t, uint64_t timestamp)
{
	t->regs.eax = (uint32_t)timestamp;
	t->regs.edx = (uint32_t)(timestamp>>32);
	t->regs.eip += RDTSC_OPCODE_SIZE;
}

#endif

void get_args(trace_t *t, long *args, int argc)
{
	int i;
	for (i=0; i<argc; i++)
		args[i] = get_arg(t, i);
}

void set_args(trace_t *t, long *args, int argc)
{
	int i;
	for (i=0; i<argc; i++)
		set_arg(t, i, args[i]);
}

void skip_syscall(trace_t *t)
{
	set_syscall(t, __NR_getpid);
	set_registers(t);
}

void undo_syscall(trace_t *t)
{
	long call = t->regs.orig_eax;

	skip_syscall(t);

	if ( (ptrace(PTRACE_SYSCALL, t->pid, 0, 0) != 0) ||
	     ( t->pid != waitpid(t->pid, NULL, __WALL) ) )
		fatal_error("cycle failed");

	set_syscall(t, call);
	reset_syscall(t);
	t->state = POST_CALL;
	set_registers(t);
}

static void wait_and_queue_signals(trace_t *t, signal_queue_t *q)
{
	int status;
	long signo;
	siginfo_t info;

	for(;;)
	{
		if ( t->pid != waitpid(t->pid, &status, __WALL) )
			fatal_error("waitpid: %s", strerror(errno));

		signo = (status>>8) & 0xff;

		if ( (signo == SIGTRAP) || (signo == CALL_SIGTRAP) )
			return;

		if (q)
		{
			get_siginfo(t->pid, &info);
			enqueue_signal(q, signo, &info);
		}

		if ( ptrace(PTRACE_SYSCALL, t->pid, 0, 0) != 0 )
			fatal_error("ptrace: %s", strerror(errno));
	}
}

void redo_syscall(trace_t *t, signal_queue_t *q)
{
	reset_syscall(t);
	set_registers(t);

	if ( ptrace(PTRACE_SYSCALL, t->pid, 0, 0) != 0 )
		fatal_error("cycle failed");

	wait_and_queue_signals(t, q);

	t->state = PRE_CALL;
}

void next_trap(trace_t *t, signal_queue_t *q)
{
	set_registers(t);

	if ( ptrace(PTRACE_SYSCALL, t->pid, 0, 0) != 0 )
		fatal_error("ptrace: %s", strerror(errno));

	wait_and_queue_signals(t, q);

	get_registers(t);
}

static registers_t inject_syscall_init(trace_t *t,
                                       long call, long args[], int argc,
                                       signal_queue_t *q)
{
	registers_t orig_regs = t->regs;

	if ( (argc < 0) || (argc > 6) || (argc && args==NULL) )
		fatal_error("wrong number of arguments: %d", argc);

	set_syscall(t, call);
	set_args(t, args, argc);
	set_trap_flag(t, 0);

	if (t->state == POST_CALL)
	{
		reset_syscall(t);
		next_trap(t, q);
	}
	else
		set_registers(t);

	if (ptrace(PTRACE_SYSCALL, t->pid, 0, 0) != 0)
		fatal_error("executing inject syscall failed");

	return orig_regs;
}

static long inject_syscall_finish(trace_t *t, registers_t *orig_regs,
                                  signal_queue_t *q)
{
	long retval;

	if (t->pid != waitpid(t->pid, NULL, __WALL))
		fatal_error("executing inject syscall failed");

	get_registers(t);

	retval = get_result(t);

	t->regs = *orig_regs;

	if (t->state == PRE_CALL)
	{
		reset_syscall(t);
		next_trap(t, q);
	}
	else
		set_registers(t);

	return retval;
}

long inject_syscall(trace_t *t, long call, long args[], int argc,
                    signal_queue_t *q)
{
	registers_t orig_regs = inject_syscall_init(t, call, args, argc, q);
	return inject_syscall_finish(t, &orig_regs, q);
}

long inject_data_syscall(trace_t *t, long call, arg_t args[], int argc,
                         signal_queue_t *q)
{
	int i;
	size_t mmap_size=0;
	long real_args[argc], base_addr, raddr, result;

	for (i=0; i<argc; i++)
		if ( args[i].flags & (TO_USER|FROM_USER) )
			mmap_size += args[i].size;

	mmap_size = ((mmap_size-1)|4095)+1;

	long mmap_args[] = { (long)NULL, mmap_size, PROT_READ|PROT_WRITE,
	                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0 };

	base_addr = inject_syscall(t, __NR_mmap2, mmap_args, 6, q);

	if ( base_addr < 0 && base_addr >= -516 )
		fatal_error("mmap failed, %lx", base_addr);

	raddr = base_addr;
	for (i=0; i<argc; i++)
	{
		if ( args[i].flags & FROM_USER )
			memstore_pipe(t, args[i].buf, (void *)raddr, args[i].size, q);

		if ( args[i].flags & (TO_USER|FROM_USER) )
		{
			real_args[i] = raddr;
			raddr += args[i].size;
		}
		else
			real_args[i] = args[i].value;
	}

	result = inject_syscall(t, call, real_args, argc, q);

	raddr = base_addr;
	for (i=0; i<argc; i++)
	{
		if ( args[i].flags & TO_USER )
			memload_pipe(t, args[i].buf, (void *)raddr, args[i].size, q);

		if ( args[i].flags & (TO_USER|FROM_USER) )
			raddr += args[i].size;
	}

	long munmap_args[] = { base_addr, mmap_size };

	if ( inject_syscall(t, __NR_munmap, munmap_args, 2, q) != 0)
		fatal_error("munmap failed, %ld bytes at %lx", mmap_size, base_addr);

	return result;
}

long inject_stat64(trace_t *t, char *file, struct stat64 *s, signal_queue_t *q)
{
	arg_t args[] =
	{
		[0] = { .buf = file, .size = strlen(file)+1, .flags = FROM_USER },
		[1] = { .buf = s, .size = sizeof(struct stat64), .flags = TO_USER },
	};
	return inject_data_syscall(t, __NR_lstat64, args, 2, q);
}

long inject_lstat64(trace_t *t, char *file, struct stat64 *s,signal_queue_t *q)
{
	arg_t args[] =
	{
		[0] = { .buf = file, .size = strlen(file)+1, .flags = FROM_USER },
		[1] = { .buf = s, .size = sizeof(struct stat64), .flags = TO_USER },
	};
	return inject_data_syscall(t, __NR_stat64, args, 2, q);
}

long inject_fstat64(trace_t *t, int fd, struct stat64 *s, signal_queue_t *q)
{
	arg_t args[] =
	{
		[0] = { .value = fd },
		[1] = { .buf = s, .size = sizeof(struct stat64), .flags = TO_USER },
	};
	return inject_data_syscall(t, __NR_fstat64, args, 2, q);
}

long inject_readlink(trace_t *t, char *path, char *buf, size_t bufsiz,
                     signal_queue_t *q)
{
	arg_t args[] =
	{
		[0] = { .buf = path, .size = strlen(path)+1, .flags = FROM_USER },
		[1] = { .buf = buf, .size = bufsiz, .flags = TO_USER },
		[2] = { .value = bufsiz },
	};
	return inject_data_syscall(t, __NR_readlink, args, 3, q);
}

mmap_data_t mmap_data(trace_t *t, void *laddr, void *raddr, size_t n, /*... ,*/
                      signal_queue_t *q)
{
	long size = ((n-1)|4095)+1;
	long args[] = { (long)NULL, size, PROT_READ|PROT_WRITE,
	                MAP_PRIVATE|MAP_ANONYMOUS, -1, 0 };

	long base_addr = inject_syscall(t, __NR_mmap2, args, 6, q);

	if ( base_addr < 0 && base_addr >= -516 )
		fatal_error("mmap failed, %lx", base_addr);

	memstore_pipe(t, laddr, raddr, n, q);

	return (mmap_data_t){ .base_addr = (void *)base_addr, .size = size };
}

void munmap_data(trace_t *t, mmap_data_t *map, signal_queue_t *q)
{
	long args[] = { (long)map->base_addr, map->size };

	if ( inject_syscall(t, __NR_munmap, args, 2, q) != 0)
		fatal_error("munmap failed, %ld bytes at %lx",
		            map->size, map->base_addr);
}

/* These are slow as hell
 *
 */

int memload(pid_t pid, void *laddr, void *raddr, size_t n)
{
	char *l=laddr, *r=raddr;
	long data; char *data_buf = (char *)&data;
	int offset = sizeof(long);

	while ( n )
	{
		if ( offset == sizeof(long))
		{
			offset = (long)r&(sizeof(long)-1);
			r -= offset;
			data = ptrace(PTRACE_PEEKDATA, pid, r, 0);
			r += sizeof(long);
		}

		*l++ = data_buf[offset++];
		n--;
	}

	return 0;
}

int memloadstr(pid_t pid, void *laddr, void *raddr, size_t max_size)
{
	char *l=laddr, *r=raddr;
	long data; char *data_buf = (char *)&data;
	int offset = sizeof(long);
	size_t c=0;

	while ( c < max_size )
	{
		if ( offset == sizeof(long))
		{
			offset = (long)r&(sizeof(long)-1);
			r -= offset;
			data = ptrace(PTRACE_PEEKDATA, pid, r, 0);
			r += sizeof(long);
		}

		*l = data_buf[offset++];

		if (*l == '\0')
			return c;

		c++;
		l++;
	}

	return -1;
}

int memstore(pid_t pid, void *laddr, void *raddr, size_t n)
{
	char *l=laddr, *r=raddr;
	long data; char *data_buf = (char *)&data;
	long align_off = (long)r & (sizeof(long)-1);

	r -= align_off;

	while ( n )
	{
		if ( align_off || n < sizeof(long) )
			data = ptrace(PTRACE_PEEKDATA, pid, r, 0);

		memcpy(data_buf+align_off, l, min(n, sizeof(long)-align_off));
		if ( ptrace(PTRACE_POKEDATA, pid, r, data) < 0 )
			return -1;

		n -= min(n, sizeof(long)-align_off);
		l += sizeof(long)-align_off;
		r += sizeof(long);
		align_off=0;
	}

	return 0;
}

/* changes the given filedescriptor to be the highest possible */
static int high_fd(int fd)
{
	struct rlimit lim, hard_lim;
	int newfd, i;

	getrlimit(RLIMIT_NOFILE, &lim);
	hard_lim.rlim_cur = hard_lim.rlim_max = lim.rlim_max;
	setrlimit(RLIMIT_NOFILE, &hard_lim);

	for (i=lim.rlim_max-1; fd < i; i--)
		if ( (newfd = fcntl(fd, F_DUPFD, i)) > -1)
		{
			close(fd);

			fd = newfd;

			if (lim.rlim_cur > fd)
				lim.rlim_cur = fd;

			break;
		}

	setrlimit(RLIMIT_NOFILE, &lim);

	return fd;
}

static int load_pipe[2] = { -1, -1 }, store_pipe[2] = { -1, -1 };

void init_pipe_channels(void)
{
	if ( pipe(load_pipe) < 0 || pipe(store_pipe) < 0 )
		fatal_error("pipe failed");

	/* close our side if the pipe on exec */
	fcntl(load_pipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(store_pipe[1], F_SETFD, FD_CLOEXEC);

	/* asynchronous I/O makes it possible to intercept
	 * signals delivered to the traced process
	 */
	fcntl(load_pipe[0], F_SETFL, O_RDONLY|O_ASYNC);
	fcntl(store_pipe[1], F_SETFL, O_WRONLY|O_ASYNC);

	load_pipe[1] = high_fd(load_pipe[1]);
	store_pipe[0] = high_fd(store_pipe[0]);
}

int is_pipe_channel(int fd)
{
	return fd == load_pipe[1] || fd == store_pipe[0];
}

/* I don't think this is conforming to POSIX pipe semantics,
 * but it's fast
 */

int memload_pipe(trace_t *t, void *laddr, void *raddr, size_t n,
                 signal_queue_t *q)
{
	long args[] = { load_pipe[1], (long)raddr, n };
	registers_t orig_regs = inject_syscall_init(t, __NR_write, args, 3, q);
	if (read(load_pipe[0], laddr, n) < 0)
		fatal_error("memload_pipe failed %s, %d <- %d",
		            strerror(errno), load_pipe[0], load_pipe[1]);

	return inject_syscall_finish(t, &orig_regs, q);
}

int memstore_pipe(trace_t *t, void *laddr, void *raddr, size_t n,
                  signal_queue_t *q)
{
	long args[] = { store_pipe[0], (long)raddr, n };
	registers_t orig_regs = inject_syscall_init(t, __NR_read, args, 3, q);
	if (write(store_pipe[1], laddr, n) < 0)
		fatal_error("memstore_pipe failed %s, %d -> %d",
		            strerror(errno), store_pipe[1], store_pipe[0]);

	return inject_syscall_finish(t, &orig_regs, q);
}
