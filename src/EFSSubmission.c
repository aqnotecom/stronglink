#define _GNU_SOURCE
#include <fcntl.h>
#include "async.h"
#include "EarthFS.h"

struct EFSSubmission {
	EFSRepoRef repo;
	str_t *type;

	str_t *tmppath;
	uv_file tmpfile;
	int64_t size;
	EFSHasherRef hasher;
	EFSMetaFileRef meta;

	URIListRef URIs;
	str_t *internalHash;
};

EFSSubmissionRef EFSRepoCreateSubmission(EFSRepoRef const repo, strarg_t const type) {
	if(!repo) return NULL;
	assertf(type, "Submission requires type");

	EFSSubmissionRef sub = calloc(1, sizeof(struct EFSSubmission));
	if(!sub) return NULL;
	sub->repo = repo;
	sub->type = strdup(type);

	sub->tmppath = EFSRepoCopyTempPath(repo);
	if(async_fs_mkdirp_dirname(sub->tmppath, 0700) < 0) {
		fprintf(stderr, "Error: couldn't create temp dir %s\n", sub->tmppath);
		EFSSubmissionFree(&sub);
		return NULL;
	}

	sub->tmpfile = async_fs_open(sub->tmppath, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0400);
	if(sub->tmpfile < 0) {
		fprintf(stderr, "Error: couldn't create temp file %s\n", sub->tmppath);
		EFSSubmissionFree(&sub);
		return NULL;
	}

	sub->hasher = EFSHasherCreate(sub->type);
	sub->meta = EFSMetaFileCreate(sub->type);

	return sub;
}
void EFSSubmissionFree(EFSSubmissionRef *const subptr) {
	EFSSubmissionRef sub = *subptr;
	if(!sub) return;
	sub->repo = NULL;
	FREE(&sub->type);
	if(sub->tmppath) async_fs_unlink(sub->tmppath);
	FREE(&sub->tmppath);
	EFSHasherFree(&sub->hasher);
	EFSMetaFileFree(&sub->meta);
	URIListFree(&sub->URIs);
	FREE(&sub->internalHash);
	FREE(subptr); sub = NULL;
}
err_t EFSSubmissionWrite(EFSSubmissionRef const sub, byte_t const *const buf, size_t const len) {
	if(!sub) return 0;
	if(sub->tmpfile < 0) return -1;

	uv_buf_t info = uv_buf_init((char *)buf, len);
	ssize_t const result = async_fs_write(sub->tmpfile, &info, 1, sub->size);
	if(result < 0) {
		fprintf(stderr, "EFSSubmission write error %d\n", result);
		return -1;
	}

	sub->size += len;
	EFSHasherWrite(sub->hasher, buf, len);
	EFSMetaFileWrite(sub->meta, buf, len);
	return 0;
}
err_t EFSSubmissionEnd(EFSSubmissionRef const sub) {
	if(!sub) return 0;
	if(sub->tmpfile < 0) return -1;
	sub->URIs = EFSHasherEnd(sub->hasher);
	sub->internalHash = strdup(EFSHasherGetInternalHash(sub->hasher));
	EFSHasherFree(&sub->hasher);

	EFSMetaFileEnd(sub->meta);

	async_fs_close(sub->tmpfile);
	sub->tmpfile = -1;
	return 0;
}
err_t EFSSubmissionWriteFrom(EFSSubmissionRef const sub, ssize_t (*read)(void *, byte_t const **), void *const context) {
	if(!sub) return 0;
	assertf(read, "Read function required");
	for(;;) {
		byte_t const *buf = NULL;
		ssize_t const len = read(context, &buf);
		if(0 == len) break;
		if(len < 0) return -1;
		if(EFSSubmissionWrite(sub, buf, len) < 0) return -1;
	}
	if(EFSSubmissionEnd(sub) < 0) return -1;
	return 0;
}

strarg_t EFSSubmissionGetPrimaryURI(EFSSubmissionRef const sub) {
	if(!sub) return NULL;
	return URIListGetURI(sub->URIs, 0);
}

err_t EFSSessionAddSubmission(EFSSessionRef const session, EFSSubmissionRef const sub) {
	if(!session) return 0;
	if(!sub) return -1;
	if(!sub->tmppath) return -1;

	// TODO: Check session mode
	// TODO: Make sure session repo and submission repo match
	EFSRepoRef const repo = sub->repo;

	str_t *internalPath = EFSRepoCopyInternalPath(repo, sub->internalHash);
	if(async_fs_mkdirp_dirname(internalPath, 0700) < 0) {
		fprintf(stderr, "Couldn't mkdir -p %s\n", internalPath);
		FREE(&internalPath);
		return -1;
	}

	err_t const result = async_fs_link(sub->tmppath, internalPath);
	if(result < 0 && -EEXIST != result) {
		fprintf(stderr, "Couldn't move %s to %s\n", sub->tmppath, internalPath);
		FREE(&internalPath);
		return -1;
	}
	FREE(&internalPath);

	async_fs_unlink(sub->tmppath);
	FREE(&sub->tmppath);

	uint64_t const userID = EFSSessionGetUserID(session);
	sqlite3 *const db = EFSRepoDBConnect(repo);

	EXEC(QUERY(db, "BEGIN TRANSACTION"));

	sqlite3_stmt *insertFile = QUERY(db,
		"INSERT OR IGNORE INTO files (internal_hash, file_type, file_size)\n"
		"VALUES (?, ?, ?)");
	sqlite3_bind_text(insertFile, 1, sub->internalHash, -1, SQLITE_STATIC);
	sqlite3_bind_text(insertFile, 2, sub->type, -1, SQLITE_STATIC);
	sqlite3_bind_int64(insertFile, 3, sub->size);
	EXEC(insertFile); insertFile = NULL;

	// We can't use last_insert_rowid() if the file already existed.
	sqlite3_stmt *selectFile = QUERY(db,
		"SELECT file_id FROM files\n"
		"WHERE internal_hash = ? AND file_type = ?");
	sqlite3_bind_text(selectFile, 1, sub->internalHash, -1, SQLITE_STATIC);
	sqlite3_bind_text(selectFile, 2, sub->type, -1, SQLITE_STATIC);
	STEP(selectFile);
	int64_t const fileID = sqlite3_column_int64(selectFile, 0);
	sqlite3_finalize(selectFile); selectFile = NULL;

	sqlite3_stmt *insertURI = QUERY(db,
		"INSERT OR IGNORE INTO uris (uri) VALUES (?)");
	sqlite3_stmt *insertFileURI = QUERY(db,
		"INSERT OR IGNORE INTO file_uris (file_id, uri_id)\n"
		"SELECT ?, uri_id FROM uris WHERE uri = ? LIMIT 1");
	for(index_t i = 0; i < URIListGetCount(sub->URIs); ++i) {
		strarg_t const URI = URIListGetURI(sub->URIs, i);
		sqlite3_bind_text(insertURI, 1, URI, -1, SQLITE_STATIC);
		STEP(insertURI); sqlite3_reset(insertURI);

		sqlite3_bind_int64(insertFileURI, 1, fileID);
		sqlite3_bind_text(insertFileURI, 2, URI, -1, SQLITE_STATIC);
		STEP(insertFileURI); sqlite3_reset(insertFileURI);
	}
	sqlite3_finalize(insertURI); insertURI = NULL;
	sqlite3_finalize(insertFileURI); insertFileURI = NULL;


	// TODO: Add permissions for other specified users too.
	sqlite3_stmt *insertFilePermission = QUERY(db,
		"INSERT OR IGNORE INTO file_permissions\n"
		"	(file_id, user_id, meta_file_id)\n"
		"VALUES (?, ?, ?)");
	sqlite3_bind_int64(insertFilePermission, 1, fileID);
	sqlite3_bind_int64(insertFilePermission, 2, userID);
	sqlite3_bind_int64(insertFilePermission, 3, fileID);
	EXEC(insertFilePermission); insertFilePermission = NULL;

	strarg_t const preferredURI = URIListGetURI(sub->URIs, 0);
	if(EFSMetaFileStore(sub->meta, fileID, preferredURI, db) < 0) {
		fprintf(stderr, "EFSMetaFileStore error\n");
		EXEC(QUERY(db, "ROLLBACK"));
		EFSRepoDBClose(repo, db);
		return -1;
	}

	EXEC(QUERY(db, "COMMIT"));
	EFSRepoDBClose(repo, db);

	return 0;
}

