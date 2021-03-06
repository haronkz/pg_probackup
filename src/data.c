/*-------------------------------------------------------------------------
 *
 * data.c: utils to parse and backup data pages
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include <common/pg_lzcompress.h>
#include "utils/file.h"

#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "utils/thread.h"

/* Union to ease operations on relation pages */
typedef union DataPage
{
	PageHeaderData page_data;
	char		data[BLCKSZ];
} DataPage;

#ifdef HAVE_LIBZ
/* Implementation of zlib compression method */
static int32
zlib_compress(void *dst, size_t dst_size, void const *src, size_t src_size,
			  int level)
{
	uLongf		compressed_size = dst_size;
	int			rc = compress2(dst, &compressed_size, src, src_size,
							   level);

	return rc == Z_OK ? compressed_size : rc;
}

/* Implementation of zlib compression method */
static int32
zlib_decompress(void *dst, size_t dst_size, void const *src, size_t src_size)
{
	uLongf		dest_len = dst_size;
	int			rc = uncompress(dst, &dest_len, src, src_size);

	return rc == Z_OK ? dest_len : rc;
}
#endif

/*
 * Compresses source into dest using algorithm. Returns the number of bytes
 * written in the destination buffer, or -1 if compression fails.
 */
int32
do_compress(void* dst, size_t dst_size, void const* src, size_t src_size,
			CompressAlg alg, int level, const char **errormsg)
{
	switch (alg)
	{
		case NONE_COMPRESS:
		case NOT_DEFINED_COMPRESS:
			return -1;
#ifdef HAVE_LIBZ
		case ZLIB_COMPRESS:
			{
				int32		ret;
				ret = zlib_compress(dst, dst_size, src, src_size, level);
				if (ret < Z_OK && errormsg)
					*errormsg = zError(ret);
				return ret;
			}
#endif
		case PGLZ_COMPRESS:
			return pglz_compress(src, src_size, dst, PGLZ_strategy_always);
	}

	return -1;
}

/*
 * Decompresses source into dest using algorithm. Returns the number of bytes
 * decompressed in the destination buffer, or -1 if decompression fails.
 */
int32
do_decompress(void* dst, size_t dst_size, void const* src, size_t src_size,
			  CompressAlg alg, const char **errormsg)
{
	switch (alg)
	{
		case NONE_COMPRESS:
		case NOT_DEFINED_COMPRESS:
		    if (errormsg)
				*errormsg = "Invalid compression algorithm";
			return -1;
#ifdef HAVE_LIBZ
		case ZLIB_COMPRESS:
			{
				int32		ret;
				ret = zlib_decompress(dst, dst_size, src, src_size);
				if (ret < Z_OK && errormsg)
					*errormsg = zError(ret);
				return ret;
			}
#endif
		case PGLZ_COMPRESS:

#if PG_VERSION_NUM >= 120000
			return pglz_decompress(src, src_size, dst, dst_size, true);
#else
			return pglz_decompress(src, src_size, dst, dst_size);
#endif
	}

	return -1;
}


#define ZLIB_MAGIC 0x78

/*
 * Before version 2.0.23 there was a bug in pro_backup that pages which compressed
 * size is exactly the same as original size are not treated as compressed.
 * This check tries to detect and decompress such pages.
 * There is no 100% criteria to determine whether page is compressed or not.
 * But at least we will do this check only for pages which will no pass validation step.
 */
static bool
page_may_be_compressed(Page page, CompressAlg alg, uint32 backup_version)
{
	PageHeader	phdr;

	phdr = (PageHeader) page;

	/* First check if page header is valid (it seems to be fast enough check) */
	if (!(PageGetPageSize(phdr) == BLCKSZ &&
	//	  PageGetPageLayoutVersion(phdr) == PG_PAGE_LAYOUT_VERSION &&
		  (phdr->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
		  phdr->pd_lower >= SizeOfPageHeaderData &&
		  phdr->pd_lower <= phdr->pd_upper &&
		  phdr->pd_upper <= phdr->pd_special &&
		  phdr->pd_special <= BLCKSZ &&
		  phdr->pd_special == MAXALIGN(phdr->pd_special)))
	{
		/* ... end only if it is invalid, then do more checks */
		if (backup_version >= 20023)
		{
			/* Versions 2.0.23 and higher don't have such bug */
			return false;
		}
#ifdef HAVE_LIBZ
		/* For zlib we can check page magic:
		 * https://stackoverflow.com/questions/9050260/what-does-a-zlib-header-look-like
		 */
		if (alg == ZLIB_COMPRESS && *(char*)page != ZLIB_MAGIC)
		{
			return false;
		}
#endif
		/* otherwise let's try to decompress the page */
		return true;
	}
	return false;
}

/* Verify page's header */
bool
parse_page(Page page, XLogRecPtr *lsn)
{
	PageHeader	phdr = (PageHeader) page;

	/* Get lsn from page header */
	*lsn = PageXLogRecPtrGet(phdr->pd_lsn);

	if (PageGetPageSize(phdr) == BLCKSZ &&
	//	PageGetPageLayoutVersion(phdr) == PG_PAGE_LAYOUT_VERSION &&
		(phdr->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
		phdr->pd_lower >= SizeOfPageHeaderData &&
		phdr->pd_lower <= phdr->pd_upper &&
		phdr->pd_upper <= phdr->pd_special &&
		phdr->pd_special <= BLCKSZ &&
		phdr->pd_special == MAXALIGN(phdr->pd_special))
		return true;

	return false;
}

/* We know that header is invalid, store specific
 * details in errormsg.
 */
void
get_header_errormsg(Page page, char **errormsg)
{
	PageHeader  phdr = (PageHeader) page;
	*errormsg = pgut_malloc(MAXPGPATH);

	if (PageGetPageSize(phdr) != BLCKSZ)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"page size %lu is not equal to block size %u",
				PageGetPageSize(phdr), BLCKSZ);

	else if (phdr->pd_lower < SizeOfPageHeaderData)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_lower %i is less than page header size %lu",
				phdr->pd_lower, SizeOfPageHeaderData);

	else if (phdr->pd_lower > phdr->pd_upper)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_lower %u is greater than pd_upper %u",
				phdr->pd_lower, phdr->pd_upper);

	else if (phdr->pd_upper > phdr->pd_special)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_upper %u is greater than pd_special %u",
				phdr->pd_upper, phdr->pd_special);

	else if (phdr->pd_special > BLCKSZ)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_special %u is greater than block size %u",
				phdr->pd_special, BLCKSZ);

	else if (phdr->pd_special != MAXALIGN(phdr->pd_special))
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_special %i is misaligned, expected %lu",
				phdr->pd_special, MAXALIGN(phdr->pd_special));

	else if (phdr->pd_flags & ~PD_VALID_FLAG_BITS)
		snprintf(*errormsg, MAXPGPATH, "page header invalid, "
				"pd_flags mask contain illegal bits");

	else
		snprintf(*errormsg, MAXPGPATH, "page header invalid");
}

/* We know that checksumms are mismatched, store specific
 * details in errormsg.
 */
void
get_checksum_errormsg(Page page, char **errormsg, BlockNumber absolute_blkno)
{
	PageHeader	phdr = (PageHeader) page;
	*errormsg = pgut_malloc(MAXPGPATH);

	snprintf(*errormsg, MAXPGPATH,
			 "page verification failed, "
			 "calculated checksum %u but expected %u",
			 phdr->pd_checksum,
			 pg_checksum_page(page, absolute_blkno));
}

/*
 * Retrieves a page taking the backup mode into account
 * and writes it into argument "page". Argument "page"
 * should be a pointer to allocated BLCKSZ of bytes.
 *
 * Prints appropriate warnings/errors/etc into log.
 * Returns:
 *                 PageIsOk(0) if page was successfully retrieved
 *         PageIsTruncated(-1) if the page was truncated
 *         SkipCurrentPage(-2) if we need to skip this page,
 *                                only used for DELTA backup
 *         PageIsCorrupted(-3) if the page checksum mismatch
 *                                or header corruption,
 *                                only used for checkdb
 *                                TODO: probably we should always
 *                                      return it to the caller
 */
static int32
prepare_page(ConnectionArgs *conn_arg,
			 pgFile *file, XLogRecPtr prev_backup_start_lsn,
			 BlockNumber blknum, FILE *in,
			 BackupMode backup_mode,
			 Page page, bool strict,
			 uint32 checksum_version,
			 int ptrack_version_num,
			 const char *ptrack_schema,
			 const char *from_fullpath)
{
	XLogRecPtr	page_lsn = 0;
	int			try_again = PAGE_READ_ATTEMPTS;
	bool		page_is_valid = false;
	BlockNumber absolute_blknum = file->segno * RELSEG_SIZE + blknum;

	/* check for interrupt */
	if (interrupted || thread_interrupted)
		elog(ERROR, "Interrupted during page reading");

	/*
	 * Read the page and verify its header and checksum.
	 * Under high write load it's possible that we've read partly
	 * flushed page, so try several times before throwing an error.
	 */
	if (backup_mode != BACKUP_MODE_DIFF_PTRACK || ptrack_version_num >= 20)
	{
		int rc = 0;
		while (!page_is_valid && try_again--)
		{
			/* read the block */
			int read_len = fio_pread(in, page, blknum * BLCKSZ);
			page_lsn = 0;

			/* The block could have been truncated. It is fine. */
			if (read_len == 0)
			{
				elog(VERBOSE, "Cannot read block %u of \"%s\": "
							"block truncated", blknum, from_fullpath);
				return PageIsTruncated;
			}
			else if (read_len < 0)
				elog(ERROR, "Cannot read block %u of \"%s\": %s",
						blknum, from_fullpath, strerror(errno));
			else if (read_len != BLCKSZ)
				elog(WARNING, "Cannot read block %u of \"%s\": "
								"read %i of %d, try again",
						blknum, from_fullpath, read_len, BLCKSZ);
			else
			{
				/* We have BLCKSZ of raw data, validate it */
				rc = validate_one_page(page, absolute_blknum,
										   InvalidXLogRecPtr, &page_lsn,
										   checksum_version);
				switch (rc)
				{
					case PAGE_IS_ZEROED:
						elog(VERBOSE, "File: \"%s\" blknum %u, empty page", from_fullpath, blknum);
						return PageIsOk;

					case PAGE_IS_VALID:
						/* in DELTA mode we must compare lsn */
						if (backup_mode == BACKUP_MODE_DIFF_DELTA)
							page_is_valid = true;
						else
							return PageIsOk;
						break;

					case PAGE_HEADER_IS_INVALID:
						elog(VERBOSE, "File: \"%s\" blknum %u have wrong page header, try again",
								from_fullpath, blknum);
						break;

					case PAGE_CHECKSUM_MISMATCH:
						elog(VERBOSE, "File: \"%s\" blknum %u have wrong checksum, try again",
								from_fullpath, blknum);
						break;
					default:
						Assert(false);
				}
			}
		}

		/*
		 * If page is not valid after 100 attempts to read it
		 * throw an error.
		 */
		if (!page_is_valid)
		{
			int elevel = ERROR;
			char *errormsg = NULL;

			/* Get the details of corruption */
			if (rc == PAGE_HEADER_IS_INVALID)
				get_header_errormsg(page, &errormsg);
			else if (rc == PAGE_CHECKSUM_MISMATCH)
				get_checksum_errormsg(page, &errormsg,
									  file->segno * RELSEG_SIZE + blknum);

			/* Error out in case of merge or backup without ptrack support;
			 * issue warning in case of checkdb or backup with ptrack support
			 */
			if (!strict)
				elevel = WARNING;

			if (errormsg)
				elog(elevel, "Corruption detected in file \"%s\", block %u: %s",
								from_fullpath, blknum, errormsg);
			else
				elog(elevel, "Corruption detected in file \"%s\", block %u",
								from_fullpath, blknum);

			pg_free(errormsg);
			return PageIsCorrupted;
		}

		/* Checkdb not going futher */
		if (!strict)
			return PageIsOk;
	}

	/*
	 * Get page via ptrack interface from PostgreSQL shared buffer.
	 * We do this only in the cases of PTRACK 1.x versions backup
	 */
	if (backup_mode == BACKUP_MODE_DIFF_PTRACK
		&& (ptrack_version_num >= 15 && ptrack_version_num < 20))
	{
		int rc = 0;
		size_t page_size = 0;
		Page ptrack_page = NULL;
		ptrack_page = (Page) pg_ptrack_get_block(conn_arg, file->dbOid, file->tblspcOid,
										  file->relOid, absolute_blknum, &page_size,
										  ptrack_version_num, ptrack_schema);

		if (ptrack_page == NULL)
			/* This block was truncated.*/
			return PageIsTruncated;

		if (page_size != BLCKSZ)
			elog(ERROR, "File: \"%s\", block %u, expected block size %d, but read %zu",
					   from_fullpath, blknum, BLCKSZ, page_size);

		/*
		 * We need to copy the page that was successfully
		 * retrieved from ptrack into our output "page" parameter.
		 */
		memcpy(page, ptrack_page, BLCKSZ);
		pg_free(ptrack_page);

		/*
		 * UPD: It apprears that is possible to get zeroed page or page with invalid header
		 * from shared buffer.
		 * Note, that getting page with wrong checksumm from shared buffer is
		 * acceptable.
		 */
		rc = validate_one_page(page, absolute_blknum,
								InvalidXLogRecPtr, &page_lsn,
								checksum_version);

		/* It is ok to get zeroed page */
		if (rc == PAGE_IS_ZEROED)
			return PageIsOk;

		/* Getting page with invalid header from shared buffers is unacceptable */
		if (rc == PAGE_HEADER_IS_INVALID)
		{
			char *errormsg = NULL;
			get_header_errormsg(page, &errormsg);
			elog(ERROR, "Corruption detected in file \"%s\", block %u: %s",
								from_fullpath, blknum, errormsg);
		}

		/*
		 * We must set checksum here, because it is outdated
		 * in the block recieved from shared buffers.
		 */
		if (checksum_version)
			((PageHeader) page)->pd_checksum = pg_checksum_page(page, absolute_blknum);
	}

	/*
	 * Skip page if page lsn is less than START_LSN of parent backup.
	 * Nullified pages must be copied by DELTA backup, just to be safe.
	 */
	if (backup_mode == BACKUP_MODE_DIFF_DELTA &&
		file->exists_in_prev &&
		page_lsn &&
		page_lsn < prev_backup_start_lsn)
	{
		elog(VERBOSE, "Skipping blknum %u in file: \"%s\"", blknum, from_fullpath);
		return SkipCurrentPage;
	}

	return PageIsOk;
}

static void
compress_and_backup_page(pgFile *file, BlockNumber blknum,
						FILE *in, FILE *out, pg_crc32 *crc,
						int page_state, Page page,
						CompressAlg calg, int clevel,
						const char *from_fullpath, const char *to_fullpath)
{
	BackupPageHeader header;
	size_t		write_buffer_size = sizeof(header);
	char		write_buffer[BLCKSZ+sizeof(header)];
	char		compressed_page[BLCKSZ*2]; /* compressed page may require more space than uncompressed */
	const char *errormsg = NULL;

	header.block = blknum;

	/* Compress the page */
	header.compressed_size = do_compress(compressed_page, sizeof(compressed_page),
										 page, BLCKSZ, calg, clevel,
										 &errormsg);
	/* Something went wrong and errormsg was assigned, throw a warning */
	if (header.compressed_size < 0 && errormsg != NULL)
		elog(WARNING, "An error occured during compressing block %u of file \"%s\": %s",
			 blknum, from_fullpath, errormsg);

	file->compress_alg = calg; /* TODO: wtf? why here? */

	/* The page was successfully compressed. */
	if (header.compressed_size > 0 && header.compressed_size < BLCKSZ)
	{
		memcpy(write_buffer, &header, sizeof(header));
		memcpy(write_buffer + sizeof(header),
			   compressed_page, header.compressed_size);
		write_buffer_size += MAXALIGN(header.compressed_size);
	}
	/* Non-positive value means that compression failed. Write it as is. */
	else
	{
		header.compressed_size = BLCKSZ;
		memcpy(write_buffer, &header, sizeof(header));
		memcpy(write_buffer + sizeof(header), page, BLCKSZ);
		write_buffer_size += header.compressed_size;
	}

	/* Update CRC */
	COMP_FILE_CRC32(true, *crc, write_buffer, write_buffer_size);

	/* write data page */
	if (fio_fwrite(out, write_buffer, write_buffer_size) != write_buffer_size)
		elog(ERROR, "File: \"%s\", cannot write at block %u: %s",
			 to_fullpath, blknum, strerror(errno));

	file->write_size += write_buffer_size;
	file->uncompressed_size += BLCKSZ;
}

/*
 * Backup data file in the from_root directory to the to_root directory with
 * same relative path. If prev_backup_start_lsn is not NULL, only pages with
 * higher lsn will be copied.
 * Not just copy file, but read it block by block (use bitmap in case of
 * incremental backup), validate checksum, optionally compress and write to
 * backup with special header.
 */
void
backup_data_file(ConnectionArgs* conn_arg, pgFile *file,
				 const char *from_fullpath, const char *to_fullpath,
				 XLogRecPtr prev_backup_start_lsn, BackupMode backup_mode,
				 CompressAlg calg, int clevel, uint32 checksum_version,
				 int ptrack_version_num, const char *ptrack_schema, bool missing_ok)
{
	FILE       *in;
	FILE       *out;
	BlockNumber blknum = 0;
	BlockNumber nblocks = 0;		/* number of blocks in source file */
	BlockNumber n_blocks_skipped = 0;
	int         page_state;
	char        curr_page[BLCKSZ];
	bool        use_pagemap;
	datapagemap_iterator_t *iter = NULL;

	/* stdio buffers */
	char *in_buf = NULL;
	char *out_buf = NULL;

	/* sanity */
	if (file->size % BLCKSZ != 0)
		elog(WARNING, "File: \"%s\", invalid file size %zu", from_fullpath, file->size);

	/*
	 * Compute expected number of blocks in the file.
	 * NOTE This is a normal situation, if the file size has changed
	 * since the moment we computed it.
	 */
	nblocks = file->size/BLCKSZ;

	/* set n_blocks for a file */
	file->n_blocks = nblocks;

	/*
	 * Skip unchanged file only if it exists in previous backup.
	 * This way we can correctly handle null-sized files which are
	 * not tracked by pagemap and thus always marked as unchanged.
	 */
	if ((backup_mode == BACKUP_MODE_DIFF_PAGE ||
		backup_mode == BACKUP_MODE_DIFF_PTRACK) &&
		file->pagemap.bitmapsize == PageBitmapIsEmpty &&
		file->exists_in_prev && !file->pagemap_isabsent)
	{
		/*
		 * There are no changed blocks since last backup. We want to make
		 * incremental backup, so we should exit.
		 */
		file->write_size = BYTES_INVALID;
		return;
	}

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;
	file->uncompressed_size = 0;
	INIT_FILE_CRC32(true, file->crc);

	/* open source file for read */
	in = fio_fopen(from_fullpath, PG_BINARY_R, FIO_DB_HOST);
	if (in == NULL)
	{
		FIN_FILE_CRC32(true, file->crc);

		/*
		 * If file is not found, this is not en error.
		 * It could have been deleted by concurrent postgres transaction.
		 */
		if (errno == ENOENT)
		{
			if (missing_ok)
			{
				elog(LOG, "File \"%s\" is not found", from_fullpath);
				file->write_size = FILE_NOT_FOUND;
				return;
			}
			else
				elog(ERROR, "File \"%s\" is not found", from_fullpath);
		}

		/* In all other cases throw an error */
		elog(ERROR, "Cannot open file \"%s\": %s",
			 from_fullpath, strerror(errno));
	}

	/* open backup file for write  */
	out = fopen(to_fullpath, PG_BINARY_W);
	if (out == NULL)
		elog(ERROR, "Cannot open backup file \"%s\": %s",
			 to_fullpath, strerror(errno));

	/* update file permission */
	if (chmod(to_fullpath, FILE_PERMISSION) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", to_fullpath,
			 strerror(errno));

	/*
	 * Read each page, verify checksum and write it to backup.
	 * If page map is empty or file is not present in previous backup
	 * backup all pages of the relation.
	 *
	 * In PTRACK 1.x there was a problem
	 * of data files with missing _ptrack map.
	 * Such files should be fully copied.
	 */

	if 	(file->pagemap.bitmapsize == PageBitmapIsEmpty ||
		 file->pagemap_isabsent || !file->exists_in_prev ||
		 !file->pagemap.bitmap)
		use_pagemap = false;
	else
		use_pagemap = true;

	if (!fio_is_remote_file(in))
	{
		/* enable stdio buffering for local input file,
		 * unless the pagemap is involved, which
		 * imply a lot of random access.
		 */
		if (use_pagemap)
			setvbuf(in, NULL, _IONBF, BUFSIZ);
		else
		{
			in_buf = pgut_malloc(STDIO_BUFSIZE);
			setvbuf(in, in_buf, _IOFBF, STDIO_BUFSIZE);
		}
	}

	/* enable stdio buffering for output file */
	out_buf = pgut_malloc(STDIO_BUFSIZE);
	setvbuf(out, out_buf, _IOFBF, STDIO_BUFSIZE);

	/* Remote mode */
	if (fio_is_remote_file(in))
	{
		char *errmsg = NULL;
		BlockNumber	err_blknum = 0;

		/* TODO: retrying via ptrack should be implemented on the agent */
		int rc = fio_send_pages(in, out, file,
								/* send prev backup START_LSN */
								backup_mode == BACKUP_MODE_DIFF_DELTA &&
								file->exists_in_prev ? prev_backup_start_lsn : InvalidXLogRecPtr,
								calg, clevel, checksum_version,
								/* send pagemap if any */
								use_pagemap ? &file->pagemap : NULL,
								/* variables for error reporting */
								&err_blknum, &errmsg);

		/* check for errors */
		if (rc == REMOTE_ERROR)
			elog(ERROR, "Cannot read block %u of \"%s\": %s",
					err_blknum, from_fullpath, strerror(errno));

		else if (rc == PAGE_CORRUPTION)
		{
			if (errmsg)
				elog(ERROR, "Corruption detected in file \"%s\", block %u: %s",
						from_fullpath, err_blknum, errmsg);
			else
				elog(ERROR, "Corruption detected in file \"%s\", block %u",
						from_fullpath, err_blknum);
		}

		else if (rc == WRITE_FAILED)
			elog(ERROR, "Cannot write block %u of \"%s\": %s",
					err_blknum, to_fullpath, strerror(errno));

		file->read_size = rc * BLCKSZ;
		pg_free(errmsg);

	}
	/* Local mode */
	else
	{
		if (use_pagemap)
		{
			iter = datapagemap_iterate(&file->pagemap);
			datapagemap_next(iter, &blknum); /* set first block */
		}

		while (blknum < nblocks)
		{
			page_state = prepare_page(conn_arg, file, prev_backup_start_lsn,
										  blknum, in, backup_mode, curr_page,
										  true, checksum_version,
										  ptrack_version_num, ptrack_schema,
										  from_fullpath);

			if (page_state == PageIsTruncated)
				break;

			/* TODO: remove */
			else if (page_state == SkipCurrentPage)
				n_blocks_skipped++;

			else if (page_state == PageIsOk)
				compress_and_backup_page(file, blknum, in, out, &(file->crc),
													page_state, curr_page, calg, clevel,
													from_fullpath, to_fullpath);
			/* TODO: handle PageIsCorrupted, currently it is done in prepare_page */
			else
				Assert(false);


			file->read_size += BLCKSZ;

			/* next block */
			if (use_pagemap)
			{
				/* exit if pagemap is exhausted */
				if (!datapagemap_next(iter, &blknum))
					break;
			}
			else
				blknum++;
		}
	}

	pg_free(file->pagemap.bitmap);
	pg_free(iter);

	/* refresh n_blocks for FULL and DELTA */
	if (backup_mode == BACKUP_MODE_FULL ||
	    backup_mode == BACKUP_MODE_DIFF_DELTA)
		file->n_blocks = file->read_size / BLCKSZ;

	if (fclose(out))
		elog(ERROR, "Cannot close the backup file \"%s\": %s",
			 to_fullpath, strerror(errno));

	fio_fclose(in);

	FIN_FILE_CRC32(true, file->crc);

	/* Determine that file didn`t changed in case of incremental backup */
	if (backup_mode != BACKUP_MODE_FULL &&
		file->exists_in_prev &&
		file->write_size == 0 &&
		file->n_blocks > 0)
	{
		file->write_size = BYTES_INVALID;
	}

	/*
	 * No point in storing empty files.
	 */
	if (file->write_size <= 0)
	{
		if (unlink(to_fullpath) == -1)
			elog(ERROR, "Cannot remove file \"%s\": %s", to_fullpath,
				 strerror(errno));
	}

	pg_free(in_buf);
	pg_free(out_buf);
}

/*
 * Backup non data file
 * We do not apply compression to this file.
 * If file exists in previous backup, then compare checksums
 * and make a decision about copying or skiping the file.
 */
void
backup_non_data_file(pgFile *file, pgFile *prev_file,
				 const char *from_fullpath, const char *to_fullpath,
				 BackupMode backup_mode, time_t parent_backup_time,
				 bool missing_ok)
{
	/* special treatment for global/pg_control */
	if (file->external_dir_num == 0 && strcmp(file->rel_path, XLOG_CONTROL_FILE) == 0)
	{
		copy_pgcontrol_file(from_fullpath, FIO_DB_HOST,
							to_fullpath, FIO_BACKUP_HOST, file);
		return;
	}

	/*
	 * If non-data file exists in previous backup
	 * and its mtime is less than parent backup start time ... */
	if (prev_file && file->exists_in_prev &&
		file->mtime <= parent_backup_time)
	{

		file->crc = fio_get_crc32(from_fullpath, FIO_DB_HOST, false);

		/* ...and checksum is the same... */
		if (EQ_TRADITIONAL_CRC32(file->crc, prev_file->crc))
		{
			file->write_size = BYTES_INVALID;
			return; /* ...skip copying file. */
		}
	}

	backup_non_data_file_internal(from_fullpath, FIO_DB_HOST,
								  to_fullpath, file, true);
}

/*
 * Iterate over parent backup chain and lookup given destination file in
 * filelist of every chain member starting with FULL backup.
 * Apply changed blocks to destination file from every backup in parent chain.
 */
size_t
restore_data_file(parray *parent_chain, pgFile *dest_file, FILE *out, const char *to_fullpath)
{
	int    i;
	size_t total_write_len = 0;
	char  *in_buf = pgut_malloc(STDIO_BUFSIZE);

	for (i = parray_num(parent_chain) - 1; i >= 0; i--)
	{
		char     from_root[MAXPGPATH];
		char     from_fullpath[MAXPGPATH];
		FILE    *in = NULL;

		pgFile **res_file = NULL;
		pgFile  *tmp_file = NULL;

		pgBackup   *backup = (pgBackup *) parray_get(parent_chain, i);

		/* lookup file in intermediate backup */
		res_file =  parray_bsearch(backup->files, dest_file, pgFileCompareRelPathWithExternal);
		tmp_file = (res_file) ? *res_file : NULL;

		/* Destination file is not exists yet at this moment */
		if (tmp_file == NULL)
			continue;

		/*
		 * Skip file if it haven't changed since previous backup
		 * and thus was not backed up.
		 */
		if (tmp_file->write_size == BYTES_INVALID)
			continue;

		/* If file was truncated in intermediate backup,
		 * it is ok not to truncate it now, because old blocks will be
		 * overwritten by new blocks from next backup.
		 */
		if (tmp_file->write_size == 0)
			continue;

		/*
		 * At this point we are sure, that something is going to be copied
		 * Open source file.
		 */
		join_path_components(from_root, backup->root_dir, DATABASE_DIR);
		join_path_components(from_fullpath, from_root, tmp_file->rel_path);

		in = fopen(from_fullpath, PG_BINARY_R);
		if (in == NULL)
			elog(ERROR, "Cannot open backup file \"%s\": %s", from_fullpath,
				 strerror(errno));

		/* set stdio buffering for input data file */
		setvbuf(in, in_buf, _IOFBF, STDIO_BUFSIZE);

		/*
		 * Restore the file.
		 * Datafiles are backed up block by block and every block
		 * have BackupPageHeader with meta information, so we cannot just
		 * copy the file from backup.
		 */
		total_write_len += restore_data_file_internal(in, out, tmp_file,
					  parse_program_version(backup->program_version),
					  from_fullpath, to_fullpath, dest_file->n_blocks);

		if (fclose(in) != 0)
			elog(ERROR, "Cannot close file \"%s\": %s", from_fullpath,
				strerror(errno));
	}
	pg_free(in_buf);

	return total_write_len;
}

size_t
restore_data_file_internal(FILE *in, FILE *out, pgFile *file, uint32 backup_version,
					  const char *from_fullpath, const char *to_fullpath, int nblocks)
{
	BackupPageHeader header;
	BlockNumber	blknum = 0;
	size_t	write_len = 0;
	off_t   cur_pos = 0;

	/*
	 * We rely on stdio buffering of input and output.
	 * For buffering to be efficient, we try to minimize the
	 * number of lseek syscalls, because it forces buffer flush.
	 * For that, we track current write position in
	 * output file and issue fseek only when offset of block to be
	 * written not equal to current write position, which happens
	 * a lot when blocks from incremental backup are restored,
	 * but should never happen in case of blocks from FULL backup.
	 */
	if (fio_fseek(out, cur_pos) < 0)
			elog(ERROR, "Cannot seek block %u of \"%s\": %s",
				blknum, to_fullpath, strerror(errno));

	for (;;)
	{
		off_t		write_pos;
		size_t		read_len;
		DataPage	page;
		int32		compressed_size = 0;
		bool		is_compressed = false;

		/* check for interrupt */
		if (interrupted || thread_interrupted)
			elog(ERROR, "Interrupted during data file restore");

		/* read BackupPageHeader */
		read_len = fread(&header, 1, sizeof(header), in);

		if (ferror(in))
			elog(ERROR, "Cannot read header of block %u of \"%s\": %s",
					 blknum, from_fullpath, strerror(errno));

		if (read_len != sizeof(header))
		{
			if (read_len == 0 && feof(in))
				break;		/* EOF found */

			if (read_len != 0 && feof(in))
				elog(ERROR, "Odd size page found at block %u of \"%s\"",
					 blknum, from_fullpath);
		}

		/* Consider empty blockm. wtf empty block ? */
		if (header.block == 0 && header.compressed_size == 0)
		{
			elog(WARNING, "Skip empty block of \"%s\"", from_fullpath);
			continue;
		}

		/* sanity? */
		if (header.block < blknum)
			elog(ERROR, "Backup is broken at block %u of \"%s\"",
				 blknum, from_fullpath);

		blknum = header.block;

		/*
		 * Backupward compatibility kludge: in the good old days
		 * n_blocks attribute was available only in DELTA backups.
		 * File truncate in PAGE and PTRACK happened on the fly when
		 * special value PageIsTruncated is encountered.
		 * It was inefficient.
		 *
		 * Nowadays every backup type has n_blocks, so instead
		 * writing and then truncating redundant data, writing
		 * is not happening in the first place.
		 * TODO: remove in 3.0.0
		 */
		compressed_size = header.compressed_size;

		if (compressed_size == PageIsTruncated)
		{
			/*
			 * Block header contains information that this block was truncated.
			 * We need to truncate file to this length.
			 */

			elog(VERBOSE, "Truncate file \"%s\" to block %u", to_fullpath, header.block);

			/* To correctly truncate file, we must first flush STDIO buffers */
			if (fio_fflush(out) != 0)
				elog(ERROR, "Cannot flush file \"%s\": %s", to_fullpath, strerror(errno));

			/* Set position to the start of file */
			if (fio_fseek(out, 0) < 0)
				elog(ERROR, "Cannot seek to the start of file \"%s\": %s", to_fullpath, strerror(errno));

			if (fio_ftruncate(out, header.block * BLCKSZ) != 0)
				elog(ERROR, "Cannot truncate file \"%s\": %s", to_fullpath, strerror(errno));

			break;
		}

		/* no point in writing redundant data */
		if (nblocks > 0 && blknum >= nblocks)
			break;

		if (compressed_size > BLCKSZ)
			elog(ERROR, "Size of a blknum %i exceed BLCKSZ", blknum);

		/* read a page from file */
		read_len = fread(page.data, 1, MAXALIGN(compressed_size), in);

		if (read_len != MAXALIGN(compressed_size))
			elog(ERROR, "Cannot read block %u of \"%s\", read %zu of %d",
				blknum, from_fullpath, read_len, compressed_size);

		/*
		 * if page size is smaller than BLCKSZ, decompress the page.
		 * BUGFIX for versions < 2.0.23: if page size is equal to BLCKSZ.
		 * we have to check, whether it is compressed or not using
		 * page_may_be_compressed() function.
		 */
		if (header.compressed_size != BLCKSZ
			|| page_may_be_compressed(page.data, file->compress_alg,
									  backup_version))
		{
			is_compressed = true;
		}

		/*
		 * Seek and write the restored page.
		 * When restoring file from FULL backup, pages are written sequentially,
		 * so there is no need to issue fseek for every page.
		 */
		write_pos = blknum * BLCKSZ;

		if (cur_pos != write_pos)
		{
			if (fio_fseek(out, blknum * BLCKSZ) < 0)
				elog(ERROR, "Cannot seek block %u of \"%s\": %s",
					blknum, to_fullpath, strerror(errno));
		}

		/* If page is compressed and restore is in remote mode, send compressed
		 * page to the remote side.
		 */
		if (is_compressed)
		{
			ssize_t rc;
			rc = fio_fwrite_compressed(out, page.data, compressed_size, file->compress_alg);

			if (!fio_is_remote_file(out) && rc != BLCKSZ)
				elog(ERROR, "Cannot write block %u of \"%s\": %s, size: %u",
					 blknum, to_fullpath, strerror(errno), compressed_size);
		}
		else
		{
			if (fio_fwrite(out, page.data, BLCKSZ) != BLCKSZ)
				elog(ERROR, "Cannot write block %u of \"%s\": %s",
					 blknum, to_fullpath, strerror(errno));
		}

		write_len += BLCKSZ;
		cur_pos = write_pos + BLCKSZ; /* update current write position */
	}

	elog(VERBOSE, "Copied file \"%s\": %lu bytes", from_fullpath, write_len);
	return write_len;
}

/*
 * Copy file to backup.
 * We do not apply compression to these files, because
 * it is either small control file or already compressed cfs file.
 */
void
restore_non_data_file_internal(FILE *in, FILE *out, pgFile *file,
					  const char *from_fullpath, const char *to_fullpath)
{
	size_t     read_len = 0;
	char      *buf = pgut_malloc(STDIO_BUFSIZE); /* 64kB buffer */

	/* copy content */
	for (;;)
	{
		read_len = 0;

		/* check for interrupt */
		if (interrupted || thread_interrupted)
			elog(ERROR, "Interrupted during non-data file restore");

		read_len = fread(buf, 1, STDIO_BUFSIZE, in);

		if (ferror(in))
			elog(ERROR, "Cannot read backup file \"%s\": %s",
				from_fullpath, strerror(errno));

		if (read_len > 0)
		{
			if (fio_fwrite(out, buf, read_len) != read_len)
				elog(ERROR, "Cannot write to \"%s\": %s", to_fullpath,
					 strerror(errno));
		}

		if (feof(in))
			break;
	}

	pg_free(buf);

	elog(VERBOSE, "Copied file \"%s\": %lu bytes", from_fullpath, file->write_size);
}

size_t
restore_non_data_file(parray *parent_chain, pgBackup *dest_backup,
					  pgFile *dest_file, FILE *out, const char *to_fullpath)
{
	int			i;
	char		from_root[MAXPGPATH];
	char		from_fullpath[MAXPGPATH];
	FILE		*in = NULL;

	pgFile		*tmp_file = NULL;
	pgBackup	*tmp_backup = NULL;

	/* Check if full copy of destination file is available in destination backup */
	if (dest_file->write_size > 0)
	{
		tmp_file = dest_file;
		tmp_backup = dest_backup;
	}
	else
	{
		/*
		 * Iterate over parent chain starting from direct parent of destination
		 * backup to oldest backup in chain, and look for the first
		 * full copy of destination file.
		 * Full copy is latest possible destination file with size equal or
		 * greater than zero.
		 */
		for (i = 1; i < parray_num(parent_chain); i++)
		{
			pgFile	   **res_file = NULL;

			tmp_backup = (pgBackup *) parray_get(parent_chain, i);

			/* lookup file in intermediate backup */
			res_file =  parray_bsearch(tmp_backup->files, dest_file, pgFileCompareRelPathWithExternal);
			tmp_file = (res_file) ? *res_file : NULL;

			/*
			 * It should not be possible not to find destination file in intermediate
			 * backup, without encountering full copy first.
			 */
			if (!tmp_file)
			{
				elog(ERROR, "Failed to locate non-data file \"%s\" in backup %s",
					dest_file->rel_path, base36enc(tmp_backup->start_time));
				continue;
			}

			/* Full copy is found and it is null sized, nothing to do here */
			if (tmp_file->write_size == 0)
				return 0;

			/* Full copy is found */
			if (tmp_file->write_size > 0)
				break;
		}
	}

	/* sanity */
	if (!tmp_backup)
		elog(ERROR, "Failed to found a backup containing full copy of non-data file \"%s\"",
			to_fullpath);

	if (!tmp_file)
		elog(ERROR, "Failed to locate a full copy of non-data file \"%s\"", to_fullpath);

	if (tmp_file->external_dir_num == 0)
		join_path_components(from_root, tmp_backup->root_dir, DATABASE_DIR);
	else
	{
		char		external_prefix[MAXPGPATH];

		join_path_components(external_prefix, tmp_backup->root_dir, EXTERNAL_DIR);
		makeExternalDirPathByNum(from_root, external_prefix, tmp_file->external_dir_num);
	}

	join_path_components(from_fullpath, from_root, dest_file->rel_path);

	in = fopen(from_fullpath, PG_BINARY_R);
	if (in == NULL)
		elog(ERROR, "Cannot open backup file \"%s\": %s", from_fullpath,
			 strerror(errno));

	/* disable stdio buffering for non-data files */
	setvbuf(in, NULL, _IONBF, BUFSIZ);

	/* do actual work */
	restore_non_data_file_internal(in, out, tmp_file, from_fullpath, to_fullpath);

	if (fclose(in) != 0)
		elog(ERROR, "Cannot close file \"%s\": %s", from_fullpath,
			strerror(errno));

	return tmp_file->write_size;
}

/*
 * Copy file to backup.
 * We do not apply compression to these files, because
 * it is either small control file or already compressed cfs file.
 * TODO: optimize remote copying
 */
void
backup_non_data_file_internal(const char *from_fullpath,
							fio_location from_location,
							const char *to_fullpath, pgFile *file,
							bool missing_ok)
{
	FILE       *in;
	FILE       *out;
	ssize_t     read_len = 0;
	char	   *buf;
	pg_crc32	crc;

	INIT_FILE_CRC32(true, crc);

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;
	file->uncompressed_size = 0;

	/* open source file for read */
	in = fio_fopen(from_fullpath, PG_BINARY_R, from_location);
	if (in == NULL)
	{
		FIN_FILE_CRC32(true, crc);
		file->crc = crc;

		/* maybe deleted, it's not error in case of backup */
		if (errno == ENOENT)
		{
			if (missing_ok)
			{
				elog(LOG, "File \"%s\" is not found", from_fullpath);
				file->write_size = FILE_NOT_FOUND;
				return;
			}
			else
				elog(ERROR, "File \"%s\" is not found", from_fullpath);
		}

		elog(ERROR, "Cannot open source file \"%s\": %s", from_fullpath,
			 strerror(errno));
	}

	/* open backup file for write  */
	out = fopen(to_fullpath, PG_BINARY_W);
	if (out == NULL)
		elog(ERROR, "Cannot open destination file \"%s\": %s",
			 to_fullpath, strerror(errno));

	/* update file permission */
	if (chmod(to_fullpath, file->mode) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", to_fullpath,
			 strerror(errno));

	/* disable stdio buffering for local input/output files */
	if (!fio_is_remote_file(in))
		setvbuf(in, NULL, _IONBF, BUFSIZ);
	setvbuf(out, NULL, _IONBF, BUFSIZ);

	/* allocate 64kB buffer */
	buf = pgut_malloc(STDIO_BUFSIZE);

	/* copy content and calc CRC */
	for (;;)
	{
		read_len = fio_fread(in, buf, STDIO_BUFSIZE);

		if (read_len < 0)
			elog(ERROR, "Cannot read from source file \"%s\": %s",
				 from_fullpath, strerror(errno));

		if (read_len == 0)
			break;

		if (fwrite(buf, 1, read_len, out) != read_len)
			elog(ERROR, "Cannot write to \"%s\": %s", to_fullpath,
				 strerror(errno));

		/* update CRC */
		COMP_FILE_CRC32(true, crc, buf, read_len);

		file->read_size += read_len;

//		if (read_len < STDIO_BUFSIZE)
//		{
//			if (!fio_is_remote_file(in))
//			{
//				if (ferror(in))
//					elog(ERROR, "Cannot read from source file \"%s\": %s",
//						from_fullpath, strerror(errno));
//
//				if (feof(in))
//					break;
//			}
//		}
	}

	file->write_size = (int64) file->read_size;

	if (file->write_size > 0)
		file->uncompressed_size = file->write_size;
	/* finish CRC calculation and store into pgFile */
	FIN_FILE_CRC32(true, crc);
	file->crc = crc;

	if (fclose(out))
		elog(ERROR, "Cannot write \"%s\": %s", to_fullpath, strerror(errno));
	fio_fclose(in);
	pg_free(buf);
}

/*
 * Create empty file, used for partial restore
 */
bool
create_empty_file(fio_location from_location, const char *to_root,
		  fio_location to_location, pgFile *file)
{
	char		to_path[MAXPGPATH];
	FILE	   *out;

	/* open file for write  */
	join_path_components(to_path, to_root, file->rel_path);
	out = fio_fopen(to_path, PG_BINARY_W, to_location);

	if (out == NULL)
		elog(ERROR, "Cannot open destination file \"%s\": %s",
			 to_path, strerror(errno));

	/* update file permission */
	if (fio_chmod(to_path, file->mode, to_location) == -1)
		elog(ERROR, "Cannot change mode of \"%s\": %s", to_path,
			 strerror(errno));

	if (fio_fclose(out))
		elog(ERROR, "Cannot close \"%s\": %s", to_path, strerror(errno));

	return true;
}

/*
 * Validate given page.
 * This function is expected to be executed multiple times,
 * so avoid using elog within it.
 * lsn from page is assigned to page_lsn pointer.
 * TODO: switch to enum for return codes.
 */
int
validate_one_page(Page page, BlockNumber absolute_blkno,
					XLogRecPtr stop_lsn, XLogRecPtr *page_lsn,
					uint32 checksum_version)
{
	/* new level of paranoia */
	if (page == NULL)
		return PAGE_IS_NOT_FOUND;

	/* check that page header is ok */
	if (!parse_page(page, page_lsn))
	{
		int		i;
		/* Check if the page is zeroed. */
		for (i = 0; i < BLCKSZ && page[i] == 0; i++);

		/* Page is zeroed. No need to verify checksums */
		if (i == BLCKSZ)
			return PAGE_IS_ZEROED;

		/* Page does not looking good */
		return PAGE_HEADER_IS_INVALID;
	}

	/* Verify checksum */
	if (checksum_version)
	{
		/* Checksums are enabled, so check them. */
		if (pg_checksum_page(page, absolute_blkno) != ((PageHeader) page)->pd_checksum)
			return PAGE_CHECKSUM_MISMATCH;
	}

	/* At this point page header is sane, if checksums are enabled - the`re ok.
	 * Check that page is not from future.
	 * Note, this check should be used only by validate command.
	 */
	if (stop_lsn > 0)
	{
		/* Get lsn from page header. Ensure that page is from our time. */
		if (*page_lsn > stop_lsn)
			return PAGE_LSN_FROM_FUTURE;
	}

	return PAGE_IS_VALID;
}

/*
 * Valiate pages of datafile in PGDATA one by one.
 *
 * returns true if the file is valid
 * also returns true if the file was not found
 */
bool
check_data_file(ConnectionArgs *arguments, pgFile *file,
				const char *from_fullpath, uint32 checksum_version)
{
	FILE		*in;
	BlockNumber	blknum = 0;
	BlockNumber	nblocks = 0;
	int			page_state;
	char		curr_page[BLCKSZ];
	bool 		is_valid = true;

	in = fopen(from_fullpath, PG_BINARY_R);
	if (in == NULL)
	{
		/*
		 * If file is not found, this is not en error.
		 * It could have been deleted by concurrent postgres transaction.
		 */
		if (errno == ENOENT)
		{
			elog(LOG, "File \"%s\" is not found", from_fullpath);
			return true;
		}

		elog(WARNING, "Cannot open file \"%s\": %s",
					from_fullpath, strerror(errno));
		return false;
	}

	if (file->size % BLCKSZ != 0)
		elog(WARNING, "File: \"%s\", invalid file size %zu", from_fullpath, file->size);

	/*
	 * Compute expected number of blocks in the file.
	 * NOTE This is a normal situation, if the file size has changed
	 * since the moment we computed it.
	 */
	nblocks = file->size/BLCKSZ;

	for (blknum = 0; blknum < nblocks; blknum++)
	{

		page_state = prepare_page(NULL, file, InvalidXLogRecPtr,
									blknum, in, BACKUP_MODE_FULL,
									curr_page, false, checksum_version,
									0, NULL, from_fullpath);

		if (page_state == PageIsTruncated)
			break;

		if (page_state == PageIsCorrupted)
		{
			/* Page is corrupted, no need to elog about it,
			 * prepare_page() already done that
			 */
			is_valid = false;
			continue;
		}
	}

	fclose(in);
	return is_valid;
}

/* Valiate pages of datafile in backup one by one */
bool
check_file_pages(pgFile *file, const char *fullpath, XLogRecPtr stop_lsn,
				 uint32 checksum_version, uint32 backup_version)
{
	size_t		read_len = 0;
	bool		is_valid = true;
	FILE		*in;
	pg_crc32	crc;
	bool		use_crc32c = backup_version <= 20021 || backup_version >= 20025;

	elog(VERBOSE, "Validate relation blocks for file \"%s\"", fullpath);

	in = fopen(fullpath, PG_BINARY_R);
	if (in == NULL)
	{
		if (errno == ENOENT)
		{
			elog(WARNING, "File \"%s\" is not found", fullpath);
			return false;
		}

		elog(ERROR, "Cannot open file \"%s\": %s",
			 fullpath, strerror(errno));
	}

	/* calc CRC of backup file */
	INIT_FILE_CRC32(use_crc32c, crc);

	/* read and validate pages one by one */
	while (true)
	{
		int		rc = 0;
		DataPage	compressed_page; /* used as read buffer */
		DataPage	page;
		BackupPageHeader header;
		BlockNumber blknum = 0;
		XLogRecPtr	page_lsn = 0;

		if (interrupted || thread_interrupted)
			elog(ERROR, "Interrupted during data file validation");

		/* read BackupPageHeader */
		read_len = fread(&header, 1, sizeof(header), in);

		if (ferror(in))
			elog(ERROR, "Cannot read header of block %u of \"%s\": %s",
					 blknum, fullpath, strerror(errno));

		if (read_len != sizeof(header))
		{
			if (read_len == 0 && feof(in))
				break;		/* EOF found */
			else if (read_len != 0 && feof(in))
				elog(WARNING,
					 "Odd size page found at block %u of \"%s\"",
					 blknum, fullpath);
			else
				elog(WARNING, "Cannot read header of block %u of \"%s\": %s",
					 blknum, fullpath, strerror(errno));
			return false;
		}

		COMP_FILE_CRC32(use_crc32c, crc, &header, read_len);

		if (header.block == 0 && header.compressed_size == 0)
		{
			elog(VERBOSE, "Skip empty block of \"%s\"", fullpath);
			continue;
		}

		if (header.block < blknum)
		{
			elog(WARNING, "Backup is broken at block %u of \"%s\"",
				 blknum, fullpath);
			return false;
		}

		blknum = header.block;

		if (header.compressed_size == PageIsTruncated)
		{
			elog(LOG, "Block %u of \"%s\" is truncated",
				 blknum, fullpath);
			continue;
		}

		Assert(header.compressed_size <= BLCKSZ);

		read_len = fread(compressed_page.data, 1,
			MAXALIGN(header.compressed_size), in);
		if (read_len != MAXALIGN(header.compressed_size))
		{
			elog(WARNING, "Cannot read block %u of \"%s\" read %zu of %d",
				blknum, fullpath, read_len, header.compressed_size);
			return false;
		}

		COMP_FILE_CRC32(use_crc32c, crc, compressed_page.data, read_len);

		if (header.compressed_size != BLCKSZ
			|| page_may_be_compressed(compressed_page.data, file->compress_alg,
									  backup_version))
		{
			int32		uncompressed_size = 0;
			const char *errormsg = NULL;

			uncompressed_size = do_decompress(page.data, BLCKSZ,
											  compressed_page.data,
											  header.compressed_size,
											  file->compress_alg,
											  &errormsg);
			if (uncompressed_size < 0 && errormsg != NULL)
				elog(WARNING, "An error occured during decompressing block %u of file \"%s\": %s",
					 blknum, fullpath, errormsg);

			if (uncompressed_size != BLCKSZ)
			{
				if (header.compressed_size == BLCKSZ)
				{
					is_valid = false;
					continue;
				}
				elog(WARNING, "Page of file \"%s\" uncompressed to %d bytes. != BLCKSZ",
						fullpath, uncompressed_size);
				return false;
			}

			rc = validate_one_page(page.data,
				                       file->segno * RELSEG_SIZE + blknum,
									   stop_lsn, &page_lsn, checksum_version);
		}
		else
			rc = validate_one_page(compressed_page.data,
									   file->segno * RELSEG_SIZE + blknum,
									   stop_lsn, &page_lsn, checksum_version);

		switch (rc)
		{
			case PAGE_IS_NOT_FOUND:
				elog(LOG, "File \"%s\", block %u, page is NULL", file->rel_path, blknum);
				break;
			case PAGE_IS_ZEROED:
				elog(LOG, "File: %s blknum %u, empty zeroed page", file->rel_path, blknum);
				break;
			case PAGE_HEADER_IS_INVALID:
				elog(WARNING, "Page header is looking insane: %s, block %i", file->rel_path, blknum);
				is_valid = false;
				break;
			case PAGE_CHECKSUM_MISMATCH:
				elog(WARNING, "File: %s blknum %u have wrong checksum", file->rel_path, blknum);
				is_valid = false;
				break;
			case PAGE_LSN_FROM_FUTURE:
				elog(WARNING, "File: %s, block %u, checksum is %s. "
								"Page is from future: pageLSN %X/%X stopLSN %X/%X",
							file->rel_path, blknum,
							checksum_version ? "correct" : "not enabled",
							(uint32) (page_lsn >> 32), (uint32) page_lsn,
							(uint32) (stop_lsn >> 32), (uint32) stop_lsn);
				break;
		}
	}

	FIN_FILE_CRC32(use_crc32c, crc);
	fclose(in);

	if (crc != file->crc)
	{
		elog(WARNING, "Invalid CRC of backup file \"%s\": %X. Expected %X",
				fullpath, crc, file->crc);
		is_valid = false;
	}

	return is_valid;
}
