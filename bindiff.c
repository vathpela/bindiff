// SPDX-License-Identifier: GPLv3-or-later
/*
 * bindiff.c - diff some bin
 * Copyright Peter Jones <pjones@redhat.com>
 */

#include "bindiff.h"

#include <getopt.h>
#include <xdiff.h>

int verbose = 1;

static void NORETURN
usage(int ret)
{
	FILE *out = ret == 0 ? stdout : stderr;
	fprintf(out,
	        "Usage: %s [OPTION...] FILE FILE\n"
	        "Help options:\n"
	        "  -?, --help                        Show this help message\n"
	        "      --usage                       Display brief usage message\n",
	        program_invocation_short_name);
	exit(ret);
}

static int
get_map(const char *const filename, int *fd, mmbuffer_t *mmb)
{
	struct stat sb;
	int rc;
	int errnum;
	long sz;

	if (!filename || !fd || !mmb) {
		errno = EINVAL;
		return -1;
	}

	*fd = open(filename, O_RDONLY);
	if (*fd < 0)
		return -1;

	rc = fstat(*fd, &sb);
	if (rc < 0) {
		goto err_close;
	}

	mmb->size = sb.st_size;
	if (sb.st_size < 1) {
		mmb->ptr = calloc(1, 1);
		if (mmb->ptr == NULL) {
			rc = -1;
		}
	} else {
		mmb->ptr =
			mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, *fd, 0);
		if (mmb->ptr == MAP_FAILED) {
			rc = -1;
		}
	}
	if (rc < 0) {
		goto err_close;
	}

	return 0;

err_close:
	errnum = errno;
	close(*fd);
	*fd = -1;
	errno = errnum;
	return rc;
}

static void
put_map(int fd, mmbuffer_t *mmb)
{
	if (mmb->size < 1) {
		free(mmb->ptr);
	} else {
		munmap(mmb->ptr, mmb->size);
	}
	mmb->ptr = NULL;
	mmb->size = 0;

	close(fd);
}

struct hunk {
	hexdiff_op_t op;
	size_t apos, bpos;
	off_t aoff, boff;
	char *buf;
	size_t sz;
};

struct priv {
	char *files[2];
	mmbuffer_t *mmb1, *mmb2;
	bool first;
	size_t apos, bpos, opos;
	struct hunk *hunks;
	size_t n_hunks;
	size_t n_hunk_bufs;
};

static long
find(const char *line, long line_len, char *buf, long bufsz, void *priv)
{
	debug("line:%p-%p (0x%lx) buf:%p-%p (0x%lx)\n", line,
	      line + line_len - 1, line_len, buf, buf + bufsz - 1, bufsz);
	return 0;
}

#define inside(val, start, size)                       \
	((((uint64_t)(val)) >= ((uint64_t)(start))) && \
	 (((uint64_t)(val)) < (((uint64_t)(start)) + ((uint64_t)(size)))))

static void
add_hunk(struct priv *priv, hexdiff_op_t op, off_t aoff, off_t boff, char *buf,
         size_t sz)
{
	struct hunk *hunk;

	if (priv->n_hunks == priv->n_hunk_bufs) {
		struct hunk *hunks;

		hunks = realloc(priv->hunks, (priv->n_hunk_bufs + 1024) *
		                                     sizeof(struct hunk));
		if (!hunks)
			err(1, "Could not allocate memory");
		//debug("allocated %d hunks (%zu total) at %p", 1024, priv->n_hunk_bufs + 1024, hunks);
		priv->n_hunk_bufs += 1024;
		priv->hunks = hunks;
	}
	//debug("consuming hunk %zd at %p", priv->n_hunks, &priv->hunks[priv->n_hunks]);
	hunk = &priv->hunks[priv->n_hunks++];
	hunk->op = op;
	hunk->buf = buf;
	hunk->sz = sz;
	hunk->apos = priv->apos;
	hunk->bpos = priv->bpos;
	hunk->aoff = aoff;
	hunk->boff = boff;
}

static int
collect_insert(struct priv *priv, char *buf, size_t sz)
{
	debug("insert %p-%p (0x%lx) (from %s)", buf, buf + sz - 1, sz,
	      inside(buf, priv->mmb1->ptr, priv->mmb1->size)   ? "mmb1"
	      : inside(buf, priv->mmb2->ptr, priv->mmb2->size) ? "mmb2"
	                                                       : "????");
	off_t aoff = -1, boff = -1;
	if (inside(buf, priv->mmb1->ptr, priv->mmb1->size)) {
		aoff = buf - priv->mmb1->ptr;
	} else if (inside(buf, priv->mmb2->ptr, priv->mmb2->size)) {
		boff = buf - priv->mmb2->ptr;
	}
	//debug("priv:%p", priv);
	//add_hunk(priv, IGNORE, -1, -1, NULL, 0);
	add_hunk(priv, INSERT, aoff, boff, buf, sz);
	return 0;
}

static int
collect_copy(struct priv *priv, size_t off, size_t sz)
{
	debug("copy 0x%zx-0x%zx (0x%lx)", off, off + sz, sz);
	//debug("priv:%p", priv);
	//add_hunk(priv, IGNORE, NULL, 0);
	off_t aoff, boff = -1;
	aoff = off;
	add_hunk(priv, COPY, aoff, boff, priv->mmb1->ptr + priv->apos + off,
	         sz);
	return 0;
}

static int
collect(void *privp, mmbuffer_t *mmbuf, size_t count)
{
	struct priv *priv = (struct priv *)privp;
	int prev = 0;

	//debug("priv:%p", priv);
	if (priv->first == true) {
		uint32_t *adler32 = (uint32_t *)mmbuf->ptr;
		uint32_t *size = (uint32_t *)(mmbuf->ptr + sizeof(*adler32));

		priv->first = false;

		if (mmbuf->size != 8)
			errx(3, "adler32 mmbuf size is %zd, not 8",
			     mmbuf->size);
		debug("adler32 is 0x%04x, size is 0x%x", *adler32, *size);
		return 0;
	}

	for (size_t i = 0; i < count; i++) {
		char *p = mmbuf[i].ptr;
		size_t sz = mmbuf[i].size;
		size_t off;

		dprintf("mmbuf[%zd]: { %p-%p (0x%02lx) } ", i, p, p + sz - 1,
		        sz);
		if (sz < 1)
			errx(3, "op size is %zd", sz);
		dprintf("prev = 0x%x (%s) ", prev,
		        prev == XDL_BDOP_INS ? "XDL_BDOP_INS" : "NOOP");
		switch (prev) {
		case XDL_BDOP_INS:
			prev = 0;
			dprintf("\n");
			dhexdumpf("              ", p, sz);
			return collect_insert(priv, p, sz);
			break;
		default:
			dprintf("op = 0x%x (%s)\n", p[0],
			        p[0] == XDL_BDOP_INSB  ? "XDL_BDOP_INSB"
			        : p[0] == XDL_BDOP_INS ? "XDL_BDOP_INS"
			        : p[0] == XDL_BDOP_CPY ? "XDL_BDOP_CPY"
			                               : "UNKNOWN");
			switch (p[0]) {
			case XDL_BDOP_INSB:
				return collect_insert(priv, &p[1], 1);
			case XDL_BDOP_INS:
				prev = p[0];
				break;
			case XDL_BDOP_CPY:
				prev = 0;

				off = *(uint32_t *)&p[1];
				sz = *(uint32_t *)&p[5];
				return collect_copy(priv, off, sz);
			}
			break;
		}
	}
	return 0;
}

static void
collect_diff(struct priv *priv)
{
	xdemitcb_t emitcb = { .priv = (void *)priv, .outf = collect };
	bdiffparam_t bdp = {
		16,
	};
	int rc;

	rc = xdl_bdiff_mb(priv->mmb1, priv->mmb2, &bdp, &emitcb);
	if (rc < 0)
		err(2, "could not bdiff files");
}

static void
process_diff(struct priv *priv)
{
	for (size_t i = 0; i < priv->n_hunks; i++) {
		struct hunk *thishunk = &priv->hunks[i];
		if (i + 1 < priv->n_hunks) {
			struct hunk *nexthunk = &priv->hunks[i + 1];
			if (nexthunk->op == COPY && nexthunk->aoff != -1) {
				debug("thishunk: apos:0x%lx bpos:0x%lx aoff:0x%lx boff:0x%lx",
				      thishunk->apos, thishunk->bpos,
				      thishunk->aoff, thishunk->boff);
				debug("nexthunk: apos:0x%lx bpos:0x%lx aoff:0x%lx boff:0x%lx",
				      nexthunk->apos, nexthunk->bpos,
				      nexthunk->aoff, nexthunk->boff);
			}
		}
	}
}

struct palette {
	struct color normal;
	struct color delete;
	struct color copy;
	struct color insert;
};

struct palette palette = {
	.normal = { .attr = no_attr_change, .bg = white, .fg = black },
	.delete = { .attr = no_attr_change, .bg = white, .fg = black },
	.copy = { .attr = no_attr_change, .bg = white, .fg = black },
	.insert = { .attr = no_attr_change, .bg = white, .fg = black },
};

static void
emit_diff(struct priv *priv)
{
	for (size_t i = 0; i < priv->n_hunks; i++) {
		struct hunk *hunk = &priv->hunks[i];

		hexdiff(hunk->op, &priv->apos, &priv->bpos, hunk->buf,
		        hunk->sz);
	}
	//hexdiff(COPY, &priv->apos, &priv->bpos, priv->mmb1->ptr+priv->apos+off, sz);
	//hexdiff(INSERT, &priv->apos, &priv->bpos, buf, sz);
}

static void
do_diff(char *file[2], mmbuffer_t *mmb1, mmbuffer_t *mmb2)
{
	int rc;
	struct priv priv = {
		.files = { file[0], file[1] },
		.first = true,
		.mmb1 = mmb1,
		.mmb2 = mmb2,
	};

	priv.hunks = calloc(1024, sizeof(struct hunk));
	if (!priv.hunks)
		err(1, "Could not allocate memory");
	// debug("allocated %d hunks (%zu total) at %p", 1024, priv.n_hunk_bufs + 1024, priv.hunks);
	priv.n_hunk_bufs = 1024;

	debug("mmb1:%p = { %p-%p (0x%lx) }", mmb1, mmb1->ptr,
	      mmb1->ptr + mmb1->size, mmb1->size);
	debug("mmb2:%p = { %p-%p (0x%lx) }", mmb2, mmb2->ptr,
	      mmb2->ptr + mmb2->size, mmb2->size);

	collect_diff(&priv);
	process_diff(&priv);
	emit_diff(&priv);

	free(priv.hunks);
	priv.hunks = NULL;
	priv.n_hunks = 0;
	priv.n_hunk_bufs = 0;
}

int
main(int argc, char *argv[])
{
	char *sopts = "v?";
	struct option lopts[] = { { "help", no_argument, 0, '?' },
		                  { "quiet", no_argument, 0, 'q' },
		                  { "unified", no_argument, 0, 'u' },
		                  { "usage", no_argument, 0, 0 },
		                  { "verbose", no_argument, 0, 'v' },
		                  { 0, 0, 0, 0 } };
	int c = 0;
	int i = 0;
	char *files[] = { NULL, NULL };
	int fds[] = { -1, -1 };
	int rc;
	mmbuffer_t mmb1 = { 0, }, mmb2 = { 0, };

	while ((c = getopt_long(argc, argv, sopts, lopts, &i)) != -1) {
		debug("c:%c optarg:\"%s\"\n", c, optarg);
		switch (c) {
		case 'q':
			verbose -= 1;
			if (verbose < 0)
				verbose = 0;
			break;
		case 'u':
			/* for compatibility */
			break;
		case 'v':
			verbose += 1;
			if (verbose < 0 || verbose > INT_MAX)
				verbose = INT_MAX;
			break;
		case '?':
			usage(EXIT_SUCCESS);
			break;
		case 0:
			if (strcmp(lopts[i].name, "usage"))
				usage(EXIT_SUCCESS);
			break;
		}
	}
	unc_set_debug(NULL, verbose > 1);

	for (; optind < argc; optind++) {
		int x;
		for (x = 0; x < 2; x++) {
			if (files[x] == NULL) {
				files[x] = argv[optind];
				break;
			}
		}
		if (x == 2) {
			warnx("too many arguments");
			usage(EXIT_FAILURE);
		}
	}
	if (!files[0] || !files[1]) {
		warnx("too few arguments");
		usage(EXIT_FAILURE);
	}

	rc = get_map(files[0], &fds[0], &mmb1);
	if (rc < 0)
		err(1, "Could not open and map \"%s\"", files[0]);

	rc = get_map(files[1], &fds[1], &mmb2);
	if (rc < 0)
		err(1, "Could not open and map \"%s\"", files[1]);

	do_diff(files, &mmb1, &mmb2);

	put_map(fds[0], &mmb1);
	put_map(fds[1], &mmb2);

	return 0;

#if 0
	char tmpbuf[4096] = "";
	size_t tbsz = 4096, tmpsz = 0;
	struct color redfg = { .attr = 0, .bg = no_color_change, .fg = red };
	tmpsz = snprintf(tmpbuf+tmpsz, tbsz-tmpsz, "hello\n");
	tmpsz += sncprintf(tmpbuf+tmpsz, tbsz-tmpsz, redfg, "hello");
	tmpsz += snprintf(tmpbuf+tmpsz, tbsz-tmpsz, "hello\nhello\n");
	printf("%s", tmpbuf);
	return 0;
#endif
}

// vim:fenc=utf-8:tw=75:noet
