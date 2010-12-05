
/* This file is part of minemu
 *
 * Copyright 2010 Erik Bosman <erik@minemu.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/limits.h>

#include <string.h>
#include <ctype.h>
#include <fcntl.h>

#include "mm.h"
#include "scratch.h"
#include "error.h"
#include "sigwrap.h"
#include "syscalls.h"

#include "taint_dump.h"
#include "hexdump.h"

int dump_on_exit = 0;
int dump_all = 0;

const char *regs_desc[] =
{
	"[   eax   ] [   ecx   ]  [   edx   ] [   ebx   ]",
	"[   esp   ] [   ebp   ]  [   esi   ] [   edi   ]",
};


void hexdump_taint(int fd, const void *data, ssize_t len,
                           const unsigned char *taint, int offset, int ascii,
                           const char *description[])
{
	const char *colors[256];
	int i;
	char *red = "\033[1;31m";
	colors[0] = "\033[0;37m";
	for(i=1; i<256; i++)
		colors[i] = red;

	hexdump(fd, data, len, offset, ascii, description, taint, colors);
}

static unsigned long read_addr(char *s)
{
	unsigned long addr = 0;
	int i;
	for(i=0; i<8; i++)
	{
		addr *= 16;
		switch (s[i])
		{
			case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
			case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
				addr += 9;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				addr += (s[i]&0xf);
				continue;
			default:
				die("not an address");
		}
	}
	return addr;
}

void dump_map(int fd, char *addr, unsigned long len)
{
	long *laddr;
	unsigned long i,j, last=0xFFFFFFFF;
	int t;

	i=0;

	/* trim the stack a bit */
	if ( dump_all && (long)addr == (long)(USER_END-USER_STACK_SIZE) )
	{
		for (; i<len; i++)
			if ( addr[i] || addr[i+TAINT_OFFSET] )
				break;

		for (; len>0; len--)
			if ( addr[len-1] || addr[len-1+TAINT_OFFSET] )
				break;

		i = PAGE_BASE(i);
		len = PAGE_NEXT(len);
	}

	for (; i<len; i+=PG_SIZE)
	{
		t = dump_all;

		if (!t)
		{
			laddr = (long *)&addr[i+TAINT_OFFSET];
			for (j=0; j<PG_SIZE/sizeof(long); j++)
				if (laddr[j])
					t=1;
		}

		if (t)
		{
			if (last == 0xFFFFFFFF)
				fd_printf(fd, "in map: %x (size %u)\n", addr, len);
			else if (i != last+PG_SIZE)
				fd_printf(fd, "...\n");

			hexdump_taint(fd, &addr[i], PG_SIZE,
			              (unsigned char *)&addr[i+TAINT_OFFSET], 1, 1, NULL);
			last = i;
		}
	}
}

static char taint_dump_dir_buf[PATH_MAX+1] = { 0, };

static char *taint_dump_dir = NULL;

void set_taint_dump_dir(const char *dir)
{
	if ( absdir(taint_dump_dir_buf, dir) == 0 )
		taint_dump_dir = taint_dump_dir_buf;
	else
		taint_dump_dir = NULL;
}

char *get_taint_dump_dir(void)
{
	return taint_dump_dir;
}

static char *get_taint_dump_filename(char *buf)
{
	buf[0] = '\x0';
	strcat(buf, taint_dump_dir);
	strcat(buf, "/taint_hexdump_");
	numcat(buf, sys_gettid());
	strcat(buf, ".dump");
	return buf;
}

void do_taint_dump(long *regs)
{
	if ( taint_dump_dir == NULL )
		return;

	unsigned long s_addr, e_addr;
	char buf[8];
	int fd = sys_open("/proc/self/maps", O_RDONLY, 0);
	char name[PATH_MAX+1+64];
	get_taint_dump_filename(name);
	int fd_out = sys_open(name, O_RDWR|O_CREAT, 0600), old_out;

	fd_printf(fd_out, "jump address:\n");

	hexdump_taint(fd_out, &user_eip, 4,
	              (unsigned char *)&ijmp_taint, 0,0, NULL);

	fd_printf(fd_out, "registers:\n");

	unsigned char regs_taint[32];
	get_xmm6(&regs_taint[0]);
	get_xmm7(&regs_taint[16]);
	hexdump_taint(fd_out, regs, 32, regs_taint, 0, 1, regs_desc);

#ifdef EMU_DEBUG
	print_last_gencode_opcode();
#endif

	do
	{
		sys_read(fd, buf, 8);
		s_addr = read_addr(buf);
		sys_read(fd, buf, 1);
		sys_read(fd, buf, 8);
		e_addr = read_addr(buf);
		sys_read(fd, buf, 2);
		sys_read(fd, buf, 1);
		if (buf[0] == 'w')
		{
			if (e_addr > USER_END)
				e_addr = USER_END;
			dump_map(fd_out, (char *)s_addr, e_addr-s_addr);
		}
		while (sys_read(fd, buf, 1) && buf[0] != '\n');
	}
	while (e_addr < USER_END);

	sys_close(fd);
	sys_close(fd_out);
}
