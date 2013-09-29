/*
 * - 08/21/2007 ATAG support added by Uli Luckas <u.luckas@road.de>
 *
 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <unistd.h>
#include <arch/options.h>
#include "../../kexec.h"
#include "../../kexec-syscall.h"
#include "crashdump-arm.h"

struct tag_header {
	uint32_t size;
	uint32_t tag;
};

/* The list must start with an ATAG_CORE node */
#define ATAG_CORE       0x54410001

struct tag_core {
	uint32_t flags;	    /* bit 0 = read-only */
	uint32_t pagesize;
	uint32_t rootdev;
};

/* it is allowed to have multiple ATAG_MEM nodes */
#define ATAG_MEM	0x54410002

struct tag_mem32 {
	uint32_t   size;
	uint32_t   start;  /* physical start address */
};

/* describes where the compressed ramdisk image lives (virtual address) */
/*
 * this one accidentally used virtual addresses - as such,
 * it's deprecated.
 */
#define ATAG_INITRD     0x54410005

/* describes where the compressed ramdisk image lives (physical address) */
#define ATAG_INITRD2    0x54420005

struct tag_initrd {
        uint32_t start;    /* physical start address */
        uint32_t size;     /* size of compressed ramdisk image in bytes */
};

/* command line: \0 terminated string */
#define ATAG_CMDLINE    0x54410009

struct tag_cmdline {
	char    cmdline[1];     /* this is the minimum size */
};

/* The list ends with an ATAG_NONE node. */
#define ATAG_NONE       0x00000000

struct tag {
	struct tag_header hdr;
	union {
		struct tag_core	 core;
		struct tag_mem32	mem;
		struct tag_initrd       initrd;
		struct tag_cmdline      cmdline;
	} u;
};

#define tag_next(t)     ((struct tag *)((uint32_t *)(t) + (t)->hdr.size))
#define byte_size(t)    ((t)->hdr.size << 2)
#define tag_size(type)  ((sizeof(struct tag_header) + sizeof(struct type) + 3) >> 2)

int zImage_arm_probe(const char *UNUSED(buf), off_t UNUSED(len))
{
	/* 
	 * Only zImage loading is supported. Do not check if
	 * the buffer is valid kernel image
	 */	
	return 0;
}

void zImage_arm_usage(void)
{
	printf(	"     --command-line=STRING Set the kernel command line to STRING.\n"
		"     --append=STRING       Set the kernel command line to STRING.\n"
		"     --initrd=FILE         Use FILE as the kernel's initial ramdisk.\n"
		"     --ramdisk=FILE        Use FILE as the kernel's initial ramdisk.\n"
		);
}

static
struct tag * atag_read_tags(void)
{
	unsigned long buf[1024];
	unsigned long *tags = NULL;
	size_t size = 0, read;
	const char fn[]= "/proc/atags";
	FILE *fp;
	fp = fopen(fn, "r");
	if (!fp) {
		fprintf(stderr, "Cannot open %s: %s\n", 
			fn, strerror(errno));
		return NULL;
	}

	do {
		read = fread(buf, sizeof(buf[1]), sizeof(buf)/sizeof(buf[1]), fp);
		if(ferror(fp)) {
			fprintf(stderr, "Cannot read %s: %s\n", fn, strerror(errno));
			goto fail;
		}

		tags = realloc(tags, (size+read)*sizeof(buf[1]));
		memcpy(tags+size, buf, read*sizeof(buf[1]));
		size += read;
	} while(!feof(fp));

	if (size == 0) {
		fprintf(stderr, "Read 0 atags bytes: %s\n", fn);
		goto fail;
	}

	goto exit;
fail:
	free(tags);
	tags = NULL;
exit:
	fclose(fp);
	return (struct tag *) tags;
}

static
void tag_buf_add(struct tag *t, char **buf, size_t *size)
{
	*buf = xrealloc(*buf, (*size) + byte_size(t));
	memcpy((*buf) + (*size), t, byte_size(t));
	*size += byte_size(t);
}

static
uint32_t *tag_buf_find_initrd_start(struct tag *buf)
{
	for(; byte_size(buf); buf = tag_next(buf))
		if(buf->hdr.tag == ATAG_INITRD2)
			return &buf->u.initrd.start;
	return NULL;
}

static
int atag_arm_load(struct kexec_info *info, unsigned long base,
	const char *command_line, off_t command_line_len,
	const char *initrd, off_t initrd_len, off_t initrd_off)
{
	struct tag *saved_tags = atag_read_tags();
	char *buf = NULL;
	size_t buf_size = 0;
	struct tag *params, *tag;
	uint32_t *initrd_start = NULL;

	params = xmalloc(getpagesize());
	if (!params) {
		fprintf(stderr, "Compiling ATAGs: out of memory\n");
		free(saved_tags);
		return -1;
	}
	memset(params, 0xff, getpagesize());

	if (saved_tags) {
		// Copy tags
		tag = saved_tags;
		while(byte_size(tag)) {
			switch (tag->hdr.tag) {
			case ATAG_INITRD:
			case ATAG_INITRD2:
			case ATAG_CMDLINE:
			case ATAG_NONE:
				// skip these tags
				break;
			default:
				// copy all other tags
				tag_buf_add(tag, &buf, &buf_size);
				break;
			}
			tag = tag_next(tag);
		}
		free(saved_tags);
	} else {
		params->hdr.size = 2;
		params->hdr.tag = ATAG_CORE;
		tag_buf_add(params, &buf, &buf_size);
		memset(params, 0xff, byte_size(params));
	}

	if (initrd) {
		params->hdr.size = tag_size(tag_initrd);
		params->hdr.tag = ATAG_INITRD2;
		params->u.initrd.size = initrd_len;

		tag_buf_add(params, &buf, &buf_size);
		memset(params, 0xff, byte_size(params));
	}

	if (command_line) {
		params->hdr.size = (sizeof(struct tag_header) + command_line_len + 3) >> 2;
		params->hdr.tag = ATAG_CMDLINE;
		memcpy(params->u.cmdline.cmdline, command_line,
			command_line_len);
		params->u.cmdline.cmdline[command_line_len - 1] = '\0';

		tag_buf_add(params, &buf, &buf_size);
		memset(params, 0xff, byte_size(params));
	}

	params->hdr.size = 0;
	params->hdr.tag = ATAG_NONE;
	tag_buf_add(params, &buf, &buf_size);

	free(params);

	add_segment(info, buf, buf_size, base, buf_size);

	if (initrd) {
		initrd_start = tag_buf_find_initrd_start((struct tag *)buf);
		if(!initrd_start)
		{
			fprintf(stderr, "Failed to find initrd start!\n");
			return -1;
		}

		*initrd_start = locate_hole(info, initrd_len, getpagesize(),
				initrd_off, ULONG_MAX, INT_MAX);
		if (*initrd_start == ULONG_MAX)
			return -1;
		add_segment(info, initrd, initrd_len, *initrd_start, initrd_len);
	}

	return 0;
}

int zImage_arm_load(int argc, char **argv, const char *buf, off_t len,
	struct kexec_info *info)
{
	unsigned long base;
	unsigned int atag_offset = 0x1000; /* 4k offset from memory start */
	unsigned int offset = 0x8000;      /* 32k offset from memory start */
	const char *command_line;
	char *modified_cmdline = NULL;
	off_t command_line_len;
	const char *ramdisk;
	char *ramdisk_buf;
	off_t ramdisk_length;
	off_t ramdisk_offset;
	int opt;
	/* See options.h -- add any more there, too. */
	static const struct option options[] = {
		KEXEC_ARCH_OPTIONS
		{ "command-line",	1, 0, OPT_APPEND },
		{ "append",		1, 0, OPT_APPEND },
		{ "initrd",		1, 0, OPT_RAMDISK },
		{ "ramdisk",		1, 0, OPT_RAMDISK },
		{ 0, 			0, 0, 0 },
	};
	static const char short_options[] = KEXEC_ARCH_OPT_STR "a:r:";

	/*
	 * Parse the command line arguments
	 */
	command_line = 0;
	command_line_len = 0;
	ramdisk = 0;
	ramdisk_buf = 0;
	ramdisk_length = 0;
	while((opt = getopt_long(argc, argv, short_options, options, 0)) != -1) {
		switch(opt) {
		default:
			/* Ignore core options */
			if (opt < OPT_ARCH_MAX) {
				break;
			}
		case '?':
			usage();
			return -1;
		case OPT_APPEND:
			command_line = optarg;
			break;
		case OPT_RAMDISK:
			ramdisk = optarg;
			break;
		}
	}
	if (command_line) {
		command_line_len = strlen(command_line) + 1;
		if (command_line_len > COMMAND_LINE_SIZE)
			command_line_len = COMMAND_LINE_SIZE;
	}
	if (ramdisk) {
		ramdisk_buf = slurp_file(ramdisk, &ramdisk_length);
	}

	/*
	 * If we are loading a dump capture kernel, we need to update kernel
	 * command line and also add some additional segments.
	 */
	if (info->kexec_flags & KEXEC_ON_CRASH) {
		uint64_t start, end;

		modified_cmdline = xmalloc(COMMAND_LINE_SIZE);
		if (!modified_cmdline)
			return -1;

		if (command_line) {
			(void) strncpy(modified_cmdline, command_line,
				       COMMAND_LINE_SIZE);
			modified_cmdline[COMMAND_LINE_SIZE - 1] = '\0';
		}

		if (load_crashdump_segments(info, modified_cmdline) < 0) {
			free(modified_cmdline);
			return -1;
		}

		command_line = modified_cmdline;
		command_line_len = strlen(command_line) + 1;

		/*
		 * We put the dump capture kernel at the start of crashkernel
		 * reserved memory.
		 */
		if (parse_iomem_single("Crash kernel\n", &start, &end)) {
			/*
			 * No crash kernel memory reserved. We cannot do more
			 * but just bail out.
			 */
			return -1;
		}
		base = start;
	} else {
		base = locate_hole(info,len+offset,0,0,ULONG_MAX,INT_MAX);
	}

	if (base == ULONG_MAX)
		return -1;

	/* assume the maximum kernel compression ratio is 4,
	 * and just to be safe, place ramdisk after that
	 */
	ramdisk_offset = base + len * 4;

	if (atag_arm_load(info, base + atag_offset,
			 command_line, command_line_len,
			 ramdisk_buf, ramdisk_length, ramdisk_offset) == -1)
		return -1;

	add_segment(info, buf, len, base + offset, len);

	info->entry = (void*)base + offset;

	return 0;
}
