#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "pkg.h"
#include "pkg_event.h"
#include "pkg_private.h"


struct hardlinks {
	ino_t *inodes;
	size_t len;
	size_t cap;
};

static void
sbuf_append(struct sbuf *buf, __unused const char *comment, const char *str, ...)
{
	va_list ap;

	va_start(ap, str);

/*	if (sbuf_len(buf) == 0)
		sbuf_printf(buf, "#@%s\n", comment);*/

	sbuf_vprintf(buf, str, ap);
	va_end(ap);
}

#define post_unexec_append(buf, str, ...) sbuf_append(buf, "unexec", str, __VA_ARGS__)
#define pre_unexec_append(buf, str, ...) sbuf_append(buf, "unexec", str, __VA_ARGS__)
#define exec_append(buf, str, ...) sbuf_append(buf, "exec", str, __VA_ARGS__)

int
ports_parse_plist(struct pkg *pkg, char *plist)
{
	bool ignore_next = false;
	char *plist_p, *buf, *p, *plist_buf;
	char comment[2];
	int nbel, i;
	size_t next;
	size_t len;
	size_t j;
	char sha256[SHA256_DIGEST_LENGTH * 2 + 1];
	char path[MAXPATHLEN + 1];
	char *last_plist_file = NULL;
	char *cmd = NULL;
	const char *prefix = NULL;
	struct stat st;
	int ret = EPKG_OK;
	off_t sz = 0;
	const char *slash;
	int64_t flatsize = 0;
	struct hardlinks hardlinks = {NULL, 0, 0};
	bool regular;
	struct sbuf *exec_scripts = sbuf_new_auto();
	struct sbuf *unexec_scripts = sbuf_new_auto();
	struct sbuf *pre_unexec_scripts = sbuf_new_auto();
	struct sbuf *post_unexec_scripts = sbuf_new_auto();
	void *set = NULL;
	const char *uname = NULL;
	const char *gname = NULL;
	mode_t perm=0;
	regex_t preg1, preg2;
	regmatch_t pmatch[2];

	regcomp(&preg1, "[[:space:]]\"(/[^\"]+)", REG_EXTENDED);
	regcomp(&preg2, "[[:space:]](/[[:graph:]/]+)", REG_EXTENDED);

	buf = NULL;
	p = NULL;

	assert(pkg != NULL);
	assert(plist != NULL);

	if ((ret = file_to_buffer(plist, &plist_buf, &sz)) != EPKG_OK)
		return (ret);

	pkg_get(pkg, PKG_PREFIX, &prefix);
	slash = prefix[strlen(prefix) - 1] == '/' ? "" : "/";

	nbel = split_chr(plist_buf, '\n');

	next = strlen(plist_buf);
	plist_p = plist_buf;

	for (i = 0; i <= nbel; i++) {
		if (ignore_next)
			continue;

		if (plist_p[0] == '@') {
			if (STARTS_WITH(plist_p, "@cwd")) {
				buf = plist_p;
				buf += 4;
				while (isspace(buf[0]))
					buf++;
				/* with no arguments default to the original
				 * prefix */
				if (buf[0] == '\0')
					pkg_get(pkg, PKG_PREFIX, &prefix);
				else
					prefix = buf;
				slash = prefix[strlen(prefix) - 1] == '/' ? "" : "/";
			} else if (STARTS_WITH(plist_p, "@ignore")) {
				/* ignore the next line */
				ignore_next = true;
			} else if (STARTS_WITH(plist_p, "@comment ")) {
				/* DO NOTHING: ignore the comments */
			} else if (STARTS_WITH(plist_p, "@unexec ") ||
					   STARTS_WITH(plist_p, "@exec ")) {
				buf = plist_p;

				while (!isspace(buf[0]))
					buf++;

				while (isspace(buf[0]))
					buf++;

				if (format_exec_cmd(&cmd, buf, prefix, last_plist_file) != EPKG_OK)
					continue;

				if (plist_p[1] == 'u') {
					comment[0] = '\0';
					/* workaround to detect the @dirrmtry */
					if (STARTS_WITH(cmd, "rmdir ")) {
						comment[0] = '#';
						comment[1] = '\0';
					} else if (STARTS_WITH(cmd, "/bin/rmdir ")) {
						comment[0] = '#';
						comment[1] = '\0';
					}
					/* remove the glob if any */
					if (comment[0] == '#') {
						if (strchr(cmd, '*'))
						comment[0] = '\0';

						buf = cmd;

						/* start remove mkdir -? */
						/* remove the command */
						while (!isspace(buf[0]))
							buf++;

						while (isspace(buf[0]))
							buf++;

						if (buf[0] == '-')
							comment[0] = '\0';
						/* end remove mkdir -? */
					}

					/* more workarounds */
					if (strstr(cmd, "rmdir") || strstr(cmd, "kldxref") ||
					    strstr(cmd, "mkfontscale") || strstr(cmd, "mkfontdir") ||
					    strstr(cmd, "fc-cache") || strstr(cmd, "fonts.dir") ||
					    strstr(cmd, "fonts.scale")) {
						if (comment[0] != '#')
							post_unexec_append(post_unexec_scripts, "%s%s\n", comment, cmd);
					} else
						sbuf_printf(unexec_scripts, "%s%s\n",comment, cmd);

					/* workaround to detect the @dirrmtry */
					if (comment[0] == '#') {
						buf = cmd;

						/* remove the @dirrm{,try}
						 * command */
						while (!isspace(buf[0]))
							buf++;

						split_chr(buf, '|');

						if (strstr(buf, "\"/")) {
							while (regexec(&preg1, buf, 2, pmatch, 0) == 0) {
								strlcpy(path, &buf[pmatch[1].rm_so], pmatch[1].rm_eo - pmatch[1].rm_so + 1);
								buf+=pmatch[1].rm_eo;
								if (!strcmp(path, "/dev/null"))
									continue;
								ret += pkg_adddir_attr(pkg, path, uname, gname, perm, 1);
							}
						} else {
							while (regexec(&preg2, buf, 2, pmatch, 0) == 0) {
								strlcpy(path, &buf[pmatch[1].rm_so], pmatch[1].rm_eo - pmatch[1].rm_so + 1);
								buf+=pmatch[1].rm_eo;
								if (!strcmp(path, "/dev/null"))
									continue;
								ret += pkg_adddir_attr(pkg, path, uname, gname, perm, 1);
							}
						}

					}
				} else {
					exec_append(exec_scripts, "%s\n", cmd);
				}

				free(cmd);

			} else if (STARTS_WITH(plist_p, "@dirrm")) {
				/* dirrm or dirrmtry */

				buf = plist_p;

				/* remove the @dirrm or @dirrmtry */
				while (!isspace(buf[0]))
					buf++;

				while (isspace(buf[0]))
					buf++;

				len = strlen(buf);

				while (isspace(buf[len - 1])) {
					buf[len - 1] = '\0';
					len--;
				}

				if (buf[0] == '/')
					snprintf(path, sizeof(path), "%s/", buf);
				else
					snprintf(path, sizeof(path), "%s%s%s/", prefix, slash, buf);


				if (plist_p[6] == 't') {
					//post_unexec_append(post_unexec_scripts, "#@unexec /bin/rmdir \"%s\" || true\n", path);
					ret += pkg_adddir_attr(pkg, path, uname, gname, perm, 1);
				} else {
					//post_unexec_append(post_unexec_scripts, "#@dirrm \"%s\" || true\n", path);
					ret += pkg_adddir_attr(pkg, path, uname, gname, perm, 0);
				}


			} else if (STARTS_WITH(plist_p, "@mode")) {
				buf = plist_p;
				buf += 5;
				while (isspace(buf[0]))
					buf++;

				if (buf[0] == '\0') {
					perm = 0;
				} else {
					if ((set = setmode(buf)) == NULL) {
						pkg_emit_error("%s wrong @mode value", buf);
						perm = 0;
					} else {
						getmode(set, 0);
					}
				}
			} else if (STARTS_WITH(plist_p, "@owner")) {
				buf = plist_p;
				buf += 6;
				while (isspace(buf[0]))
					buf++;

				if (buf[0])
					uname = NULL;
				else
					uname = buf;
			} else if (STARTS_WITH(plist_p, "@group")) {
				buf = plist_p;
				buf += 6;
				while (isspace(buf[0]))
					buf++;

				if (buf[0])
					gname = NULL;
				else
					gname = buf;
			} else {
				pkg_emit_error("%s is deprecated, ignoring", plist_p);
			}
		} else if ((len = strlen(plist_p)) > 0){
			if (sbuf_len(unexec_scripts) > 0) {
				sbuf_finish(unexec_scripts);
				pre_unexec_append(pre_unexec_scripts, sbuf_data(unexec_scripts), "");
				sbuf_reset(unexec_scripts);
			}
			buf = plist_p;
			last_plist_file = buf;
			sha256[0] = '\0';

			/* remove spaces at the begining and at the end */
			while (isspace(buf[0]))
				buf++;

			while (isspace(buf[len -  1])) {
				buf[len - 1] = '\0';
				len--;
			}

			snprintf(path, sizeof(path), "%s%s%s", prefix, slash, buf);

			if (lstat(path, &st) == 0) {
				p = NULL;
				regular = true;

				if (S_ISLNK(st.st_mode)) {
					regular = false;
				}
				/* Special case for hardlinks */
				if (st.st_nlink > 1) {
					for (j = 0; j < hardlinks.len; j++) {
						if (hardlinks.inodes[j] == st.st_ino) {
							regular = false;
							break;
						}
					}
					/* This is the first time we see this hardlink */
					if (regular == true) {
						if (hardlinks.cap <= hardlinks.len) {
							hardlinks.cap += BUFSIZ;
							hardlinks.inodes = reallocf(hardlinks.inodes,
								hardlinks.cap * sizeof(ino_t));
						}
						hardlinks.inodes[hardlinks.len++] = st.st_ino;
					}
				}

				if (regular) {
					flatsize += st.st_size;
					sha256_file(path, sha256);
					p = sha256;
				}
				ret += pkg_addfile_attr(pkg, path, p, uname, gname, perm);
			} else {
				pkg_emit_errno("lstat", path);
			}
		}

		if (i != nbel) {
			plist_p += next + 1;
			next = strlen(plist_p);
		}
	}

	pkg_set(pkg, PKG_FLATSIZE, flatsize);

	if (sbuf_len(pre_unexec_scripts) > 0) {
		sbuf_finish(pre_unexec_scripts);
		pkg_appendscript(pkg, sbuf_data(pre_unexec_scripts), PKG_SCRIPT_PRE_DEINSTALL);
	}
	if (sbuf_len(exec_scripts) > 0) {
		sbuf_finish(exec_scripts);
		pkg_appendscript(pkg, sbuf_data(exec_scripts), PKG_SCRIPT_POST_INSTALL);
	}
	if (sbuf_len(unexec_scripts) > 0) {
		sbuf_finish(unexec_scripts);
		post_unexec_append(post_unexec_scripts, sbuf_data(unexec_scripts), "");
		sbuf_finish(post_unexec_scripts);
	}
	if (sbuf_len(post_unexec_scripts) > 0)
		pkg_appendscript(pkg, sbuf_data(post_unexec_scripts), PKG_SCRIPT_POST_DEINSTALL);

	regfree(&preg1);
	regfree(&preg2);
	sbuf_delete(pre_unexec_scripts);
	sbuf_delete(exec_scripts);
	sbuf_delete(unexec_scripts);
	free(hardlinks.inodes);

	free(plist_buf);

	return (ret);
}
