/*
 *  be_sync.c
 *
 *  Copyright (c) 2006-2011 Pacman Development Team <pacman-dev@archlinux.org>
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <limits.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* libalpm */
#include "util.h"
#include "log.h"
#include "alpm.h"
#include "alpm_list.h"
#include "package.h"
#include "handle.h"
#include "delta.h"
#include "deps.h"
#include "dload.h"

/** Update a package database
 *
 * An update of the package database \a db will be attempted. Unless
 * \a force is true, the update will only be performed if the remote
 * database was modified since the last update.
 *
 * A transaction is necessary for this operation, in order to obtain a
 * database lock. During this transaction the front-end will be informed
 * of the download progress of the database via the download callback.
 *
 * Example:
 * @code
 * pmdb_t *db;
 * int result;
 * db = alpm_list_getdata(alpm_option_get_syncdbs());
 * if(alpm_trans_init(0, NULL, NULL, NULL) == 0) {
 *     result = alpm_db_update(0, db);
 *     alpm_trans_release();
 *
 *     if(result > 0) {
 *	       printf("Unable to update database: %s\n", alpm_strerrorlast());
 *     } else if(result < 0) {
 *         printf("Database already up to date\n");
 *     } else {
 *         printf("Database updated\n");
 *     }
 * }
 * @endcode
 *
 * @ingroup alpm_databases
 * @note After a successful update, the \link alpm_db_get_pkgcache()
 * package cache \endlink will be invalidated
 * @param force if true, then forces the update, otherwise update only in case
 * the database isn't up to date
 * @param db pointer to the package database to update
 * @return 0 on success, > 0 on error (pm_errno is set accordingly), < 0 if up
 * to date
 */
int SYMEXPORT alpm_db_update(int force, pmdb_t *db)
{
	char *dbfile, *syncpath;
	const char *dbpath;
	struct stat buf;
	size_t len;
	int ret;

	ALPM_LOG_FUNC;

	/* Sanity checks */
	ASSERT(handle != NULL, RET_ERR(PM_ERR_HANDLE_NULL, -1));
	ASSERT(db != NULL && db != handle->db_local, RET_ERR(PM_ERR_WRONG_ARGS, -1));

	if(!alpm_list_find_ptr(handle->dbs_sync, db)) {
		RET_ERR(PM_ERR_DB_NOT_FOUND, -1);
	}

	len = strlen(db->treename) + 4;
	MALLOC(dbfile, len, RET_ERR(PM_ERR_MEMORY, -1));
	sprintf(dbfile, "%s.db", db->treename);

	dbpath = alpm_option_get_dbpath();
	len = strlen(dbpath) + 6;
	MALLOC(syncpath, len, RET_ERR(PM_ERR_MEMORY, -1));
	sprintf(syncpath, "%s%s", dbpath, "sync/");

	if(stat(syncpath, &buf) != 0) {
		_alpm_log(PM_LOG_DEBUG, "database dir '%s' does not exist, creating it\n",
				syncpath);
		if(_alpm_makepath(syncpath) != 0) {
			free(dbfile);
			free(syncpath);
			RET_ERR(PM_ERR_SYSTEM, -1);
		}
	} else if(!S_ISDIR(buf.st_mode)) {
		_alpm_log(PM_LOG_WARNING, _("removing invalid file: %s\n"), syncpath);
		if(unlink(syncpath) != 0 || _alpm_makepath(syncpath) != 0) {
			free(dbfile);
			free(syncpath);
			RET_ERR(PM_ERR_SYSTEM, -1);
		}
	}

	ret = _alpm_download_single_file(dbfile, db->servers, syncpath, force);
	free(dbfile);
	free(syncpath);

	if(ret == 1) {
		/* files match, do nothing */
		pm_errno = 0;
		return(1);
	} else if(ret == -1) {
		/* pm_errno was set by the download code */
		_alpm_log(PM_LOG_DEBUG, "failed to sync db: %s\n", alpm_strerrorlast());
		return(-1);
	}

	/* Cache needs to be rebuilt */
	_alpm_db_free_pkgcache(db);

	return(0);
}

/* Forward decl so I don't reorganize the whole file right now */
static int sync_db_read(pmdb_t *db, struct archive *archive,
		struct archive_entry *entry, pmpkg_t *likely_pkg);

static int sync_db_populate(pmdb_t *db)
{
	int count = 0;
	struct archive *archive;
	struct archive_entry *entry;
	pmpkg_t *pkg = NULL;

	ALPM_LOG_FUNC;

	ASSERT(db != NULL, RET_ERR(PM_ERR_DB_NULL, -1));

	if((archive = archive_read_new()) == NULL)
		RET_ERR(PM_ERR_LIBARCHIVE, 1);

	archive_read_support_compression_all(archive);
	archive_read_support_format_all(archive);

	if(archive_read_open_filename(archive, _alpm_db_path(db),
				ARCHIVE_DEFAULT_BYTES_PER_BLOCK) != ARCHIVE_OK) {
		_alpm_log(PM_LOG_ERROR, _("could not open %s: %s\n"), _alpm_db_path(db),
				archive_error_string(archive));
		archive_read_finish(archive);
		RET_ERR(PM_ERR_DB_OPEN, 1);
	}

	while(archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
		const struct stat *st;

		st = archive_entry_stat(entry);

		if(S_ISDIR(st->st_mode)) {
			const char *name;

			pkg = _alpm_pkg_new();
			if(pkg == NULL) {
				archive_read_finish(archive);
				RET_ERR(PM_ERR_MEMORY, -1);
			}

			name = archive_entry_pathname(entry);

			if(_alpm_splitname(name, pkg) != 0) {
				_alpm_log(PM_LOG_ERROR, _("invalid name for database entry '%s'\n"),
						name);
				_alpm_pkg_free(pkg);
				continue;
			}

			/* duplicated database entries are not allowed */
			if(_alpm_pkg_find(db->pkgcache, pkg->name)) {
				_alpm_log(PM_LOG_ERROR, _("duplicated database entry '%s'\n"), pkg->name);
				_alpm_pkg_free(pkg);
				continue;
			}

			pkg->origin = PKG_FROM_SYNCDB;
			pkg->ops = &default_pkg_ops;
			pkg->origin_data.db = db;

			/* add to the collection */
			_alpm_log(PM_LOG_FUNCTION, "adding '%s' to package cache for db '%s'\n",
					pkg->name, db->treename);
			db->pkgcache = alpm_list_add(db->pkgcache, pkg);
			count++;
		} else {
			/* we have desc, depends or deltas - parse it */
			sync_db_read(db, archive, entry, pkg);
		}
	}

	db->pkgcache = alpm_list_msort(db->pkgcache, (size_t)count, _alpm_pkg_cmp);
	archive_read_finish(archive);

	return(count);
}

#define READ_NEXT(s) do { \
	if(_alpm_archive_fgets(archive, &buf) != ARCHIVE_OK) goto error; \
	s = _alpm_strtrim(buf.line); \
} while(0)

#define READ_AND_STORE(f) do { \
	READ_NEXT(line); \
	STRDUP(f, line, goto error); \
} while(0)

#define READ_AND_STORE_ALL(f) do { \
	char *linedup; \
	READ_NEXT(line); \
	if(strlen(line) == 0) break; \
	STRDUP(linedup, line, goto error); \
	f = alpm_list_add(f, linedup); \
} while(1) /* note the while(1) and not (0) */

static int sync_db_read(pmdb_t *db, struct archive *archive,
		struct archive_entry *entry, pmpkg_t *likely_pkg)
{
	const char *entryname = NULL, *filename;
	char *pkgname, *p, *q;
	pmpkg_t *pkg;
	struct archive_read_buffer buf;

	ALPM_LOG_FUNC;

	if(db == NULL) {
		RET_ERR(PM_ERR_DB_NULL, -1);
	}

	if(entry != NULL) {
		entryname = archive_entry_pathname(entry);
	}
	if(entryname == NULL) {
		_alpm_log(PM_LOG_DEBUG, "invalid archive entry provided to _alpm_sync_db_read, skipping\n");
		return(-1);
	}

	_alpm_log(PM_LOG_FUNCTION, "loading package data from archive entry %s\n",
			entryname);

	memset(&buf, 0, sizeof(buf));
	/* 512K for a line length seems reasonable */
	buf.max_line_size = 512 * 1024;

	/* get package and db file names */
	STRDUP(pkgname, entryname, RET_ERR(PM_ERR_MEMORY, -1));
	p = pkgname + strlen(pkgname);
	for(q = --p; *q && *q != '/'; q--);
	filename = q + 1;
	for(p = --q; *p && *p != '-'; p--);
	for(q = --p; *q && *q != '-'; q--);
	*q = '\0';

	/* package is already in db due to parsing of directory name */
	if(likely_pkg && strcmp(likely_pkg->name, pkgname) == 0) {
		pkg = likely_pkg;
	} else {
		pkg = _alpm_pkg_find(db->pkgcache, pkgname);
	}
	if(pkg == NULL) {
		_alpm_log(PM_LOG_DEBUG, "package %s not found in %s sync database",
					pkgname, db->treename);
		return(-1);
	}

	if(strcmp(filename, "desc") == 0 || strcmp(filename, "depends") == 0
			|| strcmp(filename, "deltas") == 0) {
		while(_alpm_archive_fgets(archive, &buf) == ARCHIVE_OK) {
			char *line = _alpm_strtrim(buf.line);

			if(strcmp(line, "%NAME%") == 0) {
				READ_NEXT(line);
				if(strcmp(line, pkg->name) != 0) {
					_alpm_log(PM_LOG_ERROR, _("%s database is inconsistent: name "
								"mismatch on package %s\n"), db->treename, pkg->name);
				}
			} else if(strcmp(line, "%VERSION%") == 0) {
				READ_NEXT(line);
				if(strcmp(line, pkg->version) != 0) {
					_alpm_log(PM_LOG_ERROR, _("%s database is inconsistent: version "
								"mismatch on package %s\n"), db->treename, pkg->name);
				}
			} else if(strcmp(line, "%FILENAME%") == 0) {
				READ_AND_STORE(pkg->filename);
			} else if(strcmp(line, "%DESC%") == 0) {
				READ_AND_STORE(pkg->desc);
			} else if(strcmp(line, "%GROUPS%") == 0) {
				READ_AND_STORE_ALL(pkg->groups);
			} else if(strcmp(line, "%URL%") == 0) {
				READ_AND_STORE(pkg->url);
			} else if(strcmp(line, "%LICENSE%") == 0) {
				READ_AND_STORE_ALL(pkg->licenses);
			} else if(strcmp(line, "%ARCH%") == 0) {
				READ_AND_STORE(pkg->arch);
			} else if(strcmp(line, "%BUILDDATE%") == 0) {
				READ_NEXT(line);
				pkg->builddate = _alpm_parsedate(line);
			} else if(strcmp(line, "%PACKAGER%") == 0) {
				READ_AND_STORE(pkg->packager);
			} else if(strcmp(line, "%CSIZE%") == 0) {
				/* Note: the CSIZE and SIZE fields both share the "size" field in the
				 * pkginfo_t struct. This can be done b/c CSIZE is currently only used
				 * in sync databases, and SIZE is only used in local databases.
				 */
				READ_NEXT(line);
				pkg->size = atol(line);
				/* also store this value to isize if isize is unset */
				if(pkg->isize == 0) {
					pkg->isize = pkg->size;
				}
			} else if(strcmp(line, "%ISIZE%") == 0) {
				READ_NEXT(line);
				pkg->isize = atol(line);
			} else if(strcmp(line, "%MD5SUM%") == 0) {
				READ_AND_STORE(pkg->md5sum);
			} else if(strcmp(line, "%REPLACES%") == 0) {
				READ_AND_STORE_ALL(pkg->replaces);
			} else if(strcmp(line, "%DEPENDS%") == 0) {
				/* Different than the rest because of the _alpm_splitdep call. */
				while(1) {
					READ_NEXT(line);
					if(strlen(line) == 0) break;
					pkg->depends = alpm_list_add(pkg->depends, _alpm_splitdep(line));
				}
			} else if(strcmp(line, "%OPTDEPENDS%") == 0) {
				READ_AND_STORE_ALL(pkg->optdepends);
			} else if(strcmp(line, "%CONFLICTS%") == 0) {
				READ_AND_STORE_ALL(pkg->conflicts);
			} else if(strcmp(line, "%PROVIDES%") == 0) {
				READ_AND_STORE_ALL(pkg->provides);
			} else if(strcmp(line, "%DELTAS%") == 0) {
				READ_AND_STORE_ALL(pkg->deltas);
			}
		}
	} else {
		 /* unknown database file */
		_alpm_log(PM_LOG_DEBUG, "unknown database file: %s", filename);
	}

error:
	FREE(pkgname);
	/* TODO: return 0 always? */
	return(0);
}

struct db_operations sync_db_ops = {
	.populate         = sync_db_populate,
	.unregister       = _alpm_db_unregister,
};

pmdb_t *_alpm_db_register_sync(const char *treename)
{
	pmdb_t *db;
	alpm_list_t *i;

	ALPM_LOG_FUNC;

	for(i = handle->dbs_sync; i; i = i->next) {
		pmdb_t *sdb = i->data;
		if(strcmp(treename, sdb->treename) == 0) {
			_alpm_log(PM_LOG_DEBUG, "attempt to re-register the '%s' database, using existing\n", sdb->treename);
			return sdb;
		}
	}

	_alpm_log(PM_LOG_DEBUG, "registering sync database '%s'\n", treename);

	db = _alpm_db_new(treename, 0);
	db->ops = &sync_db_ops;
	if(db == NULL) {
		RET_ERR(PM_ERR_DB_CREATE, NULL);
	}

	handle->dbs_sync = alpm_list_add(handle->dbs_sync, db);
	return(db);
}


/* vim: set ts=2 sw=2 noet: */