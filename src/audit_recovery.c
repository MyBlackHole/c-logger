#define _GNU_SOURCE
#include "audit_record.h"
#include "logger_fault.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint32_t audit_crc32(const void *p_, size_t n)
{
	const unsigned char *p = p_;
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & -(int32_t)(c & 1));
	}
	return ~c;
}

static int open_regular(const char *, int, FILE **, struct stat *);

int audit_checkpoint_load(const char *path, audit_ckpt_t *out)
{
	if (!path || !out) {
		errno = EINVAL;
		return -1;
	}
	FILE *f = NULL;
	struct stat st;
	int opened = open_regular(path, 0, &f, &st);
	if (opened) {
		if (opened != -ENOENT) {
			errno = -opened;
			return -1;
		}
		*out = (audit_ckpt_t){ 0 };
		return 0;
	}
	char line[AUDIT_CHECKPOINT_MAX + 1u];
	size_t length;
	int kind = audit_line_read(f, line, AUDIT_CHECKPOINT_MAX, &length);
	audit_ckpt_t parsed;
	int rc = kind == AUDIT_LINE_COMPLETE ?
			 audit_checkpoint_parse(line, length, &parsed) :
		 kind < 0 ? kind :
			    -EBADMSG;
	if (!rc) {
		kind = audit_line_read(f, line, AUDIT_CHECKPOINT_MAX, &length);
		if (kind != AUDIT_LINE_EOF)
			rc = kind < 0 ? kind : -EBADMSG;
	}
	if (fclose(f) && !rc)
		rc = -(errno ? errno : EIO);
	if (rc) {
		errno = -rc;
		return -1;
	}
	*out = parsed;
	return 0;
}
/* Persist derived state without leaking a descriptor on an earlier error.
 * A rename followed by failed directory fsync is still a failed checkpoint:
 * the destination may have changed, so callers retry the SAME committed head.
 */
int audit_checkpoint_persist(const char *path, const audit_ckpt_t *cp)
{
	if (!path || !cp || cp->offset > INT64_MAX ||
	    !audit_digest_provider((audit_integrity_t)cp->algorithm)) {
		errno = EINVAL;
		return -1;
	}
	char hex[65], canon[448], line[512], tmp[AUDIT_PATH_MAX],
		dir[AUDIT_PATH_MAX];
	audit_hash_hex(cp->hash, hex);
	int n = snprintf(
		canon, sizeof(canon),
		"AUDIT_STATE version=2 alg=%u seq=%llu offset=%llu hash=%s",
		cp->algorithm, (unsigned long long)cp->seq,
		(unsigned long long)cp->offset, hex);
	if (n < 0 || (size_t)n >= sizeof(canon)) {
		errno = EOVERFLOW;
		return -1;
	}
	uint32_t crc = audit_crc32(canon, (size_t)n);
	int m = snprintf(line, sizeof(line), "%s crc=%08x\n", canon, crc);
	n = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
	int k = snprintf(dir, sizeof(dir), "%s", path);
	if (m < 0 || (size_t)m >= sizeof(line) || n < 0 ||
	    (size_t)n >= sizeof(tmp) || k < 0 || (size_t)k >= sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	char *slash = strrchr(dir, '/');
	if (slash) {
		if (slash == dir)
			slash[1] = '\0';
		else
			*slash = '\0';
	} else {
		memcpy(dir, ".", 2);
	}
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -1;
	int fd = mkostemp(tmp, O_CLOEXEC); /* private unique 0600 inode */
	int error = 0;
	int owns_tmp = fd >= 0;
	if (fd < 0) {
		error = errno;
		goto out;
	}
	size_t off = 0;
	while (off < (size_t)m) {
		ssize_t written;
		if (logger_fault_should_fail(LOGGER_FAULT_STATE_WRITE))
			written = -1;
		else
			written = write(fd, line + off, (size_t)m - off);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			goto out;
		}
		if (!written) {
			error = EIO;
			goto out;
		}
		off += (size_t)written;
	}
	for (;;) {
		int rc = logger_fault_should_fail(LOGGER_FAULT_STATE_FSYNC) ?
				 -1 :
				 fsync(fd);
		if (!rc)
			break;
		if (errno != EINTR) {
			error = errno;
			goto out;
		}
	}
	if (close(fd)) {
		error = errno;
		fd = -1; /* Linux close errors do not license retry of this fd number */
		goto out;
	}
	fd = -1;
	logger_fault_crash_if_requested("before_state_rename");
	if (logger_fault_should_fail(LOGGER_FAULT_STATE_RENAME) ||
	    rename(tmp, path)) {
		error = errno;
		goto out;
	}
	owns_tmp = 0;
	logger_fault_crash_if_requested("after_state_rename");
	while (fsync(dfd)) {
		if (errno != EINTR) {
			error = errno;
			goto out;
		}
	}
out:
	if (fd >= 0 && close(fd) && !error)
		error = errno;
	if (owns_tmp)
		(void)unlink(tmp);
	if (close(dfd) && !error)
		error = errno;
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

#define AUDIT_ARCHIVE_LIMIT 4096u

typedef struct {
	char *path;
	unsigned char first_prev[32], last_hash[32];
	uint64_t end, seq;
	int has_records;
	int visited;
} segment_t;

static int timestamp_name(const char *s, size_t n)
{
	if (n != 23 || s[8] != 'T' || s[15] != '.' || s[22] != 'Z')
		return 0;
	for (size_t i = 0; i < n; ++i) {
		if (i == 8 || i == 15 || i == 22)
			continue;
		if (s[i] < '0' || s[i] > '9')
			return 0;
	}
	return 1;
}

/* Recognize exactly both archive layouts. Current writer uses the first one;
 * old documented/exported layout uses the second. Names do NOT order the chain. */
static int archive_name(const char *name, const char *base)
{
	size_t n = strlen(name), b = strlen(base);
	if (n != b + 34u || memcmp(name, base, b))
		return 0;
	const char *p = name + b;
	return (!memcmp(p, ".audit.", 7) && timestamp_name(p + 7, 23) &&
		!memcmp(p + 30, ".log", 4)) ||
	       (p[0] == '.' && timestamp_name(p + 1, 23) &&
		!memcmp(p + 24, ".audit.log", 10));
}

static int add_segment(segment_t *files, size_t *count, const char *dir,
		       const char *name)
{
	if (*count == AUDIT_ARCHIVE_LIMIT)
		return -E2BIG;
	char path[AUDIT_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;
	files[*count].path = strdup(path);
	if (!files[*count].path)
		return -ENOMEM;
	++*count;
	return 0;
}

static int open_regular(const char *path, int writable, FILE **out,
			struct stat *st)
{
	int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC |
				    O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	int rc = fstat(fd, st) ? -errno : 0;
	if (!rc && (!S_ISREG(st->st_mode) || st->st_size < 0))
		rc = -EINVAL;
	if (rc) {
		(void)close(fd);
		return rc;
	}
	FILE *f = fdopen(fd, writable ? "r+" : "r");
	if (!f) {
		rc = -errno;
		(void)close(fd);
		return rc;
	}
	*out = f;
	return 0;
}

static int sync_fd(int fd)
{
	while (fsync(fd)) {
		if (errno != EINTR)
			return -errno;
	}
	return 0;
}

/* Scan without modifying files. Every complete record, even before a checkpoint,
 * is parsed and self-verified. Checkpoint offset is matched to an actual verified
 * record, NEVER accepted simply because a file is large enough. */
static int scan_segment(FILE *f, segment_t *file, const audit_ckpt_t *cp,
			const audit_digest_ops_t *digest, unsigned *anchors,
			char *partial, size_t *partial_length)
{
	char line[AUDIT_RECORD_MAX + 2u];
	for (;;) {
		size_t length;
		int kind = audit_line_read(f, line, AUDIT_RECORD_MAX, &length);
		if (kind < 0)
			return kind;
		if (kind == AUDIT_LINE_EOF)
			break;
		if (kind == AUDIT_LINE_PARTIAL) {
			if (!partial)
				return -EBADMSG; /* archives are never repaired */
			audit_record_view_t complete;
			line[length] = '\n';
			/* Missing LF alone is not sufficient reason to erase a complete
             * record (even one with a wrong hash). Require operator review. */
			if (!audit_record_parse_line(line, length + 1u,
						     &complete))
				return -EBADMSG;
			memcpy(partial, line, length);
			*partial_length = length;
			break;
		}
		audit_record_view_t record;
		int rc = audit_record_parse_line(line, length, &record);
		if (rc)
			return rc;
		if (!file->has_records)
			memcpy(file->first_prev, record.previous, 32);
		rc = audit_record_verify(&record,
					 file->has_records ? file->last_hash :
							     record.previous,
					 digest, file->last_hash);
		if (rc)
			return rc;
		file->has_records = 1;
		file->seq = record.seq;
		if (file->end > INT64_MAX - length)
			return -EOVERFLOW;
		file->end += length;
		if (cp->offset == file->end && cp->seq == record.seq &&
		    !memcmp(cp->hash, record.hash, 32))
			++*anchors;
	}
	/* offset=0/nonzero hash represents the boundary after an archived segment
     * when the active path had not yet been created at the last recovery. */
	if (!cp->offset && file->has_records && cp->seq == file->seq &&
	    !memcmp(cp->hash, file->last_hash, 32))
		++*anchors;
	return 0;
}

static int same_stat(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
	       a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
	       a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
	       a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
	       a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Persist exact evidence before modifying a true EOF fragment. This is not a
 * diagnosis of a crash: manual truncation/tampering can produce the same bytes.
 * Never repair an archive, oversized input or a complete malformed line. */
static int preserve_and_trim(FILE *f, const char *active, const char *tail,
			     size_t length, uint64_t last,
			     const struct stat *scanned)
{
	struct stat now, named;
	if (fstat(fileno(f), &now) || lstat(active, &named))
		return -errno;
	if (!same_stat(scanned, &now) || !same_stat(scanned, &named))
		return -ESTALE;
	char path[AUDIT_PATH_MAX], dir[AUDIT_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s.tail.XXXXXX", active);
	int m = snprintf(dir, sizeof(dir), "%s", active);
	if (n < 0 || (size_t)n >= sizeof(path) || m < 0 ||
	    (size_t)m >= sizeof(dir))
		return -ENAMETOOLONG;
	char *slash = strrchr(dir, '/');
	if (!slash)
		memcpy(dir, ".", 2);
	else if (slash == dir)
		slash[1] = 0;
	else
		*slash = 0;
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		return -errno;
	int fd = mkostemp(path, O_CLOEXEC);
	int rc = fd < 0 ? -errno : 0;
	if (fd >= 0) {
		size_t offset = 0;
		while (!rc && offset < length) {
			ssize_t written =
				write(fd, tail + offset, length - offset);
			if (written < 0) {
				if (errno != EINTR)
					rc = -errno;
			} else if (!written)
				rc = -EIO;
			else
				offset += (size_t)written;
		}
		if (!rc)
			rc = sync_fd(fd);
		if (close(fd) && !rc)
			rc = -errno;
		if (rc)
			(void)unlink(
				path); /* incomplete evidence; original untouched */
		else
			rc = sync_fd(
				dfd); /* preserve evidence path before truncation */
	}
	if (close(dfd) && !rc)
		rc = -errno;
	if (rc)
		return rc;
	if (fstat(fileno(f), &now) || lstat(active, &named))
		return -errno;
	if (!same_stat(scanned, &now) || !same_stat(scanned, &named))
		return -ESTALE;
	off_t offset = (off_t)last;
	if (offset < 0 || (uint64_t)offset != last)
		return -EOVERFLOW;
	for (;;) {
		int status =
			logger_fault_should_fail(LOGGER_FAULT_FILE_FTRUNCATE) ?
				-1 :
				ftruncate(fileno(f), offset);
		if (!status)
			break;
		if (errno != EINTR)
			return -errno;
	}
	return sync_fd(fileno(f));
}

int audit_recover_set(const char *dir, const char *name, const char *active,
		      audit_ckpt_t *out)
{
	if (!dir || !name || !active || !out) {
		errno = EINVAL;
		return -1;
	}
	const audit_digest_ops_t *digest =
		audit_digest_provider((audit_integrity_t)out->algorithm);
	int rc = audit_digest_check(digest);
	if (rc) {
		errno = -rc;
		return -1;
	}
	audit_ckpt_t next = *out;
	const unsigned char zero[32] = { 0 };
	int genesis = !next.seq && !next.offset && !memcmp(next.hash, zero, 32);
	segment_t *files = calloc(AUDIT_ARCHIVE_LIMIT + 1u, sizeof(*files));
	if (!files) {
		errno = ENOMEM;
		return -1;
	}
	size_t count = 0, active_index = 0;
	FILE *active_file = NULL;
	DIR *d = opendir(dir);
	if (!d) {
		rc = -errno;
		goto done;
	}
	for (;;) {
		errno = 0;
		struct dirent *e = readdir(d);
		if (!e) {
			if (errno)
				rc = -errno;
			break;
		}
		if (archive_name(e->d_name, name)) {
			rc = add_segment(files, &count, dir, e->d_name);
			if (rc)
				break;
		}
	}
	if (closedir(d) && !rc)
		rc = -errno;
	if (rc)
		goto done;
	active_index = count;
	files[count].path = strdup(active);
	if (!files[count].path) {
		rc = -ENOMEM;
		goto done;
	}
	++count;
	unsigned anchors = 0;
	char tail[AUDIT_RECORD_MAX + 1u];
	size_t tail_length = 0, nonempty = 0;
	struct stat active_stat = { 0 };
	for (size_t i = 0; i < count; ++i) {
		FILE *f = NULL;
		struct stat st;
		int is_active = i == active_index;
		rc = open_regular(files[i].path, is_active, &f, &st);
		if (rc == -ENOENT && is_active) {
			rc = 0;
			continue;
		}
		if (rc)
			goto done;
		if (is_active) {
			active_file = f;
			active_stat = st;
		}
		rc = scan_segment(f, &files[i], &next, digest, &anchors,
				  is_active ? tail : NULL, &tail_length);
		struct stat after;
		if (!rc && fstat(fileno(f), &after))
			rc = -errno;
		if (!rc && !same_stat(&st, &after))
			rc = -ESTALE;
		/* Valid bytes recovered beyond an old checkpoint must be synced before
         * a new checkpoint can acknowledge them. No application data rewrite. */
		if (!rc && !is_active)
			rc = sync_fd(fileno(f));
		if (!is_active && fclose(f) && !rc)
			rc = -errno;
		if (rc)
			goto done;
		nonempty += (unsigned)files[i].has_records;
	}
	if ((!genesis && anchors != 1) || (genesis && anchors)) {
		rc = -EBADMSG;
		goto done;
	}
	/* Form a UNIQUE chain of all retained nonempty segments by their verified
     * digest edges. UTC filenames are hints only (clock can move backwards).
     * Missing/duplicated/branched segments fail closed instead of being skipped. */
	size_t root = count, roots = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!files[i].has_records)
			continue;
		unsigned predecessors = 0;
		for (size_t j = 0; j < count; ++j)
			if (files[j].has_records &&
			    !memcmp(files[j].last_hash, files[i].first_prev,
				    32))
				++predecessors;
		if (predecessors > 1) {
			rc = -EBADMSG;
			goto done;
		}
		if (!predecessors) {
			root = i;
			++roots;
		}
	}
	if (nonempty && roots != 1) {
		rc = -EBADMSG;
		goto done;
	}
	if (genesis && nonempty && memcmp(files[root].first_prev, zero, 32)) {
		rc = -EBADMSG;
		goto done;
	}
	size_t visited = 0, last = count;
	while (root < count) {
		if (files[root].visited) {
			rc = -EBADMSG;
			goto done;
		}
		files[root].visited = 1;
		++visited;
		last = root;
		unsigned successors = 0;
		size_t follow = count;
		for (size_t i = 0; i < count; ++i) {
			if (files[i].has_records &&
			    !memcmp(files[root].last_hash, files[i].first_prev,
				    32)) {
				++successors;
				follow = i;
			}
		}
		if (successors > 1 || (successors && root == active_index)) {
			rc = -EBADMSG;
			goto done;
		}
		root = follow;
	}
	if (visited != nonempty ||
	    (files[active_index].has_records && last != active_index)) {
		rc = -EBADMSG;
		goto done;
	}
	if (last < count) {
		memcpy(next.hash, files[last].last_hash, 32);
		next.seq = files[last].seq;
	}
	next.offset = files[active_index].end;
	if (tail_length) {
		rc = preserve_and_trim(active_file, active, tail, tail_length,
				       next.offset, &active_stat);
		if (rc)
			goto done;
	} else if (active_file) {
		rc = sync_fd(fileno(active_file));
		if (rc)
			goto done;
	}
done:
	if (active_file && fclose(active_file) && !rc)
		rc = -errno;
	for (size_t i = 0; i < count; ++i)
		free(files[i].path);
	free(files);
	if (rc) {
		errno = -rc;
		return -1;
	}
	*out = next;
	return 0;
}
