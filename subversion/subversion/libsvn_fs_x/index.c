/* index.c indexing support for FSX support
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <assert.h>

#include "svn_io.h"
#include "svn_pools.h"
#include "svn_sorts.h"

#include "index.h"
#include "util.h"
#include "pack.h"

#include "private/svn_dep_compat.h"
#include "private/svn_sorts_private.h"
#include "private/svn_subr_private.h"
#include "private/svn_temp_serializer.h"

#include "svn_private_config.h"
#include "temp_serializer.h"

#include "../libsvn_fs/fs-loader.h"

/* maximum length of a uint64 in an 7/8b encoding */
#define ENCODED_INT_LENGTH 10

/* Page tables in the log-to-phys index file exclusively contain entries
 * of this type to describe position and size of a given page.
 */
typedef struct l2p_page_table_entry_t
{
  /* global offset on the page within the index file */
  apr_uint64_t offset;

  /* number of mapping entries in that page */
  apr_uint32_t entry_count;

  /* size of the page on disk (in the index file) */
  apr_uint32_t size;
} l2p_page_table_entry_t;

/* Master run-time data structure of an log-to-phys index.  It contains
 * the page tables of every revision covered by that index - but not the
 * pages themselves. 
 */
typedef struct l2p_header_t
{
  /* first revision covered by this index */
  svn_revnum_t first_revision;

  /* number of revisions covered */
  apr_size_t revision_count;

  /* (max) number of entries per page */
  apr_uint32_t page_size;

  /* indexes into PAGE_TABLE that mark the first page of the respective
   * revision.  PAGE_TABLE_INDEX[REVISION_COUNT] points to the end of
   * PAGE_TABLE.
   */
  apr_size_t * page_table_index;

  /* Page table covering all pages in the index */
  l2p_page_table_entry_t * page_table;
} l2p_header_t;

/* Run-time data structure containing a single log-to-phys index page.
 */
typedef struct l2p_page_t
{
  /* number of entries in the OFFSETS array */
  apr_uint32_t entry_count;

  /* global file offsets (item index is the array index) within the
   * packed or non-packed rev file.  Offset will be -1 for unused /
   * invalid item index values. */
  apr_off_t *offsets;

  /* In case that the item is stored inside a container, this is the
   * identifying index of the item within that container.  0 for the
   * container itself or for items that aren't containers. */
  apr_uint32_t *sub_items;
} l2p_page_t;

/* All of the log-to-phys proto index file consist of entries of this type.
 */
typedef struct l2p_proto_entry_t
{
  /* phys offset + 1 of the data container. 0 for "new revision" entries. */
  apr_uint64_t offset;

  /* corresponding item index. 0 for "new revision" entries. */
  apr_uint64_t item_index;

  /* index within the container starting @ offset.  0 for "new revision"
   * entries and for items with no outer container. */
  apr_uint32_t sub_item;
} l2p_proto_entry_t;

/* Master run-time data structure of an phys-to-log index.  It contains
 * an array with one offset value for each rev file cluster.
 */
typedef struct p2l_header_t
{
  /* first revision covered by the index (and rev file) */
  svn_revnum_t first_revision;

  /* number of bytes in the rev files covered by each p2l page */
  apr_uint64_t page_size;

  /* number of pages / clusters in that rev file */
  apr_size_t page_count;

  /* number of bytes in the rev file */
  apr_uint64_t file_size;

  /* offsets of the pages / cluster descriptions within the index file */
  apr_off_t *offsets;
} p2l_header_t;

/*
 * packed stream array
 */

/* How many numbers we will pre-fetch and buffer in a packed number stream.
 */
enum { MAX_NUMBER_PREFETCH = 64 };

/* Prefetched number entry in a packed number stream.
 */
typedef struct value_position_pair_t
{
  /* prefetched number */
  apr_uint64_t value;

  /* number of bytes read, *including* this number, since the buffer start */
  apr_size_t total_len;
} value_position_pair_t;

/* State of a prefetching packed number stream.  It will read compressed
 * index data efficiently and present it as a series of non-packed uint64.
 */
typedef struct packed_number_stream_t
{
  /* underlying data file containing the packed values */
  apr_file_t *file;

  /* number of used entries in BUFFER (starting at index 0) */
  apr_size_t used;

  /* index of the next number to read from the BUFFER (0 .. USED).
   * If CURRENT == USED, we need to read more data upon get() */
  apr_size_t current;

  /* offset in FILE from which the first entry in BUFFER has been read */
  apr_off_t start_offset;

  /* offset in FILE from which the next number has to be read */
  apr_off_t next_offset;

  /* read the file in chunks of this size */
  apr_size_t block_size;

  /* pool to be used for file ops etc. */
  apr_pool_t *pool;

  /* buffer for prefetched values */
  value_position_pair_t buffer[MAX_NUMBER_PREFETCH];
} packed_number_stream_t;

/* Return an svn_error_t * object for error ERR on STREAM with the given
 * MESSAGE string.  The latter must have a placeholder for the index file
 * name ("%s") and the current read offset (e.g. "0x%lx").
 */
static svn_error_t *
stream_error_create(packed_number_stream_t *stream,
                    apr_status_t err,
                    const char *message)
{
  const char *file_name;
  apr_off_t offset = 0;
  SVN_ERR(svn_io_file_name_get(&file_name, stream->file,
                               stream->pool));
  SVN_ERR(svn_io_file_seek(stream->file, APR_CUR, &offset, stream->pool));

  return svn_error_createf(err, NULL, message, file_name,
                           (apr_uint64_t)offset);
}

/* Read up to MAX_NUMBER_PREFETCH numbers from the STREAM->NEXT_OFFSET in
 * STREAM->FILE and buffer them.
 *
 * We don't want GCC and others to inline this (infrequently called)
 * function into packed_stream_get() because it prevents the latter from
 * being inlined itself.
 */
SVN__PREVENT_INLINE
static svn_error_t *
packed_stream_read(packed_number_stream_t *stream)
{
  unsigned char buffer[MAX_NUMBER_PREFETCH];
  apr_size_t read = 0;
  apr_size_t i;
  value_position_pair_t *target;
  apr_off_t block_start = 0;
  apr_off_t block_left = 0;
  apr_status_t err;

  /* all buffered data will have been read starting here */
  stream->start_offset = stream->next_offset;

  /* packed numbers are usually not aligned to MAX_NUMBER_PREFETCH blocks,
   * i.e. the last number has been incomplete (and not buffered in stream)
   * and need to be re-read.  Therefore, always correct the file pointer.
   */
  SVN_ERR(svn_io_file_aligned_seek(stream->file, stream->block_size,
                                   &block_start, stream->next_offset,
                                   stream->pool));

  /* prefetch at least one number but, if feasible, don't cross block
   * boundaries.  This shall prevent jumping back and forth between two
   * blocks because the extra data was not actually request _now_.
   */
  read = sizeof(buffer);
  block_left = stream->block_size - (stream->next_offset - block_start);
  if (block_left >= 10 && block_left < read)
    read = block_left;

  err = apr_file_read(stream->file, buffer, &read);
  if (err && !APR_STATUS_IS_EOF(err))
    return stream_error_create(stream, err,
      _("Can't read index file '%s' at offset 0x%" APR_UINT64_T_HEX_FMT));

  /* if the last number is incomplete, trim it from the buffer */
  while (read > 0 && buffer[read-1] >= 0x80)
    --read;

  /* we call read() only if get() requires more data.  So, there must be
   * at least *one* further number. */
  if SVN__PREDICT_FALSE(read == 0)
    return stream_error_create(stream, err,
      _("Unexpected end of index file %s at offset 0x%"APR_UINT64_T_HEX_FMT));

  /* parse file buffer and expand into stream buffer */
  target = stream->buffer;
  for (i = 0; i < read;)
    {
      if (buffer[i] < 0x80)
        {
          /* numbers < 128 are relatively frequent and particularly easy
           * to decode.  Give them special treatment. */
          target->value = buffer[i];
          ++i;
          target->total_len = i;
          ++target;
        }
      else
        {
          apr_uint64_t value = 0;
          apr_uint64_t shift = 0;
          while (buffer[i] >= 0x80)
            {
              value += ((apr_uint64_t)buffer[i] & 0x7f) << shift;
              shift += 7;
              ++i;
            }

          target->value = value + ((apr_uint64_t)buffer[i] << shift);
          ++i;
          target->total_len = i;
          ++target;

          /* let's catch corrupted data early.  It would surely cause
           * havoc further down the line. */
          if SVN__PREDICT_FALSE(shift > 8 * sizeof(value))
            return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_CORRUPTION, NULL,
                                     _("Corrupt index: number too large"));
       }
    }

  /* update stream state */
  stream->used = target - stream->buffer;
  stream->next_offset = stream->start_offset + i;
  stream->current = 0;

  return SVN_NO_ERROR;
}

/* Create and open a packed number stream reading from FILE_NAME and
 * return it in *STREAM.  Access the file in chunks of BLOCK_SIZE bytes.
 * Use POOL for allocations.
 */
static svn_error_t *
packed_stream_open(packed_number_stream_t **stream,
                   const char *file_name,
                   apr_size_t block_size,
                   apr_pool_t *pool)
{
  packed_number_stream_t *result = apr_palloc(pool, sizeof(*result));
  result->pool = svn_pool_create(pool);

  SVN_ERR(svn_io_file_open(&result->file, file_name,
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT,
                           result->pool));
  
  result->used = 0;
  result->current = 0;
  result->start_offset = 0;
  result->next_offset = 0;
  result->block_size = block_size;

  *stream = result;
  
  return SVN_NO_ERROR;
}

/* Close STREAM which may be NULL.
 */
SVN__FORCE_INLINE
static svn_error_t *
packed_stream_close(packed_number_stream_t *stream)
{
  if (stream)
    {
      SVN_ERR(svn_io_file_close(stream->file, stream->pool));
      svn_pool_destroy(stream->pool);
    }

  return SVN_NO_ERROR;
}

/*
 * The forced inline is required for performance reasons:  This is a very
 * hot code path (called for every item we read) but e.g. GCC would rather
 * chose to inline packed_stream_read() here, preventing packed_stream_get
 * from being inlined itself.
 */
SVN__FORCE_INLINE
static svn_error_t*
packed_stream_get(apr_uint64_t *value,
                  packed_number_stream_t *stream)
{
  if (stream->current == stream->used)
    SVN_ERR(packed_stream_read(stream));

  *value = stream->buffer[stream->current].value;
  ++stream->current;

  return SVN_NO_ERROR;
}

/* Navigate STREAM to packed file offset OFFSET.  There will be no checks
 * whether the given OFFSET is valid.
 */
static void
packed_stream_seek(packed_number_stream_t *stream,
                   apr_off_t offset)
{
  if (   stream->used == 0
      || offset < stream->start_offset
      || offset >= stream->next_offset)
    {
      /* outside buffered data.  Next get() will read() from OFFSET. */
      stream->start_offset = offset;
      stream->next_offset = offset;
      stream->current = 0;
      stream->used = 0;
    }
  else
    {
      /* Find the suitable location in the stream buffer.
       * Since our buffer is small, it is efficient enough to simply scan
       * it for the desired position. */
      apr_size_t i;
      for (i = 0; i < stream->used; ++i)
        if (stream->buffer[i].total_len > offset - stream->start_offset)
          break;

      stream->current = i;
    }
}

/* Return the packed file offset of at which the next number in the stream
 * can be found.
 */
static apr_off_t
packed_stream_offset(packed_number_stream_t *stream)
{
  return stream->current == 0
       ? stream->start_offset
       : stream->buffer[stream->current-1].total_len + stream->start_offset;
}

/*
 * log-to-phys index
 */
svn_error_t *
svn_fs_x__l2p_proto_index_open(apr_file_t **proto_index,
                               const char *file_name,
                               apr_pool_t *pool)
{
  SVN_ERR(svn_io_file_open(proto_index, file_name, APR_READ | APR_WRITE
                           | APR_CREATE | APR_APPEND | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  return SVN_NO_ERROR;
}

/* Write ENTRY to log-to-phys PROTO_INDEX file and verify the results.
 * Use POOL for allocations.
 */
static svn_error_t *
write_entry_to_proto_index(apr_file_t *proto_index,
                           l2p_proto_entry_t entry,
                           apr_pool_t *pool)
{
  apr_size_t written = sizeof(entry);

  SVN_ERR(svn_io_file_write(proto_index, &entry, &written, pool));
  SVN_ERR_ASSERT(written == sizeof(entry));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__l2p_proto_index_add_revision(apr_file_t *proto_index,
                                       apr_pool_t *pool)
{
  l2p_proto_entry_t entry = { 0 };
  return svn_error_trace(write_entry_to_proto_index(proto_index, entry,
                                                    pool));
}

svn_error_t *
svn_fs_x__l2p_proto_index_add_entry(apr_file_t *proto_index,
                                    apr_off_t offset,
                                    apr_uint32_t sub_item,
                                    apr_uint64_t item_index,
                                    apr_pool_t *pool)
{
  l2p_proto_entry_t entry = { 0 };

  /* make sure the conversion to uint64 works */
  SVN_ERR_ASSERT(offset >= -1);

  /* we support offset '-1' as a "not used" indication */
  entry.offset = (apr_uint64_t)offset + 1;

  /* make sure we can use item_index as an array index when building the
   * final index file */
  SVN_ERR_ASSERT(item_index < UINT_MAX / 2);
  entry.item_index = item_index;

  /* no limits on the container sub-item index */
  entry.sub_item = sub_item;

  return svn_error_trace(write_entry_to_proto_index(proto_index, entry,
                                                    pool));
}

/* Encode VALUE as 7/8b into P and return the number of bytes written.
 * This will be used when _writing_ packed data.  packed_stream_* is for
 * read operations only.
 */
static apr_size_t
encode_uint(unsigned char *p, apr_uint64_t value)
{
  unsigned char *start = p;
  while (value >= 0x80)
    {
      *p = (unsigned char)((value % 0x80) + 0x80);
      value /= 0x80;
      ++p;
    }

  *p = (unsigned char)(value % 0x80);
  return (p - start) + 1;
}

/* Encode VALUE as 7/8b into P and return the number of bytes written.
 * This maps signed ints onto unsigned ones.
 */
static apr_size_t
encode_int(unsigned char *p, apr_int64_t value)
{
  return encode_uint(p, (apr_uint64_t)(value < 0 ? -1 - 2*value : 2*value));
}

/* Run-length-encode the uint64 numbers in ARRAY starting at index START
 * up to but not including END.  All numbers must be > 0.
 * Return the number of remaining entries in ARRAY after START.
 */
static int
rle_array(apr_array_header_t *array, int start, int end)
{
  int i;
  int target = start;
  for (i = start; i < end; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(array, i, apr_uint64_t);
      assert(value > 0);

      if (value == 1)
        {
          int counter;
          for (counter = 1; i + counter < end; ++counter)
            if (APR_ARRAY_IDX(array, i + counter, apr_uint64_t) != 1)
              break;

          if (--counter)
            {
              APR_ARRAY_IDX(array, target, apr_uint64_t) = 0;
              APR_ARRAY_IDX(array, target + 1, apr_uint64_t) = counter;
              target += 2;
              i += counter;
              continue;
            }
        }

      APR_ARRAY_IDX(array, target, apr_uint64_t) = value;
      ++target;
    }

  return target;
}

/* Utility data structure describing an log-2-phys page entry.
 * This is only used as a transient representation during index creation.
 */
typedef struct l2p_page_entry_t
{
  apr_uint64_t offset;
  apr_uint32_t sub_item;
} l2p_page_entry_t;

/* qsort-compatible compare function taking two l2p_page_entry_t and
 * ordering them by offset.
 */
static int
compare_l2p_entries_by_offset(const l2p_page_entry_t *lhs,
                              const l2p_page_entry_t *rhs)
{
  return lhs->offset > rhs->offset ? 1
                                   : lhs->offset == rhs->offset ? 0 : -1;
}

/* Write the log-2-phys index page description for the l2p_page_entry_t
 * array ENTRIES, starting with element START up to but not including END.
 * Write the resulting representation into BUFFER.  Use POOL for temporary
 * allocations.
 */
static svn_error_t *
encode_l2p_page(apr_array_header_t *entries,
                int start,
                int end,
                svn_spillbuf_t *buffer,
                apr_pool_t *pool)
{
  unsigned char encoded[ENCODED_INT_LENGTH];
  apr_hash_t *containers = apr_hash_make(pool);
  int count = end - start;
  int container_count = 0;
  apr_uint64_t last_offset = 0;
  int i;
  
  apr_size_t data_size = count * sizeof(l2p_page_entry_t);
  svn_stringbuf_t *container_offsets
    = svn_stringbuf_create_ensure(count * 2, pool);

  /* SORTED: relevant items from ENTRIES, sorted by offset */
  l2p_page_entry_t *sorted
    = apr_pmemdup(pool,
                  entries->elts + start * sizeof(l2p_page_entry_t),
                  data_size);
  qsort(sorted, end - start, sizeof(l2p_page_entry_t),
        (int (*)(const void *, const void *))compare_l2p_entries_by_offset);

  /* identify container offsets and create container list */
  for (i = 0; i < count; ++i)
    {
      /* skip "unused" entries */
      if (sorted[i].offset == 0)
        continue;
      
      /* offset already covered? */
      if (i > 0 && sorted[i].offset == sorted[i-1].offset)
        continue;

      /* is this a container item
       * (appears more than once or accesses to sub-items other than 0)? */
      if (   (i != count-1 && sorted[i].offset == sorted[i+1].offset)
          || (sorted[i].sub_item != 0))
        {
          svn_stringbuf_appendbytes(container_offsets, (const char *)encoded,
                                    encode_uint(encoded,   sorted[i].offset
                                                         - last_offset));
          last_offset = sorted[i].offset;
          apr_hash_set(containers,
                       &sorted[i].offset,
                       sizeof(sorted[i].offset),
                       (void *)(apr_uintptr_t)++container_count);
        }
    }

  /* write container list to BUFFER */
  SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                              encode_uint(encoded, container_count),
                              pool));
  SVN_ERR(svn_spillbuf__write(buffer, (const char *)container_offsets->data,
                              container_offsets->len, pool));

  /* encode items */
  for (i = start; i < end; ++i)
    {
      l2p_page_entry_t *entry = &APR_ARRAY_IDX(entries, i, l2p_page_entry_t);
      if (entry->offset == 0)
        {
          SVN_ERR(svn_spillbuf__write(buffer, "\0", 1, pool));
        }
      else
        {
          void *void_idx = apr_hash_get(containers, &entry->offset,
                                        sizeof(entry->offset));
          if (void_idx == NULL)
            {
              apr_uint64_t value = entry->offset + container_count;
              SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                          encode_uint(encoded, value), pool));
            }
          else
            {
              apr_uintptr_t idx = (apr_uintptr_t)void_idx;
              apr_uint64_t value = entry->sub_item;
              SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                          encode_uint(encoded, idx), pool));
              SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                          encode_uint(encoded, value), pool));
            }
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
index_create(apr_file_t **index_file, const char *file_name, apr_pool_t *pool)
{
  /* remove any old index file
   * (it would probably be r/o and simply re-writing it would fail) */
  SVN_ERR(svn_io_remove_file2(file_name, TRUE, pool));

  /* We most likely own the write lock to the repo, so this should
   * either just work or fail indicating a serious problem. */
  SVN_ERR(svn_io_file_open(index_file, file_name,
                           APR_WRITE | APR_CREATE | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__l2p_index_create(svn_fs_t *fs,
                           const char *file_name,
                           const char *proto_file_name,
                           svn_revnum_t revision,
                           apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_file_t *proto_index = NULL;
  int i;
  int end;
  apr_uint64_t entry;
  svn_boolean_t eof = FALSE;
  apr_file_t *index_file;
  unsigned char encoded[ENCODED_INT_LENGTH];

  int last_page_count = 0;          /* total page count at the start of
                                       the current revision */

  /* temporary data structures that collect the data which will be moved
     to the target file in a second step */
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_pool_t *iterpool = svn_pool_create(local_pool);
  apr_array_header_t *page_counts
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));
  apr_array_header_t *page_sizes
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));
  apr_array_header_t *entry_counts
    = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));

  /* collect the item offsets and sub-item value for the current revision */
  apr_array_header_t *entries
    = apr_array_make(local_pool, 256, sizeof(l2p_page_entry_t));

  /* 64k blocks, spill after 16MB */
  svn_spillbuf_t *buffer
    = svn_spillbuf__create(0x10000, 0x1000000, local_pool);

  /* Paranoia check that makes later casting to int32 safe.
   * The current implementation is limited to 2G entries per page. */
  if (ffd->l2p_page_size > APR_INT32_MAX)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                            _("L2P index page size  %s" 
                              " exceeds current limit of 2G entries"),
                            apr_psprintf(local_pool, "%" APR_UINT64_T_FMT,
                                         ffd->l2p_page_size));

  /* start at the beginning of the source file */
  SVN_ERR(svn_io_file_open(&proto_index, proto_file_name,
                           APR_READ | APR_CREATE | APR_BUFFERED,
                           APR_OS_DEFAULT, local_pool));

  /* process all entries until we fail due to EOF */
  for (entry = 0; !eof; ++entry)
    {
      l2p_proto_entry_t proto_entry;
      apr_size_t read = 0;

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(proto_index,
                                     &proto_entry, sizeof(proto_entry),
                                     &read, &eof, local_pool));
      SVN_ERR_ASSERT(eof || read == sizeof(proto_entry));

      /* handle new revision */
      if ((entry > 0 && proto_entry.offset == 0) || eof)
        {
          /* dump entries, grouped into pages */

          int entry_count = 0;
          for (i = 0; i < entries->nelts; i += entry_count)
            {
              /* 1 page with up to 8k entries */
              apr_size_t last_buffer_size = svn_spillbuf__get_size(buffer);

              svn_pool_clear(iterpool);

              entry_count = ffd->l2p_page_size < entries->nelts - i
                          ? (int)ffd->l2p_page_size
                          : entries->nelts - i;
              SVN_ERR(encode_l2p_page(entries, i, i + entry_count,
                                      buffer, iterpool));

              APR_ARRAY_PUSH(entry_counts, apr_uint64_t) = entry_count;
              APR_ARRAY_PUSH(page_sizes, apr_uint64_t)
                = svn_spillbuf__get_size(buffer) - last_buffer_size;
            }

          apr_array_clear(entries);

          /* store the number of pages in this revision */
          APR_ARRAY_PUSH(page_counts, apr_uint64_t)
            = page_sizes->nelts - last_page_count;

          last_page_count = page_sizes->nelts;
        }
      else
        {
          int idx;

          /* store the mapping in our array */
          l2p_page_entry_t page_entry = { 0 };

          if (proto_entry.item_index > APR_INT32_MAX)
            return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                                    _("Item index %s too large "
                                      "in l2p proto index for revision %ld"),
                                    apr_psprintf(local_pool,
                                                 "%" APR_UINT64_T_FMT,
                                                proto_entry.item_index),
                                    revision + page_counts->nelts);

          idx = (int)proto_entry.item_index;
          while (idx >= entries->nelts)
            APR_ARRAY_PUSH(entries, l2p_page_entry_t) = page_entry;

          page_entry.offset = proto_entry.offset;
          page_entry.sub_item = proto_entry.sub_item;
          APR_ARRAY_IDX(entries, idx, l2p_page_entry_t) = page_entry;
        }
    }

  /* we are now done with the source file */
  SVN_ERR(svn_io_file_close(proto_index, local_pool));

  /* create the target file */
  SVN_ERR(index_create(&index_file, file_name, local_pool));

  /* Paranoia check that makes later casting to int32 safe.
   * The current implementation is limited to 2G pages per index. */
  if (page_counts->nelts > APR_INT32_MAX)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                            _("L2P index page count  %d"
                              " exceeds current limit of 2G pages"),
                            page_counts->nelts);

  /* write header info */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, revision),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_counts->nelts),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, ffd->l2p_page_size),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_sizes->nelts),
                                 NULL, local_pool));

  /* write the revision table */
  end = rle_array(page_counts, 0, page_counts->nelts);
  for (i = 0; i < end; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(page_counts, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }
    
  /* write the page table */
  for (i = 0; i < page_sizes->nelts; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(page_sizes, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
      value = APR_ARRAY_IDX(entry_counts, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }

  /* append page contents */
  SVN_ERR(svn_stream_copy3(svn_stream__from_spillbuf(buffer, local_pool),
                           svn_stream_from_aprfile2(index_file, TRUE,
                                                    local_pool),
                           NULL, NULL, local_pool));

  /* finalize the index file */
  SVN_ERR(svn_io_file_close(index_file, local_pool));
  SVN_ERR(svn_io_set_file_read_only(file_name, FALSE, local_pool));

  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Return the base revision used to identify the p2l or lp2 index covering
 * REVISION in FS.
 */
static svn_revnum_t
base_revision(svn_fs_t *fs, svn_revnum_t revision)
{
  fs_x_data_t *ffd = fs->fsap_data;
  return svn_fs_x__is_packed_rev(fs, revision)
       ? revision - (revision % ffd->max_files_per_dir)
       : revision;
}

/* Data structure that describes which l2p page info shall be extracted
 * from the cache and contains the fields that receive the result.
 */
typedef struct l2p_page_info_baton_t
{
  /* input data: we want the page covering (REVISION,ITEM_INDEX) */
  svn_revnum_t revision;
  apr_uint64_t item_index;

  /* out data */
  /* page location and size of the page within the l2p index file */
  l2p_page_table_entry_t entry;

  /* page number within the pages for REVISION (not l2p index global!) */
  apr_uint32_t page_no;

  /* offset of ITEM_INDEX within that page */
  apr_uint32_t page_offset;

  /* revision identifying the l2p index file, also the first rev in that */
  svn_revnum_t first_revision;
} l2p_page_info_baton_t;


/* Utility function that copies the info requested by BATON->REVISION and
 * BATON->ITEM_INDEX and from HEADER and PAGE_TABLE into the output fields
 * of *BATON.  Use SCRATCH_POOL for temporary allocations.
 */
static svn_error_t *
l2p_header_copy(l2p_page_info_baton_t *baton,
                const l2p_header_t *header,
                const l2p_page_table_entry_t *page_table,
                const apr_size_t *page_table_index,
                apr_pool_t *scratch_pool)
{
  /* revision offset within the index file */
  apr_size_t rel_revision = baton->revision - header->first_revision;
  if (rel_revision >= header->revision_count)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_REVISION , NULL,
                             _("Revision %ld not covered by item index"),
                             baton->revision);

  /* select the relevant page */
  if (baton->item_index < header->page_size)
    {
      /* most revs fit well into a single page */
      baton->page_offset = (apr_uint32_t)baton->item_index;
      baton->page_no = 0;
      baton->entry = page_table[page_table_index[rel_revision]];
    }
  else
    {
      const l2p_page_table_entry_t *first_entry;
      const l2p_page_table_entry_t *last_entry;
      apr_uint64_t max_item_index;

      /* range of pages for this rev */
      first_entry = page_table + page_table_index[rel_revision];
      last_entry = page_table + page_table_index[rel_revision + 1];

      /* do we hit a valid index page? */
      max_item_index =   (apr_uint64_t)header->page_size
                       * (last_entry - first_entry);
      if (baton->item_index >= max_item_index)
        return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                                _("Item index %s exceeds l2p limit "
                                  "of %s for revision %ld"),
                                apr_psprintf(scratch_pool,
                                             "%" APR_UINT64_T_FMT,
                                             baton->item_index),
                                apr_psprintf(scratch_pool,
                                             "%" APR_UINT64_T_FMT,
                                             max_item_index),
                                baton->revision);

      /* all pages are of the same size and full, except for the last one */
      baton->page_offset = (apr_uint32_t)(baton->item_index % header->page_size);
      baton->page_no = (apr_uint32_t)(baton->item_index / header->page_size);
      baton->entry = first_entry[baton->page_no];
    }

  baton->first_revision = header->first_revision;

  return SVN_NO_ERROR;
}

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_page_info_baton_t *BATON from l2p_header_t *DATA into the output
 * fields in *BATON.
 */
static svn_error_t *
l2p_header_access_func(void **out,
                       const void *data,
                       apr_size_t data_len,
                       void *baton,
                       apr_pool_t *result_pool)
{
  /* resolve all pointer values of in-cache data */
  const l2p_header_t *header = data;
  const l2p_page_table_entry_t *page_table
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->page_table);
  const apr_size_t *page_table_index
    = svn_temp_deserializer__ptr(header,
                           (const void *const *)&header->page_table_index);

  /* copy the info */
  return l2p_header_copy(baton, header, page_table, page_table_index,
                         result_pool);
}

/* Read COUNT run-length-encoded (see rle_array) uint64 from STREAM and
 * return them in VALUES.
 */
static svn_error_t *
expand_rle(apr_array_header_t *values,
           packed_number_stream_t *stream,
           apr_size_t count)
{
  apr_array_clear(values);

  while (count)
    {
      apr_uint64_t value;
      SVN_ERR(packed_stream_get(&value, stream));

      if (value)
        {
          APR_ARRAY_PUSH(values, apr_uint64_t) = value;
          --count;
        }
      else
        {
          apr_uint64_t i;
          apr_uint64_t repetitions;
          SVN_ERR(packed_stream_get(&repetitions, stream));
          if (++repetitions > count)
            repetitions = count;

          for (i = 0; i < repetitions; ++i)
            APR_ARRAY_PUSH(values, apr_uint64_t) = 1;

          count -= repetitions;
        }
    }

  return SVN_NO_ERROR;
}

/* Read the header data structure of the log-to-phys index for REVISION
 * in FS and return it in *HEADER.  To maximize efficiency, use or return
 * the data stream in *STREAM.  Use POOL for allocations.
 */
static svn_error_t *
get_l2p_header_body(l2p_header_t **header,
                    packed_number_stream_t **stream,
                    svn_fs_t *fs,
                    svn_revnum_t revision,
                    apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  int i;
  apr_size_t page, page_count;
  apr_off_t offset;
  l2p_header_t *result = apr_pcalloc(pool, sizeof(*result));
  apr_size_t page_table_index;
  apr_array_header_t *expanded_values
    = apr_array_make(pool, 16, sizeof(apr_uint64_t));

  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_x__is_packed_rev(fs, revision);

  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream, 
                               svn_fs_x__path_l2p_index(fs, revision, pool),
                               ffd->block_size, pool));
  else
    packed_stream_seek(*stream, 0);

  /* read the table sizes */
  SVN_ERR(packed_stream_get(&value, *stream));
  result->first_revision = (svn_revnum_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->revision_count = (int)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_size = (apr_uint32_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  page_count = (apr_size_t)value;

  if (result->first_revision > revision
      || result->first_revision + result->revision_count <= revision)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_CORRUPTION, NULL,
                      _("Corrupt L2P index for r%ld only covers r%ld:%ld"),
                      revision, result->first_revision,
                      result->first_revision + result->revision_count);

  /* allocate the page tables */
  result->page_table
    = apr_pcalloc(pool, page_count * sizeof(*result->page_table));
  result->page_table_index
    = apr_pcalloc(pool, (result->revision_count + 1)
                      * sizeof(*result->page_table_index));

  /* read per-revision page table sizes (i.e. number of pages per rev) */
  page_table_index = 0;
  result->page_table_index[0] = page_table_index;
  SVN_ERR(expand_rle(expanded_values, *stream, result->revision_count));
  for (i = 0; i < result->revision_count; ++i)
    {
      page_table_index
        += (apr_size_t)APR_ARRAY_IDX(expanded_values, i, apr_uint64_t);
      result->page_table_index[i+1] = page_table_index;
    }

  /* read actual page tables */
  for (page = 0; page < page_count; ++page)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      result->page_table[page].size = (apr_uint32_t)value;
      SVN_ERR(packed_stream_get(&value, *stream));
      result->page_table[page].entry_count = (apr_uint32_t)value;
    }

  /* correct the page description offsets */
  offset = packed_stream_offset(*stream);
  for (page = 0; page < page_count; ++page)
    {
      result->page_table[page].offset = offset;
      offset += result->page_table[page].size;
    }

  /* return and cache the header */
  *header = result;
  SVN_ERR(svn_cache__set(ffd->l2p_header_cache, &key, result, pool));

  return SVN_NO_ERROR;
}

/* Get the page info requested in *BATON from FS and set the output fields
 * in *BATON.
 * To maximize efficiency, use or return the data stream in *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_l2p_page_info(l2p_page_info_baton_t *baton,
                  packed_number_stream_t **stream,
                  svn_fs_t *fs,
                  apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  l2p_header_t *result;
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* try to find the info in the cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, baton->revision);
  key.second = svn_fs_x__is_packed_rev(fs, baton->revision);
  SVN_ERR(svn_cache__get_partial((void**)&dummy, &is_cached,
                                 ffd->l2p_header_cache, &key,
                                 l2p_header_access_func, baton,
                                 pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* read from disk, cache and copy the result */
  SVN_ERR(get_l2p_header_body(&result, stream, fs, baton->revision, pool));
  SVN_ERR(l2p_header_copy(baton, result, result->page_table,
                          result->page_table_index, pool));

  return SVN_NO_ERROR;
}

/* Read the log-to-phys header info of the index covering REVISION from FS
 * and return it in *HEADER.  To maximize efficiency, use or return the
 * data stream in *STREAM.  Use POOL for allocations.
 */
static svn_error_t *
get_l2p_header(l2p_header_t **header,
               packed_number_stream_t **stream,
               svn_fs_t *fs,
               svn_revnum_t revision,
               apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  svn_boolean_t is_cached = FALSE;

  /* first, try cache lookop */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_x__is_packed_rev(fs, revision);
  SVN_ERR(svn_cache__get((void**)header, &is_cached, ffd->l2p_header_cache,
                         &key, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* read from disk and cache the result */
  SVN_ERR(get_l2p_header_body(header, stream, fs, revision, pool));

  return SVN_NO_ERROR;
}

/* From the log-to-phys index file starting at START_REVISION in FS, read
 * the mapping page identified by TABLE_ENTRY and return it in *PAGE.
 * To maximize efficiency, use or return the data stream in *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_l2p_page(l2p_page_t **page,
             packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t start_revision,
             l2p_page_table_entry_t *table_entry,
             apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_uint64_t value, last_value = 0;
  apr_uint32_t i;
  l2p_page_t *result = apr_pcalloc(pool, sizeof(*result));
  apr_uint64_t container_count;
  apr_off_t *container_offsets;

  /* open index file and select page */
  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream,
                               svn_fs_x__path_l2p_index(fs, start_revision,
                                                        pool),
                               ffd->block_size,
                               pool));

  packed_stream_seek(*stream, table_entry->offset);

  /* initialize the page content */
  result->entry_count = table_entry->entry_count;
  result->offsets = apr_pcalloc(pool, result->entry_count
                                    * sizeof(*result->offsets));
  result->sub_items = apr_pcalloc(pool, result->entry_count
                                      * sizeof(*result->sub_items));

  /* container offsets array */

  SVN_ERR(packed_stream_get(&container_count, *stream));
  container_offsets = apr_pcalloc(pool, container_count * sizeof(*result));
  for (i = 0; i < container_count; ++i)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      last_value += value;
      container_offsets[i] = (apr_off_t)last_value - 1;
      /* '-1' is represented as '0' in the index file */
    }
  
  /* read all page entries (offsets in rev file and container sub-items) */
  for (i = 0; i < result->entry_count; ++i)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      if (value == 0)
        {
          result->offsets[i] = -1;
          result->sub_items[i] = 0;
        }
      else if (value <= container_count)
        {
          result->offsets[i] = container_offsets[value - 1];
          SVN_ERR(packed_stream_get(&value, *stream));
          result->sub_items[i] = (apr_uint32_t)value;
        }
      else
        {
          result->offsets[i] = (apr_off_t)(value - 1 - container_count);
          result->sub_items[i] = 0;
        }
    }

  *page = result;

  return SVN_NO_ERROR;
}

/* Request data structure for l2p_page_access_func.
 */
typedef struct l2p_page_baton_t
{
  /* in data */
  /* revision. Used for error messages only */
  svn_revnum_t revision;

  /* item index to look up. Used for error messages only */
  apr_uint64_t item_index;

  /* offset within the cached page */
  apr_uint32_t page_offset;
  
  /* out data */
  /* absolute item or container offset in rev / pack file */
  apr_off_t offset;

  /* 0 -> container / item itself; sub-item in container otherwise */
  apr_uint32_t sub_item;
 
} l2p_page_baton_t;

/* Return the rev / pack file offset of the item at BATON->PAGE_OFFSET in
 * OFFSETS of PAGE and write it to *OFFSET.
 */
static svn_error_t *
l2p_page_get_offset(l2p_page_baton_t *baton,
                    const l2p_page_t *page,
                    const apr_off_t *offsets,
                    const apr_uint32_t *sub_items,
                    apr_pool_t *pool)
{
  /* overflow check */
  if (page->entry_count <= baton->page_offset)
    return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                             _("Item index %s too large in"
                               " revision %ld"),
                             apr_psprintf(pool, "%" APR_UINT64_T_FMT,
                                          baton->item_index),
                             baton->revision);

  /* return the result */
  baton->offset = offsets[baton->page_offset];
  baton->sub_item = sub_items[baton->page_offset];

  return SVN_NO_ERROR;
}

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_page_baton_t *BATON from l2p_page_t *DATA into apr_off_t *OUT.
 */
static svn_error_t *
l2p_page_access_func(void **out,
                     const void *data,
                     apr_size_t data_len,
                     void *baton,
                     apr_pool_t *result_pool)
{
  /* resolve all in-cache pointers */
  const l2p_page_t *page = data;
  const apr_off_t *offsets
    = svn_temp_deserializer__ptr(page, (const void *const *)&page->offsets);
  const apr_uint32_t *sub_items
    = svn_temp_deserializer__ptr(page, (const void *const *)&page->sub_items);

  /* return the requested data */
  return l2p_page_get_offset(baton, page, offsets, sub_items, result_pool);
}

/* Data request structure used by l2p_page_table_access_func.
 */
typedef struct l2p_page_table_baton_t
{
  /* revision for which to read the page table */
  svn_revnum_t revision;

  /* page table entries (of type l2p_page_table_entry_t).
   * Must be created by caller and will be filled by callee. */
  apr_array_header_t *pages;
} l2p_page_table_baton_t;

/* Implement svn_cache__partial_getter_func_t: copy the data requested in
 * l2p_page_baton_t *BATON from l2p_page_t *DATA into BATON->PAGES and *OUT.
 */
static svn_error_t *
l2p_page_table_access_func(void **out,
                           const void *data,
                           apr_size_t data_len,
                           void *baton,
                           apr_pool_t *result_pool)
{
  /* resolve in-cache pointers */
  l2p_page_table_baton_t *table_baton = baton;
  const l2p_header_t *header = (const l2p_header_t *)data;
  const l2p_page_table_entry_t *page_table
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->page_table);
  const apr_size_t *page_table_index
    = svn_temp_deserializer__ptr(header,
                           (const void *const *)&header->page_table_index);

  /* copy the revision's page table into BATON */
  apr_size_t rel_revision = table_baton->revision - header->first_revision;
  if (rel_revision < header->revision_count)
    {
      const l2p_page_table_entry_t *entry
        = page_table + page_table_index[rel_revision];
      const l2p_page_table_entry_t *last_entry
        = page_table + page_table_index[rel_revision + 1];

      for (; entry < last_entry; ++entry)
        APR_ARRAY_PUSH(table_baton->pages, l2p_page_table_entry_t)
          = *entry;
    }

  /* set output as a courtesy to the caller */
  *out = table_baton->pages;
  
  return SVN_NO_ERROR;
}

/* Read the l2p index page table for REVISION in FS from cache and return
 * it in PAGES.  The later must be provided by the caller (and can be
 * re-used); existing entries will be removed before writing the result.
 * If the data cannot be found in the cache, the result will be empty
 * (it never can be empty for a valid REVISION if the data is cached).
 * Use the info from REV_FILE to determine pack / rev file properties.
 * Use POOL for temporary allocations.
 */
static svn_error_t *
get_l2p_page_table(apr_array_header_t *pages,
                   svn_fs_t *fs,
                   svn_revnum_t revision,
                   apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  svn_boolean_t is_cached = FALSE;
  l2p_page_table_baton_t baton;

  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_x__is_packed_rev(fs, revision);

  apr_array_clear(pages);
  baton.revision = revision;
  baton.pages = pages;
  SVN_ERR(svn_cache__get_partial((void**)&pages, &is_cached,
                                 ffd->l2p_header_cache, &key,
                                 l2p_page_table_access_func, &baton, pool));

  return SVN_NO_ERROR;
}

/* Utility function.  Read the l2p index pages for REVISION in FS from
 * STREAM and put them into the cache.  Skip page number EXLCUDED_PAGE_NO
 * (use -1 for 'skip none') and pages outside the MIN_OFFSET, MAX_OFFSET
 * range in the l2p index file.  The index is being identified by
 * FIRST_REVISION.  PAGES is a scratch container provided by the caller.
 * SCRATCH_POOL is used for temporary allocations.
 *
 * This function may be a no-op if the header cache lookup fails / misses.
 */
static svn_error_t *
prefetch_l2p_pages(svn_boolean_t *end,
                   svn_fs_t *fs,
                   packed_number_stream_t *stream,
                   svn_revnum_t first_revision,
                   svn_revnum_t revision,
                   apr_array_header_t *pages,
                   int exlcuded_page_no,
                   apr_off_t min_offset,
                   apr_off_t max_offset,
                   apr_pool_t *scratch_pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  int i;
  apr_pool_t *iterpool;
  svn_fs_x__page_cache_key_t key = { 0 };

  /* get the page table for REVISION from cache */
  *end = FALSE;
  SVN_ERR(get_l2p_page_table(pages, fs, revision, scratch_pool));
  if (pages->nelts == 0)
    {
      /* not found -> we can't continue without hitting the disk again */
      *end = TRUE;
      return SVN_NO_ERROR;
    }

  /* prefetch pages individually until all are done or we found one in
   * the cache */
  iterpool = svn_pool_create(scratch_pool);
  assert(revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)revision;
  key.is_packed = svn_fs_x__is_packed_rev(fs, revision);

  for (i = 0; i < pages->nelts && !*end; ++i)
    {
      svn_boolean_t is_cached;

      l2p_page_table_entry_t *entry
        = &APR_ARRAY_IDX(pages, i, l2p_page_table_entry_t);
      svn_pool_clear(iterpool);

      if (i == exlcuded_page_no)
        continue;

      /* skip pages outside the specified index file range */
      if (   entry->offset < min_offset
          || entry->offset + entry->size > max_offset)
        {
          *end = TRUE;
          continue;
        }

      /* page already in cache? */
      key.page = i;
      SVN_ERR(svn_cache__has_key(&is_cached, ffd->l2p_page_cache,
                                 &key, iterpool));
      if (!is_cached)
        {
          /* no in cache -> read from stream (data already buffered in APR)
           * and cache the result */
          l2p_page_t *page = NULL;
          SVN_ERR(get_l2p_page(&page, &stream, fs, first_revision,
                               entry, iterpool));

          SVN_ERR(svn_cache__set(ffd->l2p_page_cache, &key, page,
                                 iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Using the log-to-phys indexes in FS, find the absolute offset in the
 * rev file for (REVISION, ITEM_INDEX) and return it in *OFFSET.
 * Use POOL for allocations.
 */
static svn_error_t *
l2p_index_lookup(apr_off_t *offset,
                 apr_uint32_t *sub_item,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_uint64_t item_index,
                 apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  l2p_page_info_baton_t info_baton;
  l2p_page_baton_t page_baton;
  l2p_page_t *page = NULL;
  packed_number_stream_t *stream = NULL;
  svn_fs_x__page_cache_key_t key = { 0 };
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* read index master data structure and extract the info required to
   * access the l2p index page for (REVISION,ITEM_INDEX)*/
  info_baton.revision = revision;
  info_baton.item_index = item_index;
  SVN_ERR(get_l2p_page_info(&info_baton, &stream, fs, pool));

  /* try to find the page in the cache and get the OFFSET from it */
  page_baton.revision = revision;
  page_baton.item_index = item_index;
  page_baton.page_offset = info_baton.page_offset;

  assert(revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)revision;
  key.is_packed = svn_fs_x__is_packed_rev(fs, revision);
  key.page = info_baton.page_no;

  SVN_ERR(svn_cache__get_partial(&dummy, &is_cached,
                                 ffd->l2p_page_cache, &key,
                                 l2p_page_access_func, &page_baton, pool));

  if (!is_cached)
    {
      /* we need to read the info from disk (might already be in the
       * APR file buffer, though) */
      apr_array_header_t *pages;
      svn_revnum_t prefetch_revision;
      svn_revnum_t last_revision
        = info_baton.first_revision
          + (key.is_packed ? ffd->max_files_per_dir : 1);
      apr_pool_t *iterpool = svn_pool_create(pool);
      svn_boolean_t end;
      apr_off_t max_offset
        = APR_ALIGN(info_baton.entry.offset + info_baton.entry.size,
                    ffd->block_size);
      apr_off_t min_offset = max_offset - ffd->block_size;

      /* read the relevant page */
      SVN_ERR(get_l2p_page(&page, &stream, fs, info_baton.first_revision,
                           &info_baton.entry, pool));

      /* cache the page and extract the result we need */
      SVN_ERR(svn_cache__set(ffd->l2p_page_cache, &key, page, pool));
      SVN_ERR(l2p_page_get_offset(&page_baton, page, page->offsets,
                                  page->sub_items, pool));

      /* prefetch pages from following and preceding revisions */
      pages = apr_array_make(pool, 16, sizeof(l2p_page_table_entry_t));
      end = FALSE;
      for (prefetch_revision = revision;
           prefetch_revision < last_revision && !end;
           ++prefetch_revision)
        {
          int excluded_page_no = prefetch_revision == revision
                               ? info_baton.page_no
                               : -1;
          svn_pool_clear(iterpool);

          SVN_ERR(prefetch_l2p_pages(&end, fs, stream,
                                     info_baton.first_revision,
                                     prefetch_revision, pages,
                                     excluded_page_no, min_offset,
                                     max_offset, iterpool));
        }

      end = FALSE;
      for (prefetch_revision = revision-1;
           prefetch_revision >= info_baton.first_revision && !end;
           --prefetch_revision)
        {
          svn_pool_clear(iterpool);

          SVN_ERR(prefetch_l2p_pages(&end, fs, stream,
                                     info_baton.first_revision,
                                     prefetch_revision, pages, -1,
                                     min_offset, max_offset, iterpool));
        }

      svn_pool_destroy(iterpool);
    }

  SVN_ERR(packed_stream_close(stream));

  *offset = page_baton.offset;
  *sub_item = page_baton.sub_item;

  return SVN_NO_ERROR;
}

/* Using the log-to-phys proto index in transaction TXN_ID in FS, find the
 * absolute offset in the proto rev file for the given ITEM_INDEX and return
 * it in *OFFSET.  Use POOL for allocations.
 */
static svn_error_t *
l2p_proto_index_lookup(apr_off_t *offset,
                       apr_uint32_t *sub_item,
                       svn_fs_t *fs,
                       svn_fs_x__txn_id_t txn_id,
                       apr_uint64_t item_index,
                       apr_pool_t *pool)
{
  svn_boolean_t eof = FALSE;
  apr_file_t *file = NULL;
  SVN_ERR(svn_io_file_open(&file,
                           svn_fs_x__path_l2p_proto_index(fs, txn_id, pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));

  /* process all entries until we fail due to EOF */
  *offset = -1;
  while (!eof)
    {
      l2p_proto_entry_t entry;
      apr_size_t read = 0;

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(file, &entry, sizeof(entry),
                                     &read, &eof, pool));
      SVN_ERR_ASSERT(eof || read == sizeof(entry));

      /* handle new revision */
      if (!eof && entry.item_index == item_index)
        {
          *offset = (apr_off_t)entry.offset - 1;
          *sub_item = entry.sub_item;
          break;
        }
    }

  SVN_ERR(svn_io_file_close(file, pool));
  
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__l2p_get_max_ids(apr_array_header_t **max_ids,
                          svn_fs_t *fs,
                          svn_revnum_t start_rev,
                          apr_size_t count,
                          apr_pool_t *pool)
{
  l2p_header_t *header = NULL;
  svn_revnum_t revision;
  svn_revnum_t last_rev = (svn_revnum_t)(start_rev + count);
  packed_number_stream_t *stream = NULL;
  apr_pool_t *header_pool = svn_pool_create(pool);

  /* read index master data structure for the index covering START_REV */
  SVN_ERR(get_l2p_header(&header, &stream, fs, start_rev, header_pool));
  SVN_ERR(packed_stream_close(stream));
  stream = NULL;

  /* Determine the length of the item index list for each rev.
   * Read new index headers as required. */
  *max_ids = apr_array_make(pool, (int)count, sizeof(apr_uint64_t));
  for (revision = start_rev; revision < last_rev; ++revision)
    {
      apr_uint64_t full_page_count;
      apr_uint64_t item_count;
      apr_size_t first_page_index, last_page_index;

      if (revision >= header->first_revision + header->revision_count)
        {
          /* need to read the next index. Clear up memory used for the
           * previous one.  Note that intermittent pack runs do not change
           * the number of items in a revision, i.e. there is no consistency
           * issue here. */
          svn_pool_clear(header_pool);
          SVN_ERR(get_l2p_header(&header, &stream, fs, revision,
                                 header_pool));
          SVN_ERR(packed_stream_close(stream));
          stream = NULL;
        }

      /* in a revision with N index pages, the first N-1 index pages are
       * "full", i.e. contain HEADER->PAGE_SIZE entries */
      first_page_index
         = header->page_table_index[revision - header->first_revision];
      last_page_index
         = header->page_table_index[revision - header->first_revision + 1];
      full_page_count = last_page_index - first_page_index - 1;
      item_count = full_page_count * header->page_size
                 + header->page_table[last_page_index - 1].entry_count;

      APR_ARRAY_PUSH(*max_ids, apr_uint64_t) = item_count;
    }

  svn_pool_destroy(header_pool);
  return SVN_NO_ERROR;
}

/*
 * phys-to-log index
 */
svn_fs_x__p2l_entry_t *
svn_fs_x__p2l_entry_dup(const svn_fs_x__p2l_entry_t *entry,
                        apr_pool_t *pool)
{
  svn_fs_x__p2l_entry_t *new_entry = apr_palloc(pool, sizeof(*new_entry));
  *new_entry = *entry;

  if (new_entry->item_count)
    new_entry->items = apr_pmemdup(pool,
                                   entry->items,
                                   entry->item_count * sizeof(*entry->items));

  return new_entry;
}

/*
 * phys-to-log index
 */
svn_error_t *
svn_fs_x__p2l_proto_index_open(apr_file_t **proto_index,
                               const char *file_name,
                               apr_pool_t *pool)
{
  SVN_ERR(svn_io_file_open(proto_index, file_name, APR_READ | APR_WRITE
                           | APR_CREATE | APR_APPEND | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_x__p2l_proto_index_add_entry(apr_file_t *proto_index,
                                    svn_fs_x__p2l_entry_t *entry,
                                    apr_pool_t *pool)
{
  apr_size_t written = sizeof(*entry);
  apr_size_t written_total = 0;

  /* Write main record. */
  SVN_ERR(svn_io_file_write_full(proto_index, entry, sizeof(*entry),
                                 &written, pool));
  SVN_ERR_ASSERT(written == sizeof(*entry));
  written_total += written;

  /* Add sub-items. */
  if (entry->item_count)
    {
      written = entry->item_count * sizeof(*entry->items);
      SVN_ERR(svn_io_file_write_full(proto_index, entry->items, written,
                                     &written, pool));
      SVN_ERR_ASSERT(written == entry->item_count * sizeof(*entry->items));
      written_total += written;
    }

  /* Add trailer: number of bytes total in this entr.y */
  written = sizeof(written_total);
  SVN_ERR(svn_io_file_write_full(proto_index, &written_total, written,
                                 &written, pool));
  SVN_ERR_ASSERT(written == sizeof(written_total));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_proto_index_next_offset(apr_off_t *next_offset,
                                      apr_file_t *proto_index,
                                      apr_pool_t *pool)
{
  apr_off_t offset = 0;

  /* Empty index file? */
  SVN_ERR(svn_io_file_seek(proto_index, APR_END, &offset, pool));
  if (offset == 0)
    {
      *next_offset = 0;
    }
  else
    {
      /* At least one entry.  Read last entry. */
      apr_size_t size;
      svn_fs_x__p2l_entry_t entry;

      /* Read length of last entry. */
      offset -= sizeof(size);
      SVN_ERR(svn_io_file_seek(proto_index, APR_SET, &offset, pool));
      SVN_ERR(svn_io_file_read_full2(proto_index, &size, sizeof(size),
                                    NULL, NULL, pool));

      /* Read last entry's main record. */
      offset -= size;
      SVN_ERR(svn_io_file_seek(proto_index, APR_SET, &offset, pool));
      SVN_ERR(svn_io_file_read_full2(proto_index, &entry, sizeof(entry),
                                    NULL, NULL, pool));

      /* Return next offset. */
      *next_offset = entry.offset + entry.size;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_index_create(svn_fs_t *fs,
                           const char *file_name,
                           const char *proto_file_name,
                           svn_revnum_t revision,
                           apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_uint64_t page_size = ffd->p2l_page_size;
  apr_file_t *proto_index = NULL;
  int i;
  apr_uint32_t sub_item;
  svn_boolean_t eof = FALSE;
  apr_file_t *index_file;
  unsigned char encoded[ENCODED_INT_LENGTH];

  apr_uint64_t last_entry_end = 0;
  apr_uint64_t last_page_end = 0;
  apr_size_t last_buffer_size = 0;  /* byte offset in the spill buffer at
                                       the begin of the current revision */
  apr_uint64_t file_size = 0;

  /* temporary data structures that collect the data which will be moved
     to the target file in a second step */
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_array_header_t *table_sizes
     = apr_array_make(local_pool, 16, sizeof(apr_uint64_t));

  /* 64k blocks, spill after 16MB */
  svn_spillbuf_t *buffer
     = svn_spillbuf__create(0x10000, 0x1000000, local_pool);

  /* for loop temps ... */
  apr_pool_t *iter_pool = svn_pool_create(pool);

  /* start at the beginning of the source file */
  SVN_ERR(svn_io_file_open(&proto_index, proto_file_name,
                           APR_READ | APR_CREATE | APR_BUFFERED,
                           APR_OS_DEFAULT, local_pool));

  /* process all entries until we fail due to EOF */
  while (!eof)
    {
      svn_fs_x__p2l_entry_t entry;
      apr_size_t read = 0;
      apr_size_t to_read;
      apr_uint64_t entry_end;
      svn_boolean_t new_page = svn_spillbuf__get_size(buffer) == 0;
      svn_revnum_t last_revision = revision;
      apr_uint64_t last_number = 0;

      svn_pool_clear(iter_pool);

      /* (attempt to) read the next entry from the source */
      SVN_ERR(svn_io_file_read_full2(proto_index, &entry, sizeof(entry),
                                     &read, &eof, iter_pool));
      SVN_ERR_ASSERT(eof || read == sizeof(entry));

      if (entry.item_count && !eof)
        {
          to_read = entry.item_count * sizeof(*entry.items);
          entry.items = apr_palloc(iter_pool, to_read);

          SVN_ERR(svn_io_file_read_full2(proto_index, entry.items, to_read,
                                         &read, &eof, iter_pool));
          SVN_ERR_ASSERT(eof || read == to_read);
        }

      /* Read entry trailer. However, we won't need its content. */
      if (!eof)
        {
          apr_size_t entry_size;
          to_read = sizeof(entry_size);
          SVN_ERR(svn_io_file_read_full2(proto_index, &entry_size, to_read,
                                         &read, &eof, iter_pool));
          SVN_ERR_ASSERT(eof || read == to_read);
        }

      /* "unused" (and usually non-existent) section to cover the offsets
         at the end the of the last page. */
      if (eof)
        {
          file_size = last_entry_end;

          entry.offset = last_entry_end;
          entry.size = APR_ALIGN(entry.offset, page_size) - entry.offset;
          entry.type = 0;
          entry.item_count = 0;
          entry.items = NULL;
        }

      for (sub_item = 0; sub_item < entry.item_count; ++sub_item)
        if (entry.items[sub_item].change_set == SVN_FS_X__INVALID_CHANGE_SET)
          entry.items[sub_item].change_set
            = svn_fs_x__change_set_by_rev(revision);

      /* end pages if entry is extending beyond their boundaries */
      entry_end = entry.offset + entry.size;
      while (entry_end - last_page_end > page_size)
        {
          apr_uint64_t buffer_size = svn_spillbuf__get_size(buffer);
          APR_ARRAY_PUSH(table_sizes, apr_uint64_t)
             = buffer_size - last_buffer_size;

          last_buffer_size = buffer_size;
          last_page_end += page_size;
          new_page = TRUE;
        }

      /* this entry starts a new table -> store its offset
         (all following entries in the same table will store sizes only) */
      if (new_page)
        {
          SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                      encode_uint(encoded, entry.offset),
                                      iter_pool));
          last_revision = revision;
        }

      /* write simple item / container entry */
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_uint(encoded, entry.size),
                                  iter_pool));
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_uint(encoded, entry.type + entry.item_count * 16),
                                  iter_pool));
      SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                  encode_uint(encoded, entry.fnv1_checksum),
                                  iter_pool));

      /* container contents (only one for non-container items) */
      for (sub_item = 0; sub_item < entry.item_count; ++sub_item)
        {
          svn_revnum_t item_rev
            = svn_fs_x__get_revnum(entry.items[sub_item].change_set);
          apr_int64_t diff = item_rev - last_revision;
          SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                      encode_int(encoded, diff),
                                      iter_pool));
          last_revision = item_rev;
        }

      for (sub_item = 0; sub_item < entry.item_count; ++sub_item)
        {
          apr_int64_t diff = entry.items[sub_item].number - last_number;
          SVN_ERR(svn_spillbuf__write(buffer, (const char *)encoded,
                                      encode_int(encoded, diff),
                                      iter_pool));
          last_number = entry.items[sub_item].number;
        }

      last_entry_end = entry_end;
    }

  /* close the source file */
  SVN_ERR(svn_io_file_close(proto_index, local_pool));

  /* store length of last table */
  APR_ARRAY_PUSH(table_sizes, apr_uint64_t)
      = svn_spillbuf__get_size(buffer) - last_buffer_size;

  /* create the target file */
  SVN_ERR(index_create(&index_file, file_name, local_pool));

  /* write the start revision, file size and page size */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, revision),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, file_size),
                                 NULL, local_pool));
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, page_size),
                                 NULL, local_pool));

  /* write the page table (actually, the sizes of each page description) */
  SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                 encode_uint(encoded, table_sizes->nelts),
                                 NULL, local_pool));
  for (i = 0; i < table_sizes->nelts; ++i)
    {
      apr_uint64_t value = APR_ARRAY_IDX(table_sizes, i, apr_uint64_t);
      SVN_ERR(svn_io_file_write_full(index_file, encoded,
                                     encode_uint(encoded, value),
                                     NULL, local_pool));
    }

  /* append page contents */
  SVN_ERR(svn_stream_copy3(svn_stream__from_spillbuf(buffer, local_pool),
                           svn_stream_from_aprfile2(index_file, TRUE,
                                                    local_pool),
                           NULL, NULL, local_pool));

  /* finalize the index file */
  SVN_ERR(svn_io_file_close(index_file, local_pool));
  SVN_ERR(svn_io_set_file_read_only(file_name, FALSE, local_pool));

  svn_pool_destroy(iter_pool);
  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Data structure that describes which p2l page info shall be extracted
 * from the cache and contains the fields that receive the result.
 */
typedef struct p2l_page_info_baton_t
{
  /* input variables */
  /* revision identifying the index file */
  svn_revnum_t revision;

  /* offset within the page in rev / pack file */
  apr_off_t offset;

  /* output variables */
  /* page containing OFFSET */
  apr_size_t page_no;

  /* first revision in this p2l index */
  svn_revnum_t first_revision;

  /* offset within the p2l index file describing this page */
  apr_off_t start_offset;

  /* offset within the p2l index file describing the following page */
  apr_off_t next_offset;

  /* PAGE_NO * PAGE_SIZE (is <= OFFSET) */
  apr_off_t page_start;

  /* total number of pages indexed */
  apr_size_t page_count;

  /* size of each page in pack / rev file */
  apr_uint64_t page_size;
} p2l_page_info_baton_t;

/* From HEADER and the list of all OFFSETS, fill BATON with the page info
 * requested by BATON->OFFSET.
 */
static void
p2l_page_info_copy(p2l_page_info_baton_t *baton,
                   const p2l_header_t *header,
                   const apr_off_t *offsets)
{
  /* if the requested offset is out of bounds, return info for 
   * a zero-sized empty page right behind the last page.
   */
  if (baton->offset / header->page_size < header->page_count)
    {
      baton->page_no = baton->offset / header->page_size;
      baton->start_offset = offsets[baton->page_no];
      baton->next_offset = offsets[baton->page_no + 1];
      baton->page_size = header->page_size;
    }
  else
    {
      baton->page_no = header->page_count;
      baton->start_offset = offsets[baton->page_no];
      baton->next_offset = offsets[baton->page_no];
      baton->page_size = 0;
    }

  baton->first_revision = header->first_revision;
  baton->page_start = (apr_off_t)(header->page_size * baton->page_no);
  baton->page_count = header->page_count;
}

/* Implement svn_cache__partial_getter_func_t: extract the p2l page info
 * requested by BATON and return it in BATON.
 */
static svn_error_t *
p2l_page_info_func(void **out,
                   const void *data,
                   apr_size_t data_len,
                   void *baton,
                   apr_pool_t *result_pool)
{
  /* all the pointers to cached data we need */
  const p2l_header_t *header = data;
  const apr_off_t *offsets
    = svn_temp_deserializer__ptr(header,
                                 (const void *const *)&header->offsets);

  /* copy data from cache to BATON */
  p2l_page_info_copy(baton, header, offsets);
  return SVN_NO_ERROR;
}

/* Read the header data structure of the phys-to-log index for REVISION in
 * FS and return it in *HEADER. 
 * 
 * To maximize efficiency, use or return the data stream in *STREAM.
 * If *STREAM is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_header(p2l_header_t **header,
               packed_number_stream_t **stream,
               svn_fs_t *fs,
               svn_revnum_t revision,
               apr_pool_t *stream_pool,
               apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  apr_size_t i;
  apr_off_t offset;
  p2l_header_t *result;
  svn_boolean_t is_cached = FALSE;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_x__is_packed_rev(fs, revision);

  SVN_ERR(svn_cache__get((void**)header, &is_cached, ffd->p2l_header_cache,
                         &key, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  /* not found -> must read it from disk.
   * Open index file or position read pointer to the begin of the file */
  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream,
                               svn_fs_x__path_p2l_index(fs, key.revision,
                                                        pool),
                               ffd->block_size, stream_pool));
  else
    packed_stream_seek(*stream, 0);

  /* allocate result data structure */
  result = apr_pcalloc(pool, sizeof(*result));
  
  /* read table sizes and allocate page array */
  SVN_ERR(packed_stream_get(&value, *stream));
  result->first_revision = (svn_revnum_t)value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->file_size = value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_size = value;
  SVN_ERR(packed_stream_get(&value, *stream));
  result->page_count = (apr_size_t)value;
  result->offsets
    = apr_pcalloc(pool, (result->page_count + 1) * sizeof(*result->offsets));

  /* read page sizes and derive page description offsets from them */
  result->offsets[0] = 0;
  for (i = 0; i < result->page_count; ++i)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      result->offsets[i+1] = result->offsets[i] + (apr_off_t)value;
    }

  /* correct the offset values */
  offset = packed_stream_offset(*stream);
  for (i = 0; i <= result->page_count; ++i)
    result->offsets[i] += offset;

  /* cache the header data */
  SVN_ERR(svn_cache__set(ffd->p2l_header_cache, &key, result, pool));

  /* return the result */
  *header = result;

  return SVN_NO_ERROR;
}

/* Read the header data structure of the phys-to-log index for revision
 * BATON->REVISION in FS.  Return in *BATON all info relevant to read the
 * index page for the rev / pack file offset BATON->OFFSET.
 * 
 * To maximize efficiency, use or return the data stream in *STREAM.
 * If *STREAM is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_page_info(p2l_page_info_baton_t *baton,
                  packed_number_stream_t **stream,
                  svn_fs_t *fs,
                  apr_pool_t *stream_pool,
                  apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  p2l_header_t *header;
  svn_boolean_t is_cached = FALSE;
  void *dummy = NULL;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, baton->revision);
  key.second = svn_fs_x__is_packed_rev(fs, baton->revision);

  SVN_ERR(svn_cache__get_partial(&dummy, &is_cached, ffd->p2l_header_cache,
                                 &key, p2l_page_info_func, baton, pool));
  if (is_cached)
    return SVN_NO_ERROR;

  SVN_ERR(get_p2l_header(&header, stream, fs, baton->revision,
                         stream_pool, pool));

  /* copy the requested info into *BATON */
  p2l_page_info_copy(baton, header, header->offsets);

  return SVN_NO_ERROR;
}

/* Read a mapping entry from the phys-to-log index STREAM and append it to
 * RESULT.  *ITEM_INDEX contains the phys offset for the entry and will
 * be moved forward by the size of entry.  Use POOL for allocations.
 */
static svn_error_t *
read_entry(packed_number_stream_t *stream,
           apr_off_t *item_offset,
           svn_revnum_t revision,
           apr_array_header_t *result,
           apr_pool_t *pool)
{
  apr_uint64_t value;
  apr_uint64_t number = 0;
  apr_uint32_t sub_item;

  svn_fs_x__p2l_entry_t entry;

  entry.offset = *item_offset;
  SVN_ERR(packed_stream_get(&value, stream));
  entry.size = (apr_off_t)value;
  SVN_ERR(packed_stream_get(&value, stream));
  entry.type = (int)value % 16;
  entry.item_count = (apr_uint32_t)(value / 16);
  SVN_ERR(packed_stream_get(&value, stream));
  entry.fnv1_checksum = (apr_uint32_t)value;

  if (entry.item_count == 0)
    {
      entry.items = NULL;
    }
  else
    {
      entry.items
        = apr_pcalloc(pool, entry.item_count * sizeof(*entry.items));

      for (sub_item = 0; sub_item < entry.item_count; ++sub_item)
        {
          SVN_ERR(packed_stream_get(&value, stream));
          revision += (svn_revnum_t)(value % 2 ? -1 - value / 2 : value / 2);
          entry.items[sub_item].change_set
            = svn_fs_x__change_set_by_rev(revision);
        }

      for (sub_item = 0; sub_item < entry.item_count; ++sub_item)
        {
          SVN_ERR(packed_stream_get(&value, stream));
          number += value % 2 ? -1 - value / 2 : value / 2;
          entry.items[sub_item].number = number;
        }
    }

  APR_ARRAY_PUSH(result, svn_fs_x__p2l_entry_t) = entry;
  *item_offset += entry.size;

  return SVN_NO_ERROR;
}

/* Read the phys-to-log mappings for the cluster beginning at rev file
 * offset PAGE_START from the index for START_REVISION in FS.  The data
 * can be found in the index page beginning at START_OFFSET with the next
 * page beginning at NEXT_OFFSET.  Return the relevant index entries in
 * *ENTRIES.  To maximize efficiency, use or return the data stream in
 * STREAM.  If the latter is yet to be constructed, do so in STREAM_POOL.
 * Use POOL for other allocations.
 */
static svn_error_t *
get_p2l_page(apr_array_header_t **entries,
             packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t start_revision,
             apr_off_t start_offset,
             apr_off_t next_offset,
             apr_off_t page_start,
             apr_uint64_t page_size,
             apr_pool_t *stream_pool,
             apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  apr_uint64_t value;
  apr_array_header_t *result
    = apr_array_make(pool, 16, sizeof(svn_fs_x__p2l_entry_t));
  apr_off_t item_offset;
  apr_off_t offset;

  /* open index and navigate to page start */
  if (*stream == NULL)
    SVN_ERR(packed_stream_open(stream,
                        svn_fs_x__path_p2l_index(fs, start_revision, pool),
                        ffd->block_size, stream_pool));
  packed_stream_seek(*stream, start_offset);

  /* read rev file offset of the first page entry (all page entries will
   * only store their sizes). */
  SVN_ERR(packed_stream_get(&value, *stream));
  item_offset = (apr_off_t)value;

  /* read all entries of this page */
  do
    {
      SVN_ERR(read_entry(*stream, &item_offset, start_revision, result,
                         pool));
      offset = packed_stream_offset(*stream);
    }
  while (offset < next_offset);

  /* if we haven't covered the cluster end yet, we must read the first
   * entry of the next page */
  if (item_offset < page_start + page_size)
    {
      SVN_ERR(packed_stream_get(&value, *stream));
      item_offset = (apr_off_t)value;
      SVN_ERR(read_entry(*stream, &item_offset, start_revision, result,
                         pool));
    }

  *entries = result;

  return SVN_NO_ERROR;
}

/* If it cannot be found in FS's caches, read the p2l index page selected
 * by BATON->OFFSET from *STREAM.  If the latter is yet to be constructed,
 * do so in STREAM_POOL.  Don't read the page if it precedes MIN_OFFSET.
 * Set *END to TRUE if the caller should stop refeching.
 *
 * *BATON will be updated with the selected page's info and SCRATCH_POOL
 * will be used for temporary allocations.  If the data is alread in the
 * cache, descrease *LEAKING_BUCKET and increase it otherwise.  With that
 * pattern we will still read all pages from the block even if some of
 * them survived in the cached.
 */
static svn_error_t *
prefetch_p2l_page(svn_boolean_t *end,
                  int *leaking_bucket,
                  svn_fs_t *fs,
                  packed_number_stream_t **stream,
                  p2l_page_info_baton_t *baton,
                  apr_off_t min_offset,
                  apr_pool_t *stream_pool,
                  apr_pool_t *scratch_pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  svn_boolean_t already_cached;
  apr_array_header_t *page;
  svn_fs_x__page_cache_key_t key = { 0 };

  /* fetch the page info */
  *end = FALSE;
  baton->revision = baton->first_revision;
  SVN_ERR(get_p2l_page_info(baton, stream, fs, stream_pool, scratch_pool));
  if (baton->start_offset < min_offset)
    {
      /* page outside limits -> stop prefetching */
      *end = TRUE;
      return SVN_NO_ERROR;
    }

  /* do we have that page in our caches already? */
  assert(baton->first_revision <= APR_UINT32_MAX);
  key.revision = (apr_uint32_t)baton->first_revision;
  key.is_packed = svn_fs_x__is_packed_rev(fs, baton->first_revision);
  key.page = baton->page_no;
  SVN_ERR(svn_cache__has_key(&already_cached, ffd->p2l_page_cache,
                             &key, scratch_pool));

  /* yes, already cached */
  if (already_cached)
    {
      /* stop prefetching if most pages are already cached. */
      if (!--*leaking_bucket)
        *end = TRUE;

      return SVN_NO_ERROR;
    }

  ++*leaking_bucket;

  /* read from disk */
  SVN_ERR(get_p2l_page(&page, stream, fs,
                       baton->first_revision,
                       baton->start_offset,
                       baton->next_offset,
                       baton->page_start,
                       baton->page_size,
                       stream_pool,
                       scratch_pool));

  /* and put it into our cache */
  SVN_ERR(svn_cache__set(ffd->p2l_page_cache, &key, page, scratch_pool));

  return SVN_NO_ERROR;
}

/* Lookup & construct the baton and key information that we will need for
 * a P2L page cache lookup.  We want the page covering OFFSET in the rev /
 * pack file containing REVSION in FS.  Return the results in *PAGE_INFO_P
 * and *KEY_P.  Read data through the auto-allocated *STREAM.
 * Use POOL for allocations.
 */
static svn_error_t *
get_p2l_keys(p2l_page_info_baton_t *page_info_p,
             svn_fs_x__page_cache_key_t *key_p,
             packed_number_stream_t **stream,
             svn_fs_t *fs,
             svn_revnum_t revision,
             apr_off_t offset,
             apr_pool_t *pool)
{
  p2l_page_info_baton_t page_info;
  
  /* request info for the index pages that describes the pack / rev file
   * contents at pack / rev file position OFFSET. */
  page_info.offset = offset;
  page_info.revision = revision;
  SVN_ERR(get_p2l_page_info(&page_info, stream, fs, pool, pool));

  /* if the offset refers to a non-existent page, bail out */
  if (page_info.page_count <= page_info.page_no)
    {
      SVN_ERR(packed_stream_close(*stream));
      return svn_error_createf(SVN_ERR_FS_ITEM_INDEX_OVERFLOW , NULL,
                               _("Offset %s too large in revision %ld"),
                               apr_off_t_toa(pool, offset), revision);
    }

  /* return results */
  if (page_info_p)
    *page_info_p = page_info;
  
  /* construct cache key */
  if (key_p)
    {
      svn_fs_x__page_cache_key_t key = { 0 };
      assert(page_info.first_revision <= APR_UINT32_MAX);
      key.revision = (apr_uint32_t)page_info.first_revision;
      key.is_packed = svn_fs_x__is_packed_rev(fs, revision);
      key.page = page_info.page_no;

      *key_p = key;  
    }

  return SVN_NO_ERROR;
}

/* qsort-compatible compare function that compares the OFFSET of the
 * svn_fs_x__p2l_entry_t in *LHS with the apr_off_t in *RHS. */
static int
compare_start_p2l_entry(const void *lhs,
                        const void *rhs)
{
  const svn_fs_x__p2l_entry_t *entry = lhs;
  apr_off_t start = *(const apr_off_t*)rhs;
  apr_off_t diff = entry->offset - start;

  /* restrict result to int */
  return diff < 0 ? -1 : (diff == 0 ? 0 : 1);
}

/* From the PAGE_ENTRIES array of svn_fs_x__p2l_entry_t, ordered
 * by their OFFSET member, copy all elements overlapping the range
 * [BLOCK_START, BLOCK_END) to ENTRIES.  If RESOLVE_PTR is set, the ITEMS
 * sub-array in each entry needs to be de-serialized. */
static void
append_p2l_entries(apr_array_header_t *entries,
                   apr_array_header_t *page_entries,
                   apr_off_t block_start,
                   apr_off_t block_end,
                   svn_boolean_t resolve_ptr)
{
  const svn_fs_x__p2l_entry_t *entry;
  int idx = svn_sort__bsearch_lower_bound(page_entries, &block_start,
                                          compare_start_p2l_entry);

  /* start at the first entry that overlaps with BLOCK_START */
  if (idx > 0)
    {
      entry = &APR_ARRAY_IDX(page_entries, idx - 1, svn_fs_x__p2l_entry_t);
      if (entry->offset + entry->size > block_start)
        --idx;
    }

  /* copy all entries covering the requested range */
  for ( ; idx < page_entries->nelts; ++idx)
    {
      svn_fs_x__p2l_entry_t *copy;
      entry = &APR_ARRAY_IDX(page_entries, idx, svn_fs_x__p2l_entry_t);
      if (entry->offset >= block_end)
        break;

      /* Copy the entry record. */
      copy = apr_array_push(entries);
      *copy = *entry;

      /* Copy the items of that entries. */
      if (entry->item_count)
        {
          const svn_fs_x__id_part_t *items
            = resolve_ptr
            ? svn_temp_deserializer__ptr(page_entries->elts,
                                         (const void * const *)&entry->items)
            : entry->items;

          copy->items = apr_pmemdup(entries->pool, items,
                                    entry->item_count * sizeof(*items));
        }
    }
}

/* Auxilliary struct passed to p2l_entries_func selecting the relevant
 * data range. */
typedef struct p2l_entries_baton_t
{
  apr_off_t start;
  apr_off_t end;
} p2l_entries_baton_t;

/* Implement svn_cache__partial_getter_func_t: extract p2l entries from
 * the page in DATA which overlap the p2l_entries_baton_t in BATON.
 * The target array is already provided in *OUT.
 */
static svn_error_t *
p2l_entries_func(void **out,
                 const void *data,
                 apr_size_t data_len,
                 void *baton,
                 apr_pool_t *result_pool)
{
  apr_array_header_t *entries = *(apr_array_header_t **)out;
  const apr_array_header_t *raw_page = data;
  p2l_entries_baton_t *block = baton;

  /* Make PAGE a readable APR array. */
  apr_array_header_t page = *raw_page;
  page.elts = (void *)svn_temp_deserializer__ptr(raw_page,
                                    (const void * const *)&raw_page->elts);

  /* append relevant information to result */
  append_p2l_entries(entries, &page, block->start, block->end, TRUE);

  return SVN_NO_ERROR;
}


/* Body of svn_fs_x__p2l_index_lookup.  However, do a single index page
 * lookup and append the result to the ENTRIES array provided by the caller.
 * Use successive calls to cover larger ranges.
 */
static svn_error_t *
p2l_index_lookup(apr_array_header_t *entries,
                 packed_number_stream_t **stream,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_off_t block_start,
                 apr_off_t block_end,
                 apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  svn_fs_x__page_cache_key_t key;
  svn_boolean_t is_cached = FALSE;
  p2l_page_info_baton_t page_info;
  apr_array_header_t *local_result = entries;

  /* baton selecting the relevant entries from the one page we access */
  p2l_entries_baton_t block;
  block.start = block_start;
  block.end = block_end;

  /* if we requested an empty range, the result would be empty */
  SVN_ERR_ASSERT(block_start < block_end);

  /* look for the fist page of the range in our cache */
  SVN_ERR(get_p2l_keys(&page_info, &key, stream, fs, revision, block_start,
                       pool));
  SVN_ERR(svn_cache__get_partial((void**)&local_result, &is_cached,
                                 ffd->p2l_page_cache, &key, p2l_entries_func,
                                 &block, pool));

  if (!is_cached)
    {
      svn_boolean_t end;
      apr_pool_t *iterpool = svn_pool_create(pool);
      apr_off_t original_page_start = page_info.page_start;
      int leaking_bucket = 4;
      p2l_page_info_baton_t prefetch_info = page_info;
      apr_array_header_t *page_entries;

      apr_off_t max_offset
        = APR_ALIGN(page_info.next_offset, ffd->block_size);
      apr_off_t min_offset
        = APR_ALIGN(page_info.start_offset, ffd->block_size) - ffd->block_size;

      /* Since we read index data in larger chunks, we probably got more
       * page data than we requested.  Parse & cache that until either we
       * encounter pages already cached or reach the end of the buffer.
       */

      /* pre-fetch preceding pages */
      end = FALSE;
      prefetch_info.offset = original_page_start;
      while (prefetch_info.offset >= prefetch_info.page_size && !end)
        {
          prefetch_info.offset -= prefetch_info.page_size;
          SVN_ERR(prefetch_p2l_page(&end, &leaking_bucket, fs, stream,
                                    &prefetch_info, min_offset,
                                    pool, iterpool));
          svn_pool_clear(iterpool);
        }

      /* fetch page from disk and put it into the cache */
      SVN_ERR(get_p2l_page(&page_entries, stream, fs,
                           page_info.first_revision,
                           page_info.start_offset,
                           page_info.next_offset,
                           page_info.page_start,
                           page_info.page_size, pool, iterpool));

      SVN_ERR(svn_cache__set(ffd->p2l_page_cache, &key, page_entries,
                             iterpool));

      /* append relevant information to result */
      append_p2l_entries(entries, page_entries, block_start, block_end, FALSE);

      /* pre-fetch following pages */
      end = FALSE;
      leaking_bucket = 4;
      prefetch_info = page_info;
      prefetch_info.offset = original_page_start;
      while (   prefetch_info.next_offset < max_offset
             && prefetch_info.page_no + 1 < prefetch_info.page_count
             && !end)
        {
          prefetch_info.offset += prefetch_info.page_size;
          SVN_ERR(prefetch_p2l_page(&end, &leaking_bucket, fs, stream,
                                    &prefetch_info, min_offset,
                                    pool, iterpool));
          svn_pool_clear(iterpool);
        }

      svn_pool_destroy(iterpool);
    }

  /* We access a valid page (otherwise, we had seen an error in the
   * get_p2l_keys request).  Hence, at least one entry must be found. */
  SVN_ERR_ASSERT(entries->nelts > 0);

  /* Add an "unused" entry if it extends beyond the end of the data file.
   * Since the index page size might be smaller than the current data
   * read block size, the trailing "unused" entry in this index may not
   * fully cover the end of the last block. */
  if (page_info.page_no + 1 >= page_info.page_count)
    {
      svn_fs_x__p2l_entry_t *entry
        = &APR_ARRAY_IDX(entries, entries->nelts-1, svn_fs_x__p2l_entry_t);

      apr_off_t entry_end = entry->offset + entry->size;
      if (entry_end < block_end)
        {
          if (entry->type == SVN_FS_X__ITEM_TYPE_UNUSED)
            {
              /* extend the terminal filler */
              entry->size = block_end - entry->offset;
            }
          else
            {
              /* No terminal filler. Add one. */
              entry = apr_array_push(entries);
              entry->offset = entry_end;
              entry->size = block_end - entry_end;
              entry->type = SVN_FS_X__ITEM_TYPE_UNUSED;
              entry->fnv1_checksum = 0;
              entry->item_count = 0;
              entry->items = NULL;
            }
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_index_lookup(apr_array_header_t **entries,
                           svn_fs_t *fs,
                           svn_revnum_t revision,
                           apr_off_t block_start,
                           apr_off_t block_size,
                           apr_pool_t *pool)
{
  packed_number_stream_t *stream = NULL;

  apr_off_t block_end = block_start + block_size;

  /* the receiving container */
  int last_count = 0;
  apr_array_header_t *result = apr_array_make(pool, 16,
                                              sizeof(svn_fs_x__p2l_entry_t));

  /* Fetch entries page-by-page.  Since the p2l index is supposed to cover
   * every single byte in the rev / pack file - even unused sections -
   * every iteration must result in some progress. */
  while (block_start < block_end)
    {
      svn_fs_x__p2l_entry_t *entry;
      SVN_ERR(p2l_index_lookup(result, &stream, fs, revision, block_start,
                               block_end, pool));
      SVN_ERR_ASSERT(result->nelts > 0);

      /* continue directly behind last item */
      entry = &APR_ARRAY_IDX(result, result->nelts-1, svn_fs_x__p2l_entry_t);
      block_start = entry->offset + entry->size;

      /* Some paranoia check.  Successive iterations should never return
       * duplicates but if it did, we might get into trouble later on. */
      if (last_count > 0 && last_count < result->nelts)
        {
           entry = &APR_ARRAY_IDX(result, last_count - 1,
                                  svn_fs_x__p2l_entry_t);
           SVN_ERR_ASSERT(APR_ARRAY_IDX(result, last_count,
                                        svn_fs_x__p2l_entry_t).offset
                          >= entry->offset + entry->size);
        }

      last_count = result->nelts;
    }

  /* make sure we close files after usage */
  SVN_ERR(packed_stream_close(stream));

  *entries = result;
  return SVN_NO_ERROR;
}

/* compare_fn_t comparing a svn_fs_x__p2l_entry_t at LHS with an offset
 * RHS.
 */
static int
compare_p2l_entry_offsets(const void *lhs, const void *rhs)
{
  const svn_fs_x__p2l_entry_t *entry = (const svn_fs_x__p2l_entry_t *)lhs;
  apr_off_t offset = *(const apr_off_t *)rhs;

  return entry->offset < offset ? -1 : (entry->offset == offset ? 0 : 1);
}

/* Cached data extraction utility.  DATA is a P2L index page, e.g. an APR
 * array of svn_fs_x__p2l_entry_t elements.  Return the entry for the item
 * starting at OFFSET or NULL if that's not an the start offset of any item.
 */
static svn_fs_x__p2l_entry_t *
get_p2l_entry_from_cached_page(const void *data,
                               apr_off_t offset,
                               apr_pool_t *pool)
{
  /* resolve all pointer values of in-cache data */
  const apr_array_header_t *page = data;
  apr_array_header_t *entries = apr_pmemdup(pool, page, sizeof(*page));
  svn_fs_x__p2l_entry_t *entry;

  entries->elts = (char *)svn_temp_deserializer__ptr(page,
                                     (const void *const *)&page->elts);

  /* search of the offset we want */
  entry = svn_sort__array_lookup(entries, &offset, NULL,
      (int (*)(const void *, const void *))compare_p2l_entry_offsets);

  /* return it, if it is a perfect match */
  if (entry)
    {
      svn_fs_x__p2l_entry_t *result
        = apr_pmemdup(pool, entry, sizeof(*result));
      result->items
        = (svn_fs_x__id_part_t *)svn_temp_deserializer__ptr(entries->elts,
                                     (const void *const *)&entry->items);
      return result;
    }

  return NULL;
}

/* Implements svn_cache__partial_getter_func_t for P2L index pages, copying
 * the entry for the apr_off_t at BATON into *OUT.  *OUT will be NULL if
 * there is no matching entry in the index page at DATA.
 */
static svn_error_t *
p2l_entry_lookup_func(void **out,
                      const void *data,
                      apr_size_t data_len,
                      void *baton,
                      apr_pool_t *result_pool)
{
  svn_fs_x__p2l_entry_t *entry
    = get_p2l_entry_from_cached_page(data, *(apr_off_t *)baton, result_pool);

  *out = entry && entry->offset == *(apr_off_t *)baton
       ? svn_fs_x__p2l_entry_dup(entry, result_pool)
       : NULL;

  return SVN_NO_ERROR;
}

static svn_error_t *
p2l_entry_lookup(svn_fs_x__p2l_entry_t **entry_p,
                 packed_number_stream_t **stream,
                 svn_fs_t *fs,
                 svn_revnum_t revision,
                 apr_off_t offset,
                 apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  svn_fs_x__page_cache_key_t key = { 0 };
  svn_boolean_t is_cached = FALSE;
  p2l_page_info_baton_t page_info;

  /* look for this info in our cache */
  SVN_ERR(get_p2l_keys(&page_info, &key, stream, fs, revision, offset, pool));
  SVN_ERR(svn_cache__get_partial((void**)entry_p, &is_cached,
                                 ffd->p2l_page_cache, &key,
                                 p2l_entry_lookup_func, &offset, pool));
  if (!is_cached)
    {
      /* do a standard index lookup.  This is will automatically prefetch
       * data to speed up future lookups. */
      apr_array_header_t *entries = apr_array_make(pool, 1, sizeof(**entry_p));
      SVN_ERR(p2l_index_lookup(entries, stream, fs, revision, offset,
                               offset + 1, pool));

      /* Find the entry that we want. */
      *entry_p = svn_sort__array_lookup(entries, &offset, NULL,
          (int (*)(const void *, const void *))compare_p2l_entry_offsets);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_entry_lookup(svn_fs_x__p2l_entry_t **entry_p,
                           svn_fs_t *fs,
                           svn_revnum_t revision,
                           apr_off_t offset,
                           apr_pool_t *pool)
{
  packed_number_stream_t *stream = NULL;

  /* look for this info in our cache */
  SVN_ERR(p2l_entry_lookup(entry_p, &stream, fs, revision, offset, pool));

  /* make sure we close files after usage */
  SVN_ERR(packed_stream_close(stream));

  return SVN_NO_ERROR;
}

/* Baton structure for p2l_item_lookup_func.  It describes which sub_item
 * info shall be returned.
 */
typedef struct p2l_item_lookup_baton_t
{
  /* file offset to find the P2L index entry for */
  apr_off_t offset;

  /* return the sub-item at this position within that entry */
  apr_uint32_t sub_item;
} p2l_item_lookup_baton_t;

/* Implements svn_cache__partial_getter_func_t for P2L index pages, copying
 * the svn_fs_x__id_part_t for the item described 2l_item_lookup_baton_t
 * *BATON.  *OUT will be NULL if there is no matching index entry or the
 * sub-item is out of range.
 */
static svn_error_t *
p2l_item_lookup_func(void **out,
                     const void *data,
                     apr_size_t data_len,
                     void *baton,
                     apr_pool_t *result_pool)
{
  p2l_item_lookup_baton_t *lookup_baton = baton;
  svn_fs_x__p2l_entry_t *entry
    = get_p2l_entry_from_cached_page(data, lookup_baton->offset, result_pool);

  *out =    entry
         && entry->offset == lookup_baton->offset
         && entry->item_count > lookup_baton->sub_item
       ? apr_pmemdup(result_pool,
                     entry->items + lookup_baton->sub_item,
                     sizeof(*entry->items))
       : NULL;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_item_lookup(svn_fs_x__id_part_t **item,
                          svn_fs_t *fs,
                          svn_revnum_t revision,
                          apr_off_t offset,
                          apr_uint32_t sub_item,
                          apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  packed_number_stream_t *stream = NULL;
  svn_fs_x__page_cache_key_t key = { 0 };
  svn_boolean_t is_cached = FALSE;
  p2l_page_info_baton_t page_info;
  p2l_item_lookup_baton_t baton;

  *item = NULL;

  /* look for this info in our cache */
  SVN_ERR(get_p2l_keys(&page_info, &key, &stream, fs, revision, offset,
                       pool));
  baton.offset = offset;
  baton.sub_item = sub_item;
  SVN_ERR(svn_cache__get_partial((void**)item, &is_cached,
                                 ffd->p2l_page_cache, &key,
                                 p2l_item_lookup_func, &baton, pool));
  if (!is_cached)
    {
      /* do a standard index lookup.  This is will automatically prefetch
       * data to speed up future lookups. */
      svn_fs_x__p2l_entry_t *entry;
      SVN_ERR(p2l_entry_lookup(&entry, &stream, fs, revision, offset, pool));

      /* return result */
      if (entry && entry->item_count > sub_item)
        *item = apr_pmemdup(pool, entry->items + sub_item, sizeof(**item));
    }

  /* make sure we close files after usage */
  SVN_ERR(packed_stream_close(stream));

  return SVN_NO_ERROR;
}

/* Implements svn_cache__partial_getter_func_t for P2L headers, setting *OUT
 * to the largest the first offset not covered by this P2L index.
 */
static svn_error_t *
p2l_get_max_offset_func(void **out,
                        const void *data,
                        apr_size_t data_len,
                        void *baton,
                        apr_pool_t *result_pool)
{
  const p2l_header_t *header = data;
  apr_off_t max_offset = header->file_size;
  *out = apr_pmemdup(result_pool, &max_offset, sizeof(max_offset));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__p2l_get_max_offset(apr_off_t *offset,
                             svn_fs_t *fs,
                             svn_revnum_t revision,
                             apr_pool_t *pool)
{
  fs_x_data_t *ffd = fs->fsap_data;
  packed_number_stream_t *stream = NULL;
  p2l_header_t *header;
  svn_boolean_t is_cached = FALSE;
  apr_off_t *offset_p;

  /* look for the header data in our cache */
  pair_cache_key_t key;
  key.revision = base_revision(fs, revision);
  key.second = svn_fs_x__is_packed_rev(fs, revision);

  SVN_ERR(svn_cache__get_partial((void **)&offset_p, &is_cached,
                                 ffd->p2l_header_cache, &key,
                                 p2l_get_max_offset_func, NULL, pool));
  if (is_cached)
    {
      *offset = *offset_p;
      return SVN_NO_ERROR;
    }

  SVN_ERR(get_p2l_header(&header, &stream, fs, revision, pool, pool));
  *offset = header->file_size;

  /* make sure we close files after usage */
  SVN_ERR(packed_stream_close(stream));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__item_offset(apr_off_t *offset,
                      apr_uint32_t *sub_item,
                      svn_fs_t *fs,
                      const svn_fs_x__id_part_t *item_id,
                      apr_pool_t *pool)
{
  if (svn_fs_x__is_txn(item_id->change_set))
    SVN_ERR(l2p_proto_index_lookup(offset, sub_item, fs,
                                   svn_fs_x__get_txn_id(item_id->change_set),
                                   item_id->number, pool));
  else
    SVN_ERR(l2p_index_lookup(offset, sub_item, fs,
                             svn_fs_x__get_revnum(item_id->change_set),
                             item_id->number, pool));

  return SVN_NO_ERROR;
}

/*
 * Standard (de-)serialization functions
 */

svn_error_t *
svn_fs_x__serialize_l2p_header(void **data,
                               apr_size_t *data_len,
                               void *in,
                               apr_pool_t *pool)
{
  l2p_header_t *header = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t page_count = header->page_table_index[header->revision_count];
  apr_size_t page_table_size = page_count * sizeof(*header->page_table);
  apr_size_t index_size
    = (header->revision_count + 1) * sizeof(*header->page_table_index);
  apr_size_t data_size = sizeof(*header) + index_size + page_table_size;

  /* serialize header and all its elements */
  context = svn_temp_serializer__init(header,
                                      sizeof(*header),
                                      data_size + 32,
                                      pool);

  /* page table index array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->page_table_index,
                                index_size);

  /* page table array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->page_table,
                                page_table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__deserialize_l2p_header(void **out,
                                 void *data,
                                 apr_size_t data_len,
                                 apr_pool_t *pool)
{
  l2p_header_t *header = (l2p_header_t *)data;

  /* resolve the pointers in the struct */
  svn_temp_deserializer__resolve(header, (void**)&header->page_table_index);
  svn_temp_deserializer__resolve(header, (void**)&header->page_table);

  /* done */
  *out = header;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__serialize_l2p_page(void **data,
                             apr_size_t *data_len,
                             void *in,
                             apr_pool_t *pool)
{
  l2p_page_t *page = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t of_table_size = page->entry_count * sizeof(*page->offsets);
  apr_size_t si_table_size = page->entry_count * sizeof(*page->sub_items);

  /* serialize struct and all its elements */
  context = svn_temp_serializer__init(page,
                                      sizeof(*page),
                                        of_table_size + si_table_size
                                      + sizeof(*page) + 32,
                                      pool);

  /* offsets and sub_items arrays */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&page->offsets,
                                of_table_size);
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&page->sub_items,
                                si_table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__deserialize_l2p_page(void **out,
                               void *data,
                               apr_size_t data_len,
                               apr_pool_t *pool)
{
  l2p_page_t *page = data;

  /* resolve the pointers in the struct */
  svn_temp_deserializer__resolve(page, (void**)&page->offsets);
  svn_temp_deserializer__resolve(page, (void**)&page->sub_items);

  /* done */
  *out = page;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__serialize_p2l_header(void **data,
                               apr_size_t *data_len,
                               void *in,
                               apr_pool_t *pool)
{
  p2l_header_t *header = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t table_size = (header->page_count + 1) * sizeof(*header->offsets);

  /* serialize header and all its elements */
  context = svn_temp_serializer__init(header,
                                      sizeof(*header),
                                      table_size + sizeof(*header) + 32,
                                      pool);

  /* offsets array */
  svn_temp_serializer__add_leaf(context,
                                (const void * const *)&header->offsets,
                                table_size);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__deserialize_p2l_header(void **out,
                                 void *data,
                                 apr_size_t data_len,
                                 apr_pool_t *pool)
{
  p2l_header_t *header = data;

  /* resolve the only pointer in the struct */
  svn_temp_deserializer__resolve(header, (void**)&header->offsets);

  /* done */
  *out = header;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__serialize_p2l_page(void **data,
                             apr_size_t *data_len,
                             void *in,
                             apr_pool_t *pool)
{
  apr_array_header_t *page = in;
  svn_temp_serializer__context_t *context;
  svn_stringbuf_t *serialized;
  apr_size_t table_size = page->elt_size * page->nelts;
  svn_fs_x__p2l_entry_t *entries = (svn_fs_x__p2l_entry_t *)page->elts;
  int i;

  /* serialize array header and all its elements */
  context = svn_temp_serializer__init(page,
                                      sizeof(*page),
                                      table_size + sizeof(*page) + 32,
                                      pool);

  /* items in the array */
  svn_temp_serializer__push(context,
                            (const void * const *)&page->elts,
                            table_size);

  for (i = 0; i < page->nelts; ++i)
    svn_temp_serializer__add_leaf(context, 
                                  (const void * const *)&entries[i].items,
                                    entries[i].item_count
                                  * sizeof(*entries[i].items));

  svn_temp_serializer__pop(context);

  /* return the serialized result */
  serialized = svn_temp_serializer__get(context);

  *data = serialized->data;
  *data_len = serialized->len;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_x__deserialize_p2l_page(void **out,
                               void *data,
                               apr_size_t data_len,
                               apr_pool_t *pool)
{
  apr_array_header_t *page = (apr_array_header_t *)data;
  svn_fs_x__p2l_entry_t *entries;
  int i;

  /* resolve the only pointer in the struct */
  svn_temp_deserializer__resolve(page, (void**)&page->elts);

  /* resolve sub-struct pointers*/
  entries = (svn_fs_x__p2l_entry_t *)page->elts;
  for (i = 0; i < page->nelts; ++i)
    svn_temp_deserializer__resolve(entries, (void**)&entries[i].items);

  /* patch up members */
  page->pool = pool;
  page->nalloc = page->nelts;

  /* done */
  *out = page;

  return SVN_NO_ERROR;
}
