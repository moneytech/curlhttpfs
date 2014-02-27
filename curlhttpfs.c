/* vim: set ts=8 sw=8 sts=8 noet ai tw=79:

  FUSE: Filesystem in Userspace. Heavily mutilated the Github
  louisje/curlhttpfs version.

  This program can be distributed under the terms of the GNU GPL.

  Author: Walter Doekes, 2014.
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <fuse_opt.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <curl/curl.h>
#include <curl/easy.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <stdarg.h>

void logmsg(const char *msg, ...) {
	FILE *fp;
	struct tm tmbuf;
	char buf[128];
	size_t len;
	time_t timebuf;
	va_list ap;

	timebuf = time(NULL);
	if (!localtime_r(&timebuf, &tmbuf)) {
		return;
	}

	fp = fopen("/tmp/curlhttpfs_debug.log", "ab");
	if (!fp) {
		return;
	}

	len = strftime(buf, 127, "%Y-%m-%d %H:%M:%S: ", &tmbuf);
	fwrite(buf, 1, len, fp);

	va_start(ap, msg);
	vfprintf(fp, msg, ap);
	va_end(ap);

	fclose(fp);
}

struct options {
	char *base_url;
} options;

#define MY_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

static struct fuse_opt httpfs_opts[] = {
	MY_OPT_KEY("base_url=%s", base_url, 0),
	FUSE_OPT_END
};

struct httpfs_file {
	char url[512];
	ssize_t size;
	unsigned is_dir:1;
};

struct httpfs_buffer {
	const char *url;

	CURL *curl;
	int status;
	ssize_t remote_start;
	ssize_t remote_chunk; /* unused, fixme */
	ssize_t remote_size;

	char *dest;
	size_t destlen;
	size_t len;

	unsigned in_header:1;
};

static int httpfs_getattr(const char* path, struct stat *stbuf)
{
	size_t pathlen = strlen(path);
	logmsg("httpfs_getattr: %s\n", path);
	
	/* Ok. This is kind of sad, but we have to return S_IFDIR for all paths
	 * leading up to the file we want. For now we'll use the period (".")
	 * to discern paths from files. If it has a period, we consider it a
	 * file. */

	if (pathlen == 1) { /* path is "/" */
		stbuf->st_mode = S_IFDIR | 0111;
		stbuf->st_nlink = 3; /* dir . and .. and 1 file */
	} else if (!strchr(path, '.')) {
		stbuf->st_mode = S_IFDIR | 0111;
		stbuf->st_nlink = 3; /* dir . and .. and 1 file */
	} else {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
	}
	return 0;
}

static int httpfs_opendir(const char *path, struct fuse_file_info *fi)
{
	return -EACCES;
}

static int httpfs_open(const char *path, struct fuse_file_info *fi)
{
	struct httpfs_file* fh;
	size_t pathlen = strlen(path);
	logmsg("httpfs_open: %s\n", path);

	if ((fi->flags & 3) != O_RDONLY) {
		return -EACCES;
	}

	fh = malloc(sizeof(*fh));
	fi->fh = (uint64_t)fh;
	strcpy(fh->url, options.base_url);
	strcat(fh->url, path);
	fh->size = -1;
	fh->is_dir = (pathlen == 0 || path[pathlen - 1] == '/');

	/* > Q: I can not know the file size in advance, how do I force
	 * >    EOF from fs_read() to be seen in the application?
	 * > A: Set direct_io in fs_open(). */
	fi->direct_io = 1;

	/* Don't know about keep_cache. The nonseekable makes sense. */
	fi->keep_cache = 1;
	fi->nonseekable = 1;

	return 0;
}

static int httpfs_release(const char *path, struct fuse_file_info *fi)
{
	struct httpfs_file* fh;
	logmsg("httpfs_release: %s\n", path);

	fh = (struct httpfs_file*)fi->fh;
	free(fh);
	fi->fh = 0;

	return 0; /* return value is ignored */
}

static size_t my_parse_status(struct httpfs_buffer *httpfs_buf,
			      const char *src, size_t len)
{
	long curl_response;
	CURLcode code = curl_easy_getinfo(httpfs_buf->curl,
					  CURLINFO_RESPONSE_CODE,
					  &curl_response);
	if (code != CURLE_OK) {
		logmsg("my_write_callback: bad response code\n");
		return (size_t)-1;
	}

	if (curl_response == 416) {
		logmsg("my_write_callback: a fetch too many? got 416\n");
		return len;
	}

	if (curl_response < 200 || 300 <= curl_response) {
		logmsg("my_write_callback: bad response code %ld\n", curl_response);
		return (size_t)-1;
	}

	httpfs_buf->status = (int)curl_response;
	httpfs_buf->in_header = 1;

	return len;
}

static size_t my_parse_header(struct httpfs_buffer *httpfs_buf,
			      const char *src, size_t len)
{
	if (len == 2 && memcmp(src, "\r\n", 2) == 0) {
		if (httpfs_buf->status == 206 && httpfs_buf->remote_size == -1) {
			logmsg("my_write_callback: Got 206 but no usable Content-Range\n");
			return (size_t)-1;
		}
		httpfs_buf->in_header = 0;
	}

	if (httpfs_buf->status == 206) {
		/* HTTP/1.1 206 Partial Content
		 * Accept-Ranges: bytes
		 * Content-Length: 375
		 * Content-Range: bytes 0-374/375 */
		if (strncasecmp(src, "Content-Range:", 14) == 0) {
			unsigned long r1, r2, r3;
			src += 14;
			if (sscanf(src, " bytes %lu-%lu/%lu ",
				   &r1, &r2, &r3) == 3) {
				httpfs_buf->remote_start = (ssize_t)r1;
				httpfs_buf->remote_chunk = (ssize_t)(r2 + 1 - r1);
				httpfs_buf->remote_size = (ssize_t)r3;
			}
		}
	} else {
		/* FIXME.. handle 200 with manual skipping/filtering..
		 * */
		logmsg("my_write_callback: not implemented status code %d\n",
		       httpfs_buf->status);
		return (size_t)-1;
	}

	return len;
}

static size_t my_write_callback(void *ptr, size_t size, size_t nmemb,
				void *stream)
{
	/* Whenever we encounter an error, we return (size_it)-1:
	 * > Return the number of bytes actually taken care of.
	 * > If that amount differs from the amount passed to your
	 * > function, it'll signal an error to the library. */
	size_t total_size = size * nmemb;
	size_t room_left;
	struct httpfs_buffer *httpfs_buf = (struct httpfs_buffer *)stream;

	/* Is this the first result? Fetch status. */
	if (!httpfs_buf->status) {
		return my_parse_status(httpfs_buf, ptr, total_size);
	}
	/* Are we reading headers? Fetch those. */
	if (httpfs_buf->in_header) {
		return my_parse_header(httpfs_buf, ptr, total_size);
	}

	/* ... */
	room_left = httpfs_buf->destlen - httpfs_buf->len;
	if (room_left >= total_size) {
		memcpy(httpfs_buf->dest + httpfs_buf->len, ptr, total_size);
		httpfs_buf->len += total_size;
	} else {
		memcpy(httpfs_buf->dest + httpfs_buf->len, ptr, room_left);
		httpfs_buf->len += room_left;
	}

	/* Pretend everything went ok, even if we got too much */
	return total_size;
}

static ssize_t read_curl_buffer(size_t size, off_t offset,
			        struct httpfs_buffer *httpfs_buf)
{
	char range[100];
	CURL *curl;
	CURLcode code;

	/* Always try specifying a range. However, not all web servers
	 * (or individual pages) support this. */
	sprintf(range, "%llu-%llu", (unsigned long long)offset,
		(unsigned long long)offset + (unsigned long long)size - 1);

	curl = curl_easy_init();
	if (!curl) {
		return -1;
	}

	/* Pass curl along so we can check the range in my_write_callback. */
	httpfs_buf->curl = curl;

	logmsg("read_curl_buffer: url=%s, range=%s\n", httpfs_buf->url, range);
	curl_easy_setopt(curl, CURLOPT_URL, httpfs_buf->url);

	/* my_write_callback is called once for every header, and then
	 * a couple of times with the data chunks. */
	curl_easy_setopt(curl, CURLOPT_HEADER, 1);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, httpfs_buf);

	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1);
	curl_easy_setopt(curl, CURLOPT_RANGE, range);

	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		logmsg("read_curl_buffer: got error: %s\n",
		       curl_easy_strerror(code));
		curl_easy_cleanup(curl);
		return -1;
	}

	curl_easy_cleanup(curl);
	return httpfs_buf->len;
}

static int httpfs_read(const char *path, char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
	struct httpfs_file *fh = (struct httpfs_file*)fi->fh;
	struct httpfs_buffer httpfs_buf = {0,};
	size_t pathlen = strlen(path);
	ssize_t res;
	logmsg("httpfs_read: %s\n", path);

	/* The open on the dir should succeed. First the read() should
	 * fail. Note that this never happens, as the directory
	 * traversal mechanism tries the preceding directories
	 * first (opendir?) */
	if (pathlen == 0 || path[pathlen - 1] == '/') {
		return -EISDIR;
	}

	/* If this is a second read, we may have a filesize already. */
	if (fh->size != -1) {
		if (offset >= fh->size) {
			return 0;
		}
		if (size > fh->size - offset) {
			size = fh->size - offset;
		}
	}

	/* Init our parameters. */
	httpfs_buf.url = fh->url;
	httpfs_buf.dest = buf;
	httpfs_buf.destlen = size;
	httpfs_buf.remote_size = -1;

	res = read_curl_buffer(size, offset, &httpfs_buf);
	if (res < 0) {
		logmsg("enoent!\n", buf, res);
		return -ENOENT;
	}

	/* If we didn't have a size already, perhaps now we do. */
	if (fh->size == -1) {
		fh->size = httpfs_buf.remote_size;
	}

	return (int)res;
}

static void *httpfs_init(struct fuse_conn_info *conn)
{
	logmsg("httpfs_init\n");
	/* Initialize curl (when still single threaded) */
	curl_global_init(CURL_GLOBAL_SSL);

	return NULL; /* userdata */
}

static void httpfs_destroy(void *userdata)
{
	logmsg("httpfs_destroy\n");
	/* TODO: fclose open files? */
	curl_global_cleanup();
}

static struct fuse_operations httpfs_oper = {
	/* Init the fs */
	.init		= httpfs_init,
	.destroy	= httpfs_destroy,
	/* open/read/close */
	.open		= httpfs_open,
	.read		= httpfs_read,
	.release	= httpfs_release,  /* every open needs a close */
	/* We require getattr, and opendir makes things behave */
	.getattr	= httpfs_getattr,
	.opendir	= httpfs_opendir,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	memset(&options, 0, sizeof(struct options));

	if (fuse_opt_parse(&args, &options, httpfs_opts, NULL) == -1)
	{
		fprintf(stderr, "ERROR: Fail to parse arguments!\n");
		return -1;
	}

	if (!options.base_url)
	{
		fprintf(stderr, "`base_url` is not specified!\n");
		return -1;
	}

	if (strlen(options.base_url) > 512)
	{
		fprintf(stderr, "ERROR: Target URL has ridiculous length!\n");
		return -1;
	}

	/* Background and start the fs (fuse is multithreaded by default) */
	if (fuse_main(args.argc, args.argv, &httpfs_oper, NULL))
	{
		printf("\n");
	}

	/* Free fuse */
	fuse_opt_free_args(&args);

	return 0;
}
