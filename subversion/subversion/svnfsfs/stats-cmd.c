/* stats-cmd.c -- implements the size stats sub-command.
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

#include "svn_dirent_uri.h"
#include "svn_fs.h"
#include "svn_pools.h"
#include "svn_sorts.h"

#include "private/svn_cache.h"
#include "private/svn_sorts_private.h"
#include "private/svn_string_private.h"

#include "../libsvn_fs_fs/index.h"
#include "../libsvn_fs_fs/pack.h"
#include "../libsvn_fs_fs/rev_file.h"
#include "../libsvn_fs_fs/util.h"
#include "../libsvn_fs_fs/fs_fs.h"
#include "../libsvn_fs/fs-loader.h"

#include "svn_private_config.h"
#include "svnfsfs.h"

/* We group representations into 2x2 different kinds plus one default:
 * [dir / file] x [text / prop]. The assignment is done by the first node
 * that references the respective representation.
 */
typedef enum rep_kind_t
{
  /* The representation is _directly_ unused, i.e. not referenced by any
   * noderev. However, some other representation may use it as delta base.
   * null value. Should not occur in real-word repositories. */
  unused_rep,

  /* a properties on directory rep  */
  dir_property_rep,

  /* a properties on file rep  */
  file_property_rep,

  /* a directory rep  */
  dir_rep,

  /* a file rep  */
  file_rep
} rep_kind_t;

/* A representation fragment.
 */
typedef struct rep_stats_t
{
  /* absolute offset in the file */
  apr_size_t offset;

  /* item length in bytes */
  apr_size_t size;

  /* item length after de-deltification */
  apr_size_t expanded_size;

  /* deltification base, or NULL if there is none */
  struct rep_stats_t *delta_base;

  /* revision that contains this representation
   * (may be referenced by other revisions, though) */
  svn_revnum_t revision;

  /* number of nodes that reference this representation */
  apr_uint32_t ref_count;

  /* length of the PLAIN / DELTA line in the source file in bytes */
  apr_uint16_t header_size;

  /* classification of the representation. values of rep_kind_t */
  char kind;

  /* the source content has a PLAIN header, so we may simply copy the
   * source content into the target */
  char is_plain;

} rep_stats_t;

/* Represents a single revision.
 * There will be only one instance per revision. */
typedef struct revision_info_t
{
  /* number of this revision */
  svn_revnum_t revision;

  /* pack file offset (manifest value), 0 for non-packed files */
  apr_off_t offset;

  /* offset of the changes list relative to OFFSET */
  apr_size_t changes;

  /* length of the changes list on bytes */
  apr_size_t changes_len;

  /* offset of the changes list relative to OFFSET */
  apr_size_t change_count;

  /* first offset behind the revision data in the pack file (file length
   * for non-packed revs) */
  apr_off_t end;

  /* number of directory noderevs in this revision */
  apr_size_t dir_noderev_count;

  /* number of file noderevs in this revision */
  apr_size_t file_noderev_count;

  /* total size of directory noderevs (i.e. the structs - not the rep) */
  apr_size_t dir_noderev_size;

  /* total size of file noderevs (i.e. the structs - not the rep) */
  apr_size_t file_noderev_size;

  /* all rep_stats_t of this revision (in no particular order),
   * i.e. those that point back to this struct */
  apr_array_header_t *representations;
} revision_info_t;

/* Data type to identify a representation. It will be used to address
 * cached combined (un-deltified) windows.
 */
typedef struct cache_key_t
{
  /* revision of the representation */
  svn_revnum_t revision;

  /* its offset */
  apr_size_t offset;
} cache_key_t;

/* Description of one large representation.  It's content will be reused /
 * overwritten when it gets replaced by an even larger representation.
 */
typedef struct large_change_info_t
{
  /* size of the (deltified) representation */
  apr_size_t size;

  /* revision of the representation */
  svn_revnum_t revision;

  /* node path. "" for unused instances */
  svn_stringbuf_t *path;
} large_change_info_t;

/* Container for the largest representations found so far.  The capacity
 * is fixed and entries will be inserted by reusing the last one and
 * reshuffling the entry pointers.
 */
typedef struct largest_changes_t
{
  /* number of entries allocated in CHANGES */
  apr_size_t count;

  /* size of the smallest change */
  apr_size_t min_size;

  /* changes kept in this struct */
  large_change_info_t **changes;
} largest_changes_t;

/* Information we gather per size bracket.
 */
typedef struct histogram_line_t
{
  /* number of item that fall into this bracket */
  apr_int64_t count;

  /* sum of values in this bracket */
  apr_int64_t sum;
} histogram_line_t;

/* A histogram of 64 bit integer values.
 */
typedef struct histogram_t
{
  /* total sum over all brackets */
  histogram_line_t total;

  /* one bracket per binary step.
   * line[i] is the 2^(i-1) <= x < 2^i bracket */
  histogram_line_t lines[64];
} histogram_t;

/* Information we collect per file ending.
 */
typedef struct extension_info_t
{
  /* file extension, including leading "."
   * "(none)" in the container for files w/o extension. */
  const char *extension;

  /* histogram of representation sizes */
  histogram_t rep_histogram;

  /* histogram of sizes of changed files */
  histogram_t node_histogram;
} extension_info_t;

/* Root data structure containing all information about a given repository.
 */
typedef struct fs_t
{
  /* FS API object*/
  svn_fs_t *fs;

  /* The HEAD revision. */
  svn_revnum_t head;

  /* Number of revs per shard; 0 for non-sharded repos. */
  int shard_size;

  /* First non-packed revision. */
  svn_revnum_t min_unpacked_rev;

  /* all revisions */
  apr_array_header_t *revisions;

  /* empty representation.
   * Used as a dummy base for DELTA reps without base. */
  rep_stats_t *null_base;

  /* undeltified txdelta window cache */
  svn_cache__t *window_cache;

  /* track the biggest contributors to repo size */
  largest_changes_t *largest_changes;

  /* history of representation sizes */
  histogram_t rep_size_histogram;

  /* history of sizes of changed nodes */
  histogram_t node_size_histogram;

  /* history of representation sizes */
  histogram_t added_rep_size_histogram;

  /* history of sizes of changed nodes */
  histogram_t added_node_size_histogram;

  /* history of unused representations */
  histogram_t unused_rep_histogram;

  /* history of sizes of changed files */
  histogram_t file_histogram;

  /* history of sizes of file representations */
  histogram_t file_rep_histogram;

  /* history of sizes of changed file property sets */
  histogram_t file_prop_histogram;

  /* history of sizes of file property representations */
  histogram_t file_prop_rep_histogram;

  /* history of sizes of changed directories (in bytes) */
  histogram_t dir_histogram;

  /* history of sizes of directories representations */
  histogram_t dir_rep_histogram;

  /* history of sizes of changed directories property sets */
  histogram_t dir_prop_histogram;

  /* history of sizes of directories property representations */
  histogram_t dir_prop_rep_histogram;

  /* extension -> extension_info_t* map */
  apr_hash_t *by_extension;
} fs_t;

/* Open the file containing revision REV in FS and return it in *FILE.
 */
static svn_error_t *
open_rev_or_pack_file(apr_file_t **file,
                      fs_t *fs,
                      svn_revnum_t rev,
                      apr_pool_t *pool)
{
  svn_fs_fs__revision_file_t *rev_file;
  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, fs->fs, rev,
                                           pool, pool));

  *file = rev_file->file;
  return SVN_NO_ERROR;
}

/* Return the length of FILE in *FILE_SIZE.  Use POOL for allocations.
*/
static svn_error_t *
get_file_size(apr_off_t *file_size,
              apr_file_t *file,
              apr_pool_t *pool)
{
  apr_finfo_t finfo;

  SVN_ERR(svn_io_file_info_get(&finfo, APR_FINFO_SIZE, file, pool));

  *file_size = finfo.size;
  return SVN_NO_ERROR;
}

/* Get the file content of revision REVISION in FS and return it in *CONTENT.
 * Read the LEN bytes starting at file OFFSET.  When provided, use FILE as
 * packed or plain rev file.
 * Use POOL for temporary allocations.
 */
static svn_error_t *
get_content(svn_stringbuf_t **content,
            apr_file_t *file,
            fs_t *fs,
            svn_revnum_t revision,
            apr_off_t offset,
            apr_size_t len,
            apr_pool_t *pool)
{
  apr_pool_t * file_pool = svn_pool_create(pool);
  apr_size_t large_buffer_size = 0x10000;

  if (file == NULL)
    SVN_ERR(open_rev_or_pack_file(&file, fs, revision, file_pool));

  *content = svn_stringbuf_create_ensure(len, pool);
  (*content)->len = len;

  /* for better efficiency use larger buffers on large reads */
  if (   (len >= large_buffer_size)
      && (apr_file_buffer_size_get(file) < large_buffer_size))
    apr_file_buffer_set(file,
                        apr_palloc(apr_file_pool_get(file),
                                   large_buffer_size),
                        large_buffer_size);

  SVN_ERR(svn_io_file_seek(file, APR_SET, &offset, pool));
  SVN_ERR(svn_io_file_read_full2(file, (*content)->data, len,
                                 NULL, NULL, pool));
  svn_pool_destroy(file_pool);

  return SVN_NO_ERROR;
}

/* In *RESULT, return the cached txdelta window stored in REPRESENTATION
 * within FS.  If that has not been found in cache, return NULL.
 * Allocate the result in POOL.
 */
static svn_error_t *
get_cached_window(svn_stringbuf_t **result,
                  fs_t *fs,
                  rep_stats_t *representation,
                  apr_pool_t *pool)
{
  svn_boolean_t found = FALSE;
  cache_key_t key;
  key.revision = representation->revision;
  key.offset = representation->offset;

  *result = NULL;
  return svn_error_trace(svn_cache__get((void**)result, &found,
                                        fs->window_cache,
                                        &key, pool));
}

/* Cache the undeltified txdelta WINDOW for REPRESENTATION within FS.
 * Use POOL for temporaries.
 */
static svn_error_t *
set_cached_window(fs_t *fs,
                  rep_stats_t *representation,
                  svn_stringbuf_t *window,
                  apr_pool_t *pool)
{
  /* select entry */
  cache_key_t key;
  key.revision = representation->revision;
  key.offset = representation->offset;

  return svn_error_trace(svn_cache__set(fs->window_cache, &key, window,
                                        pool));
}

/* Initialize the LARGEST_CHANGES member in FS with a capacity of COUNT
 * entries.  Use POOL for allocations.
 */
static void
initialize_largest_changes(fs_t *fs,
                           apr_size_t count,
                           apr_pool_t *pool)
{
  apr_size_t i;

  fs->largest_changes = apr_pcalloc(pool, sizeof(*fs->largest_changes));
  fs->largest_changes->count = count;
  fs->largest_changes->min_size = 1;
  fs->largest_changes->changes
    = apr_palloc(pool, count * sizeof(*fs->largest_changes->changes));

  /* allocate *all* entries before the path stringbufs.  This increases
   * cache locality and enhances performance significantly. */
  for (i = 0; i < count; ++i)
    fs->largest_changes->changes[i]
      = apr_palloc(pool, sizeof(**fs->largest_changes->changes));

  /* now initialize them and allocate the stringbufs */
  for (i = 0; i < count; ++i)
    {
      fs->largest_changes->changes[i]->size = 0;
      fs->largest_changes->changes[i]->revision = SVN_INVALID_REVNUM;
      fs->largest_changes->changes[i]->path
        = svn_stringbuf_create_ensure(1024, pool);
    }
}

/* Add entry for SIZE to HISTOGRAM.
 */
static void
add_to_histogram(histogram_t *histogram,
                 apr_int64_t size)
{
  apr_int64_t shift = 0;

  while (((apr_int64_t)(1) << shift) <= size)
    shift++;

  histogram->total.count++;
  histogram->total.sum += size;
  histogram->lines[(apr_size_t)shift].count++;
  histogram->lines[(apr_size_t)shift].sum += size;
}

/* Update data aggregators in FS with this representation of type KIND, on-
 * disk REP_SIZE and expanded node size EXPANDED_SIZE for PATH in REVSION.
 * PLAIN_ADDED indicates whether the node has a deltification predecessor.
 */
static void
add_change(fs_t *fs,
           apr_int64_t rep_size,
           apr_int64_t expanded_size,
           svn_revnum_t revision,
           const char *path,
           rep_kind_t kind,
           svn_boolean_t plain_added)
{
  /* identify largest reps */
  if (rep_size >= fs->largest_changes->min_size)
    {
      apr_size_t i;
      large_change_info_t *info
        = fs->largest_changes->changes[fs->largest_changes->count - 1];
      info->size = rep_size;
      info->revision = revision;
      svn_stringbuf_set(info->path, path);

      /* linear insertion but not too bad since count is low and insertions
       * near the end are more likely than close to front */
      for (i = fs->largest_changes->count - 1; i > 0; --i)
        if (fs->largest_changes->changes[i-1]->size >= rep_size)
          break;
        else
          fs->largest_changes->changes[i] = fs->largest_changes->changes[i-1];

      fs->largest_changes->changes[i] = info;
      fs->largest_changes->min_size
        = fs->largest_changes->changes[fs->largest_changes->count-1]->size;
    }

  /* global histograms */
  add_to_histogram(&fs->rep_size_histogram, rep_size);
  add_to_histogram(&fs->node_size_histogram, expanded_size);

  if (plain_added)
    {
      add_to_histogram(&fs->added_rep_size_histogram, rep_size);
      add_to_histogram(&fs->added_node_size_histogram, expanded_size);
    }

  /* specific histograms by type */
  switch (kind)
    {
      case unused_rep:        add_to_histogram(&fs->unused_rep_histogram,
                                               rep_size);
                              break;
      case dir_property_rep:  add_to_histogram(&fs->dir_prop_rep_histogram,
                                               rep_size);
                              add_to_histogram(&fs->dir_prop_histogram,
                                              expanded_size);
                              break;
      case file_property_rep: add_to_histogram(&fs->file_prop_rep_histogram,
                                               rep_size);
                              add_to_histogram(&fs->file_prop_histogram,
                                               expanded_size);
                              break;
      case dir_rep:           add_to_histogram(&fs->dir_rep_histogram,
                                               rep_size);
                              add_to_histogram(&fs->dir_histogram,
                                               expanded_size);
                              break;
      case file_rep:          add_to_histogram(&fs->file_rep_histogram,
                                               rep_size);
                              add_to_histogram(&fs->file_histogram,
                                               expanded_size);
                              break;
    }

  /* by extension */
  if (kind == file_rep)
    {
      /* determine extension */
      extension_info_t *info;
      const char * file_name = strrchr(path, '/');
      const char * extension = file_name ? strrchr(file_name, '.') : NULL;

      if (extension == NULL || extension == file_name + 1)
        extension = "(none)";

      /* get / auto-insert entry for this extension */
      info = apr_hash_get(fs->by_extension, extension, APR_HASH_KEY_STRING);
      if (info == NULL)
        {
          apr_pool_t *pool = apr_hash_pool_get(fs->by_extension);
          info = apr_pcalloc(pool, sizeof(*info));
          info->extension = apr_pstrdup(pool, extension);

          apr_hash_set(fs->by_extension, info->extension,
                       APR_HASH_KEY_STRING, info);
        }

      /* update per-extension histogram */
      add_to_histogram(&info->node_histogram, expanded_size);
      add_to_histogram(&info->rep_histogram, rep_size);
    }
}

/* Read header information for the revision stored in FILE_CONTENT (one
 * whole revision).  Return the offsets within FILE_CONTENT for the
 * *ROOT_NODEREV, the list of *CHANGES and its len in *CHANGES_LEN.
 * Use POOL for temporary allocations. */
static svn_error_t *
read_revision_header(apr_size_t *changes,
                     apr_size_t *changes_len,
                     apr_size_t *root_noderev,
                     svn_stringbuf_t *file_content,
                     apr_pool_t *pool)
{
  char buf[64];
  const char *line;
  char *space;
  apr_uint64_t val;
  apr_size_t len;

  /* Read in this last block, from which we will identify the last line. */
  len = sizeof(buf);
  if (len > file_content->len)
    len = file_content->len;

  memcpy(buf, file_content->data + file_content->len - len, len);

  /* The last byte should be a newline. */
  if (buf[(apr_ssize_t)len - 1] != '\n')
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Revision lacks trailing newline"));

  /* Look for the next previous newline. */
  buf[len - 1] = 0;
  line = strrchr(buf, '\n');
  if (line == NULL)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Final line in revision file longer "
                              "than 64 characters"));

  space = strchr(line, ' ');
  if (space == NULL)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Final line in revision file missing space"));

  /* terminate the header line */
  *space = 0;

  /* extract information */
  SVN_ERR(svn_cstring_strtoui64(&val, line+1, 0, APR_SIZE_MAX, 10));
  *root_noderev = (apr_size_t)val;
  SVN_ERR(svn_cstring_strtoui64(&val, space+1, 0, APR_SIZE_MAX, 10));
  *changes = (apr_size_t)val;
  *changes_len = file_content->len - *changes - (buf + len - line) + 1;

  return SVN_NO_ERROR;
}

/* Create *FS for the repository at PATH and read the format and size info.
 * Use POOL for temporary allocations.
 */
static svn_error_t *
fs_open(fs_t **fs, const char *path, apr_pool_t *pool)
{
  *fs = apr_pcalloc(pool, sizeof(**fs));

  /* Check repository type and open it. */
  SVN_ERR(open_fs(&(*fs)->fs, path, pool));

  /* Read repository dimensions. */
  (*fs)->shard_size = svn_fs_fs__shard_size((*fs)->fs);
  SVN_ERR(svn_fs_fs__youngest_rev(&(*fs)->head, (*fs)->fs, pool));
  SVN_ERR(svn_fs_fs__min_unpacked_rev(&(*fs)->min_unpacked_rev, (*fs)->fs,
                                      pool));

  return SVN_NO_ERROR;
}

/* Utility function that returns true if STRING->DATA matches KEY.
 */
static svn_boolean_t
key_matches(svn_string_t *string, const char *key)
{
  return strcmp(string->data, key) == 0;
}

/* Comparator used for binary search comparing the absolute file offset
 * of a representation to some other offset. DATA is a *rep_stats_t,
 * KEY is a pointer to an apr_size_t.
 */
static int
compare_representation_offsets(const void *data, const void *key)
{
  apr_ssize_t diff = (*(const rep_stats_t *const *)data)->offset
                     - *(const apr_size_t *)key;

  /* sizeof(int) may be < sizeof(ssize_t) */
  if (diff < 0)
    return -1;
  return diff > 0 ? 1 : 0;
}

/* Find the revision_info_t object to the given REVISION in FS and return
 * it in *REVISION_INFO. For performance reasons, we skip the lookup if
 * the info is already provided.
 *
 * In that revision, look for the rep_stats_t object for offset OFFSET.
 * If it already exists, set *IDX to its index in *REVISION_INFO's
 * representations list and return the representation object. Otherwise,
 * set the index to where it must be inserted and return NULL.
 */
static rep_stats_t *
find_representation(int *idx,
                    fs_t *fs,
                    revision_info_t **revision_info,
                    svn_revnum_t revision,
                    apr_size_t offset)
{
  revision_info_t *info;
  *idx = -1;

  /* first let's find the revision */
  info = revision_info ? *revision_info : NULL;
  if (info == NULL || info->revision != revision)
    {
      info = APR_ARRAY_IDX(fs->revisions, revision, revision_info_t*);
      if (revision_info)
        *revision_info = info;
    }

  /* not found -> no result */
  if (info == NULL)
    return NULL;

  /* look for the representation */
  *idx = svn_sort__bsearch_lower_bound(info->representations,
                                       &offset,
                                       compare_representation_offsets);
  if (*idx < info->representations->nelts)
    {
      /* return the representation, if this is the one we were looking for */
      rep_stats_t *result
        = APR_ARRAY_IDX(info->representations, *idx, rep_stats_t *);
      if (result->offset == offset)
        return result;
    }

  /* not parsed, yet */
  return NULL;
}

/* Read the representation header in FILE_CONTENT at OFFSET.  Return its
 * size in *HEADER_SIZE, set *IS_PLAIN if no deltification was used and
 * return the deltification base representation in *REPRESENTATION.  If
 * there is none, set it to NULL.  Use FS to it look up.
 *
 * Use POOL for allocations and SCRATCH_POOL for temporaries.
 */
static svn_error_t *
read_rep_base(rep_stats_t **representation,
              apr_size_t *header_size,
              svn_boolean_t *is_plain,
              fs_t *fs,
              svn_stringbuf_t *file_content,
              apr_size_t offset,
              apr_pool_t *pool,
              apr_pool_t *scratch_pool)
{
  char *str, *last_str;
  int idx;
  svn_revnum_t revision;
  apr_uint64_t temp;

  /* identify representation header (1 line) */
  const char *buffer = file_content->data + offset;
  const char *line_end = strchr(buffer, '\n');
  *header_size = line_end - buffer + 1;

  /* check for PLAIN rep */
  if (strncmp(buffer, "PLAIN\n", *header_size) == 0)
    {
      *is_plain = TRUE;
      *representation = NULL;
      return SVN_NO_ERROR;
    }

  /* check for DELTA against empty rep */
  *is_plain = FALSE;
  if (strncmp(buffer, "DELTA\n", *header_size) == 0)
    {
      /* This is a delta against the empty stream. */
      *representation = fs->null_base;
      return SVN_NO_ERROR;
    }

  str = apr_pstrndup(scratch_pool, buffer, line_end - buffer);
  last_str = str;

  /* parse it. */
  str = svn_cstring_tokenize(" ", &last_str);
  str = svn_cstring_tokenize(" ", &last_str);
  SVN_ERR(svn_revnum_parse(&revision, str, NULL));

  str = svn_cstring_tokenize(" ", &last_str);
  SVN_ERR(svn_cstring_strtoui64(&temp, str, 0, APR_SIZE_MAX, 10));

  /* it should refer to a rep in an earlier revision.  Look it up */
  *representation = find_representation(&idx, fs, NULL, revision, (apr_size_t)temp);
  return SVN_NO_ERROR;
}

/* Parse the representation reference (text: or props:) in VALUE, look
 * it up in FS and return it in *REPRESENTATION.  To be able to parse the
 * base rep, we pass the FILE_CONTENT as well.
 *
 * If necessary, allocate the result in POOL; use SCRATCH_POOL for temp.
 * allocations.
 */
static svn_error_t *
parse_representation(rep_stats_t **representation,
                     fs_t *fs,
                     svn_stringbuf_t *file_content,
                     svn_string_t *value,
                     revision_info_t *revision_info,
                     apr_pool_t *pool,
                     apr_pool_t *scratch_pool)
{
  rep_stats_t *result;
  svn_revnum_t revision;

  apr_uint64_t offset;
  apr_uint64_t size;
  apr_uint64_t expanded_size;
  int idx;

  /* read location (revision, offset) and size */
  char *c = (char *)value->data;
  SVN_ERR(svn_revnum_parse(&revision, svn_cstring_tokenize(" ", &c), NULL));
  SVN_ERR(svn_cstring_strtoui64(&offset, svn_cstring_tokenize(" ", &c), 0, APR_SIZE_MAX, 10));
  SVN_ERR(svn_cstring_strtoui64(&size, svn_cstring_tokenize(" ", &c), 0, APR_SIZE_MAX, 10));
  SVN_ERR(svn_cstring_strtoui64(&expanded_size, svn_cstring_tokenize(" ", &c), 0, APR_SIZE_MAX, 10));

  /* look it up */
  result = find_representation(&idx, fs, &revision_info, revision, (apr_size_t)offset);
  if (!result)
    {
      /* not parsed, yet (probably a rep in the same revision).
       * Create a new rep object and determine its base rep as well.
       */
      apr_size_t header_size;
      svn_boolean_t is_plain;

      result = apr_pcalloc(pool, sizeof(*result));
      result->revision = revision;
      result->expanded_size = (apr_size_t)(expanded_size ? expanded_size : size);
      result->offset = (apr_size_t)offset;
      result->size = (apr_size_t)size;

      if (!svn_fs_fs__use_log_addressing(fs->fs, revision))
        {
          SVN_ERR(read_rep_base(&result->delta_base, &header_size,
                                &is_plain, fs, file_content,
                                (apr_size_t)offset,
                                pool, scratch_pool));

          result->header_size = header_size;
          result->is_plain = is_plain;
        }

      svn_sort__array_insert(revision_info->representations, &result, idx);
    }

  *representation = result;

  return SVN_NO_ERROR;
}

/* Get the unprocessed (i.e. still deltified) content of REPRESENTATION in
 * FS and return it in *CONTENT.  If no NULL, FILE_CONTENT must contain
 * the contents of the revision that also contains the representation.
 * Use POOL for allocations.
 */
static svn_error_t *
get_rep_content(svn_stringbuf_t **content,
                fs_t *fs,
                rep_stats_t *representation,
                svn_stringbuf_t *file_content,
                apr_pool_t *pool)
{
  apr_off_t offset;
  svn_revnum_t revision = representation->revision;
  revision_info_t *revision_info = APR_ARRAY_IDX(fs->revisions,
                                                 revision,
                                                 revision_info_t*);

  /* not in cache. Is the revision valid at all? */
  if (revision > fs->revisions->nelts)
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Unknown revision %ld"), revision);

  if (file_content)
    {
      offset = representation->offset
            +  representation->header_size;
      *content = svn_stringbuf_ncreate(file_content->data + offset,
                                       representation->size, pool);
    }
  else
    {
      offset = revision_info->offset
             + representation->offset
             + representation->header_size;
      SVN_ERR(get_content(content, NULL, fs, revision, offset,
                          representation->size, pool));
    }

  return SVN_NO_ERROR;
}


/* Read the delta window contents of all windows in REPRESENTATION in FS.
 * If no NULL, FILE_CONTENT must contain the contents of the revision that
 * also contains the representation.
 * Return the data as svn_txdelta_window_t* instances in *WINDOWS.
 * Use POOL for allocations.
 */
static svn_error_t *
read_windows(apr_array_header_t **windows,
             fs_t *fs,
             rep_stats_t *representation,
             svn_stringbuf_t *file_content,
             apr_pool_t *pool)
{
  svn_stringbuf_t *content;
  svn_stream_t *stream;
  char version;
  apr_size_t len = sizeof(version);

  *windows = apr_array_make(pool, 0, sizeof(svn_txdelta_window_t *));

  /* get the whole revision content */
  SVN_ERR(get_rep_content(&content, fs, representation, file_content, pool));

  /* create a read stream and position it directly after the rep header */
  content->data += 3;
  content->len -= 3;
  stream = svn_stream_from_stringbuf(content, pool);
  SVN_ERR(svn_stream_read_full(stream, &version, &len));

  /* read the windows from that stream */
  while (TRUE)
    {
      svn_txdelta_window_t *window;
      svn_stream_mark_t *mark;
      char dummy;

      len = sizeof(dummy);
      SVN_ERR(svn_stream_mark(stream, &mark, pool));
      SVN_ERR(svn_stream_read_full(stream, &dummy, &len));
      if (len == 0)
        break;

      SVN_ERR(svn_stream_seek(stream, mark));
      SVN_ERR(svn_txdelta_read_svndiff_window(&window, stream, version, pool));
      APR_ARRAY_PUSH(*windows, svn_txdelta_window_t *) = window;
    }

  return SVN_NO_ERROR;
}

/* Get the undeltified representation that is a result of combining all
 * deltas from the current desired REPRESENTATION in FS with its base
 * representation.  If no NULL, FILE_CONTENT must contain the contents of
 * the revision that also contains the representation.  Store the result
 * in *CONTENT.  Use POOL for allocations.
 */
static svn_error_t *
get_combined_window(svn_stringbuf_t **content,
                    fs_t *fs,
                    rep_stats_t *representation,
                    svn_stringbuf_t *file_content,
                    apr_pool_t *pool)
{
  int i;
  apr_array_header_t *windows;
  svn_stringbuf_t *base_content, *result;
  const char *source;
  apr_pool_t *subpool;
  apr_pool_t *iterpool;

  /* special case: no un-deltification necessary */
  if (representation->is_plain)
    {
      SVN_ERR(get_rep_content(content, fs, representation, file_content,
                              pool));
      SVN_ERR(set_cached_window(fs, representation, *content, pool));
      return SVN_NO_ERROR;
    }

  /* special case: data already in cache */
  SVN_ERR(get_cached_window(content, fs, representation, pool));
  if (*content)
    return SVN_NO_ERROR;

  /* read the delta windows for this representation */
  subpool = svn_pool_create(pool);
  iterpool = svn_pool_create(pool);
  SVN_ERR(read_windows(&windows, fs, representation, file_content, subpool));

  /* fetch the / create a base content */
  if (representation->delta_base && representation->delta_base->revision)
    SVN_ERR(get_combined_window(&base_content, fs,
                                representation->delta_base, NULL, subpool));
  else
    base_content = svn_stringbuf_create_empty(subpool);

  /* apply deltas */
  result = svn_stringbuf_create_empty(pool);
  source = base_content->data;

  for (i = 0; i < windows->nelts; ++i)
    {
      svn_txdelta_window_t *window
        = APR_ARRAY_IDX(windows, i, svn_txdelta_window_t *);
      svn_stringbuf_t *buf
        = svn_stringbuf_create_ensure(window->tview_len, iterpool);

      buf->len = window->tview_len;
      svn_txdelta_apply_instructions(window, window->src_ops ? source : NULL,
                                     buf->data, &buf->len);

      svn_stringbuf_appendbytes(result, buf->data, buf->len);
      source += window->sview_len;

      svn_pool_clear(iterpool);
    }

  /* cache result and return it */
  SVN_ERR(set_cached_window(fs, representation, result, subpool));
  *content = result;

  svn_pool_destroy(iterpool);
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* forward declaration */
static svn_error_t *
read_noderev(fs_t *fs,
             svn_stringbuf_t *file_content,
             apr_size_t offset,
             revision_info_t *revision_info,
             apr_pool_t *pool,
             apr_pool_t *scratch_pool);

/* Starting at the directory in REPRESENTATION in FILE_CONTENT, read all
 * DAG nodes, directories and representations linked in that tree structure.
 * Store them in FS and REVISION_INFO.  Also, read them only once.
 *
 * Use POOL for persistent allocations and SCRATCH_POOL for temporaries.
 */
static svn_error_t *
parse_dir(fs_t *fs,
          svn_stringbuf_t *file_content,
          rep_stats_t *representation,
          revision_info_t *revision_info,
          apr_pool_t *pool,
          apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *text;
  apr_pool_t *iterpool;
  apr_pool_t *text_pool;
  const char *current;
  const char *revision_key;
  apr_size_t key_len;

  /* special case: empty dir rep */
  if (representation == NULL)
    return SVN_NO_ERROR;

  /* get the directory as unparsed string */
  iterpool = svn_pool_create(scratch_pool);
  text_pool = svn_pool_create(scratch_pool);

  SVN_ERR(get_combined_window(&text, fs, representation, file_content,
                              text_pool));
  current = text->data;

  /* calculate some invariants */
  revision_key = apr_psprintf(text_pool, "r%ld/", representation->revision);
  key_len = strlen(revision_key);

  /* Parse and process all directory entries. */
  while (*current != 'E')
    {
      char *next;

      /* skip "K ???\n<name>\nV ???\n" lines*/
      current = strchr(current, '\n');
      if (current)
        current = strchr(current+1, '\n');
      if (current)
        current = strchr(current+1, '\n');
      next = current ? strchr(++current, '\n') : NULL;
      if (next == NULL)
        return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
           _("Corrupt directory representation in r%ld at offset %ld"),
                                 representation->revision,
                                 (long)representation->offset);

      /* iff this entry refers to a node in the same revision as this dir,
       * recurse into that node */
      *next = 0;
      current = strstr(current, revision_key);
      if (current)
        {
          /* recurse */
          apr_uint64_t offset;

          SVN_ERR(svn_cstring_strtoui64(&offset, current + key_len, 0,
                                        APR_SIZE_MAX, 10));
          SVN_ERR(read_noderev(fs, file_content, (apr_size_t)offset,
                               revision_info, pool, iterpool));

          svn_pool_clear(iterpool);
        }
      current = next+1;
    }

  svn_pool_destroy(iterpool);
  svn_pool_destroy(text_pool);
  return SVN_NO_ERROR;
}

/* Starting at the noderev at OFFSET in FILE_CONTENT, read all DAG nodes,
 * directories and representations linked in that tree structure.  Store
 * them in FS and REVISION_INFO.  Also, read them only once.  Return the
 * result in *NODEREV.
 *
 * Use POOL for persistent allocations and SCRATCH_POOL for temporaries.
 */
static svn_error_t *
read_noderev(fs_t *fs,
             svn_stringbuf_t *file_content,
             apr_size_t offset,
             revision_info_t *revision_info,
             apr_pool_t *pool,
             apr_pool_t *scratch_pool)
{
  svn_string_t *line;
  rep_stats_t *text = NULL;
  rep_stats_t *props = NULL;
  apr_size_t start_offset = offset;
  svn_boolean_t is_dir = FALSE;
  svn_boolean_t has_predecessor = FALSE;
  const char *path = "???";

  scratch_pool = svn_pool_create(scratch_pool);

  /* parse the noderev line-by-line until we find an empty line */
  while (1)
    {
      /* for this line, extract key and value. Ignore invalid values */
      svn_string_t key;
      svn_string_t value;
      char *sep;
      const char *start = file_content->data + offset;
      const char *end = strchr(start, '\n');

      line = svn_string_ncreate(start, end - start, scratch_pool);
      offset += end - start + 1;

      /* empty line -> end of noderev data */
      if (line->len == 0)
        break;

      sep = strchr(line->data, ':');
      if (sep == NULL)
        continue;

      key.data = line->data;
      key.len = sep - key.data;
      *sep = 0;

      if (key.len + 2 > line->len)
        continue;

      value.data = sep + 2;
      value.len = line->len - (key.len + 2);

      /* translate (key, value) into noderev elements */
      if (key_matches(&key, "type"))
        is_dir = strcmp(value.data, "dir") == 0;
      else if (key_matches(&key, "text"))
        {
          SVN_ERR(parse_representation(&text, fs, file_content,
                                       &value, revision_info,
                                       pool, scratch_pool));

          /* if we are the first to use this rep, mark it as "text rep" */
          if (++text->ref_count == 1)
            text->kind = is_dir ? dir_rep : file_rep;
        }
      else if (key_matches(&key, "props"))
        {
          SVN_ERR(parse_representation(&props, fs, file_content,
                                       &value, revision_info,
                                       pool, scratch_pool));

          /* if we are the first to use this rep, mark it as "prop rep" */
          if (++props->ref_count == 1)
            props->kind = is_dir ? dir_property_rep : file_property_rep;
        }
      else if (key_matches(&key, "cpath"))
        path = value.data;
      else if (key_matches(&key, "pred"))
        has_predecessor = TRUE;
    }

  /* record largest changes */
  if (text && text->ref_count == 1)
    add_change(fs, (apr_int64_t)text->size, (apr_int64_t)text->expanded_size,
               text->revision, path, text->kind, !has_predecessor);
  if (props && props->ref_count == 1)
    add_change(fs, (apr_int64_t)props->size, (apr_int64_t)props->expanded_size,
               props->revision, path, props->kind, !has_predecessor);

  /* if this is a directory and has not been processed, yet, read and
   * process it recursively */
  if (   is_dir && text && text->ref_count == 1
      && !svn_fs_fs__use_log_addressing(fs->fs, revision_info->revision))
    SVN_ERR(parse_dir(fs, file_content, text, revision_info,
                      pool, scratch_pool));

  /* update stats */
  if (is_dir)
    {
      revision_info->dir_noderev_size += offset - start_offset;
      revision_info->dir_noderev_count++;
    }
  else
    {
      revision_info->file_noderev_size += offset - start_offset;
      revision_info->file_noderev_count++;
    }
  svn_pool_destroy(scratch_pool);

  return SVN_NO_ERROR;
}

/* Given the unparsed changes list in CHANGES with LEN chars, return the
 * number of changed paths encoded in it.
 */
static apr_size_t
get_change_count(const char *changes,
                 apr_size_t len)
{
  apr_size_t lines = 0;
  const char *end = changes + len;

  /* line count */
  for (; changes < end; ++changes)
    if (*changes == '\n')
      ++lines;

  /* two lines per change */
  return lines / 2;
}

/* Simple utility to print a REVISION number and make it appear immediately.
 */
static void
print_progress(svn_revnum_t revision)
{
  printf("%8ld", revision);
  fflush(stdout);
}

/* Read the content of the pack file staring at revision BASE physical
 * addressing mode and store it in FS.  Use POOL for allocations.
 */
static svn_error_t *
read_phys_pack_file(fs_t *fs,
                    svn_revnum_t base,
                    apr_pool_t *pool)
{
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_pool_t *iter_pool = svn_pool_create(local_pool);
  int i;
  apr_off_t file_size = 0;
  apr_file_t *file;

  SVN_ERR(open_rev_or_pack_file(&file, fs, base, local_pool));
  SVN_ERR(get_file_size(&file_size, file, local_pool));

  /* process each revision in the pack file */
  for (i = 0; i < fs->shard_size; ++i)
    {
      apr_size_t root_node_offset;
      svn_stringbuf_t *rev_content;

      /* create the revision info for the current rev */
      revision_info_t *info = apr_pcalloc(pool, sizeof(*info));
      info->representations = apr_array_make(iter_pool, 4, sizeof(rep_stats_t*));

      info->revision = base + i;
      SVN_ERR(svn_fs_fs__get_packed_offset(&info->offset, fs->fs, base + i,
                                           iter_pool));
      if (i + 1 == fs->shard_size)
        SVN_ERR(svn_io_file_seek(file, APR_END, &info->end, iter_pool));
      else
        SVN_ERR(svn_fs_fs__get_packed_offset(&info->end, fs->fs,
                                             base + i + 1, iter_pool));

      SVN_ERR(get_content(&rev_content, file, fs, info->revision,
                          info->offset,
                          info->end - info->offset,
                          iter_pool));

      SVN_ERR(read_revision_header(&info->changes,
                                   &info->changes_len,
                                   &root_node_offset,
                                   rev_content,
                                   iter_pool));

      info->change_count
        = get_change_count(rev_content->data + info->changes,
                           info->changes_len);
      SVN_ERR(read_noderev(fs, rev_content,
                           root_node_offset, info, pool, iter_pool));

      info->representations = apr_array_copy(pool, info->representations);
      APR_ARRAY_PUSH(fs->revisions, revision_info_t*) = info;

      /* destroy temps */
      svn_pool_clear(iter_pool);
    }

  /* one more pack file processed */
  print_progress(base);
  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Read the content of the file for REVISION in physical addressing mode
 * and store its contents in FS.  Use POOL for allocations.
 */
static svn_error_t *
read_phys_revision_file(fs_t *fs,
                        svn_revnum_t revision,
                        apr_pool_t *pool)
{
  apr_size_t root_node_offset;
  apr_pool_t *local_pool = svn_pool_create(pool);
  svn_stringbuf_t *rev_content;
  revision_info_t *info = apr_pcalloc(pool, sizeof(*info));
  apr_off_t file_size = 0;
  apr_file_t *file;

  /* read the whole pack file into memory */
  SVN_ERR(open_rev_or_pack_file(&file, fs, revision, local_pool));
  SVN_ERR(get_file_size(&file_size, file, local_pool));

  /* create the revision info for the current rev */
  info->representations = apr_array_make(pool, 4, sizeof(rep_stats_t*));

  info->revision = revision;
  info->offset = 0;
  info->end = file_size;

  SVN_ERR(get_content(&rev_content, file, fs, revision, 0, file_size,
                      local_pool));

  SVN_ERR(read_revision_header(&info->changes,
                               &info->changes_len,
                               &root_node_offset,
                               rev_content,
                               local_pool));

  /* put it into our containers */
  APR_ARRAY_PUSH(fs->revisions, revision_info_t*) = info;

  info->change_count
    = get_change_count(rev_content->data + info->changes,
                       info->changes_len);

  /* parse the revision content recursively. */
  SVN_ERR(read_noderev(fs, rev_content,
                       root_node_offset, info,
                       pool, local_pool));

  /* show progress every 1000 revs or so */
  if (fs->shard_size && (revision % fs->shard_size == 0))
    print_progress(revision);
  if (!fs->shard_size && (revision % 1000 == 0))
    print_progress(revision);

  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Read the item described by ENTRY from the REV_FILE in FS and return
 * the respective byte sequence in *CONTENTS allocated in POOL.
 */
static svn_error_t *
read_item(svn_stringbuf_t **contents,
          fs_t *fs,
          svn_fs_fs__revision_file_t *rev_file,
          svn_fs_fs__p2l_entry_t *entry,
          apr_pool_t *pool)
{
  svn_stringbuf_t *item = svn_stringbuf_create_ensure(entry->size, pool);
  item->len = entry->size;
  item->data[item->len] = 0;

  SVN_ERR(svn_io_file_aligned_seek(rev_file->file, REV_FILE_BLOCK_SIZE, NULL,
                                   entry->offset, pool));
  SVN_ERR(svn_io_file_read_full2(rev_file->file, item->data, item->len,
                                 NULL, NULL, pool));

  *contents = item;

  return SVN_NO_ERROR;
}

/* Process the logically addressed revision contents of revisions BASE to
 * BASE + COUNT - 1 in FS.  Use POOL for allocations.
 */
static svn_error_t *
read_log_rev_or_packfile(fs_t *fs,
                         svn_revnum_t base,
                         int count,
                         apr_pool_t *pool)
{
  apr_pool_t *iterpool = svn_pool_create(pool);
  apr_pool_t *localpool = svn_pool_create(pool);
  apr_off_t max_offset;
  apr_off_t offset = 0;
  int i;
  svn_fs_fs__revision_file_t *rev_file;

  /* we will process every revision in the rev / pack file */
  for (i = 0; i < count; ++i)
    {
      /* create the revision info for the current rev */
      revision_info_t *info = apr_pcalloc(pool, sizeof(*info));
      info->representations = apr_array_make(pool, 4, sizeof(rep_stats_t*));
      info->revision = base + i;

      APR_ARRAY_PUSH(fs->revisions, revision_info_t*) = info;
    }

  /* open the pack / rev file that is covered by the p2l index */
  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, fs->fs, base,
                                           localpool, iterpool));
  SVN_ERR(svn_fs_fs__p2l_get_max_offset(&max_offset, fs->fs, rev_file,
                                        base, localpool));

  /* record the whole pack size in the first rev so the total sum will
     still be correct */
  APR_ARRAY_IDX(fs->revisions, base, revision_info_t*)->end = max_offset;

  /* for all offsets in the file, get the P2L index entries and process
     the interesting items (change lists, noderevs) */
  for (offset = 0; offset < max_offset; )
    {
      apr_array_header_t *entries;

      svn_pool_clear(iterpool);

      /* get all entries for the current block */
      SVN_ERR(svn_fs_fs__p2l_index_lookup(&entries, fs->fs, rev_file, base,
                                          offset, INDEX_BLOCK_SIZE,
                                          iterpool));

      /* process all entries (and later continue with the next block) */
      for (i = 0; i < entries->nelts; ++i)
        {
          svn_fs_fs__p2l_entry_t *entry
            = &APR_ARRAY_IDX(entries, i, svn_fs_fs__p2l_entry_t);

          /* skip bits we previously processed */
          if (i == 0 && entry->offset < offset)
            continue;

          /* skip zero-sized entries */
          if (entry->size == 0)
            continue;

          /* read and process interesting items */
          if (entry->type == SVN_FS_FS__ITEM_TYPE_NODEREV)
            {
              svn_stringbuf_t *item;
              revision_info_t *info = APR_ARRAY_IDX(fs->revisions,
                                                    entry->item.revision,
                                                    revision_info_t*);
              SVN_ERR(read_item(&item, fs, rev_file, entry, iterpool));
              SVN_ERR(read_noderev(fs, item, 0, info, pool, iterpool));
            }
          else if (entry->type == SVN_FS_FS__ITEM_TYPE_CHANGES)
            {
              svn_stringbuf_t *item;
              revision_info_t *info = APR_ARRAY_IDX(fs->revisions,
                                                    entry->item.revision,
                                                    revision_info_t*);
              SVN_ERR(read_item(&item, fs, rev_file, entry, iterpool));
              info->change_count
                = get_change_count(item->data + 0, item->len);
              info->changes_len += entry->size;
            }

          /* advance offset */
          offset += entry->size;
        }
    }

  /* clean up and close file handles */
  svn_pool_destroy(iterpool);
  svn_pool_destroy(localpool);

  return SVN_NO_ERROR;
}

/* Read the content of the pack file staring at revision BASE logical
 * addressing mode and store it in FS.  Use POOL for allocations.
 */
static svn_error_t *
read_log_pack_file(fs_t *fs,
                   svn_revnum_t base,
                   apr_pool_t *pool)
{
  SVN_ERR(read_log_rev_or_packfile(fs, base, fs->shard_size, pool));

  /* one more pack file processed */
  print_progress(base);

  return SVN_NO_ERROR;
}

/* Read the content of the file for REVISION in logical addressing mode
 * and store its contents in FS.  Use POOL for allocations.
 */
static svn_error_t *
read_log_revision_file(fs_t *fs,
                       svn_revnum_t revision,
                       apr_pool_t *pool)
{
  SVN_ERR(read_log_rev_or_packfile(fs, revision, 1, pool));

  /* show progress every 1000 revs or so */
  if (fs->shard_size && (revision % fs->shard_size == 0))
    print_progress(revision);
  if (!fs->shard_size && (revision % 1000 == 0))
    print_progress(revision);

  return SVN_NO_ERROR;
}

/* Read the repository at PATH and return the result in *FS.
 * Use POOL for allocations.
 */
static svn_error_t *
read_revisions(fs_t **fs,
               const char *path,
               apr_pool_t *pool)
{
  svn_revnum_t revision;

  /* determine cache sizes */
  SVN_ERR(fs_open(fs, path, pool));

  /* create data containers and caches
   * Note: this assumes that int is at least 32-bits and that we only support
   * 32-bit wide revision numbers (actually 31-bits due to the signedness
   * of both the nelts field of the array and our revision numbers). This
   * means this code will fail on platforms where int is less than 32-bits
   * and the repository has more revisions than int can hold. */
  (*fs)->revisions = apr_array_make(pool, (int) (*fs)->head + 1,
                                    sizeof(revision_info_t *));
  (*fs)->null_base = apr_pcalloc(pool, sizeof(*(*fs)->null_base));
  initialize_largest_changes(*fs, 64, pool);
  (*fs)->by_extension = apr_hash_make(pool);

  SVN_ERR(svn_cache__create_membuffer_cache(&(*fs)->window_cache,
                                            svn_cache__get_global_membuffer_cache(),
                                            NULL, NULL,
                                            sizeof(cache_key_t),
                                            "",
                                            SVN_CACHE__MEMBUFFER_DEFAULT_PRIORITY,
                                            FALSE, pool, pool));

  /* read all packed revs */
  for ( revision = 0
      ; revision < (*fs)->min_unpacked_rev
      ; revision += (*fs)->shard_size)
    if (svn_fs_fs__use_log_addressing((*fs)->fs, revision))
      SVN_ERR(read_log_pack_file(*fs, revision, pool));
    else
      SVN_ERR(read_phys_pack_file(*fs, revision, pool));

  /* read non-packed revs */
  for ( ; revision <= (*fs)->head; ++revision)
    if (svn_fs_fs__use_log_addressing((*fs)->fs, revision))
      SVN_ERR(read_log_revision_file(*fs, revision, pool));
    else
      SVN_ERR(read_phys_revision_file(*fs, revision, pool));

  return SVN_NO_ERROR;
}

/* Compression statistics we collect over a given set of representations.
 */
typedef struct rep_pack_stats_t
{
  /* number of representations */
  apr_int64_t count;

  /* total size after deltification (i.e. on disk size) */
  apr_int64_t packed_size;

  /* total size after de-deltification (i.e. plain text size) */
  apr_int64_t expanded_size;

  /* total on-disk header size */
  apr_int64_t overhead_size;
} rep_pack_stats_t;

/* Statistics we collect over a given set of representations.
 * We group them into shared and non-shared ("unique") reps.
 */
typedef struct representation_stats_t
{
  /* stats over all representations */
  rep_pack_stats_t total;

  /* stats over those representations with ref_count == 1 */
  rep_pack_stats_t uniques;

  /* stats over those representations with ref_count > 1 */
  rep_pack_stats_t shared;

  /* sum of all ref_counts */
  apr_int64_t references;

  /* sum of ref_count * expanded_size,
   * i.e. total plaintext content if there was no rep sharing */
  apr_int64_t expanded_size;
} representation_stats_t;

/* Basic statistics we collect over a given set of noderevs.
 */
typedef struct node_stats_t
{
  /* number of noderev structs */
  apr_int64_t count;

  /* their total size on disk (structs only) */
  apr_int64_t size;
} node_stats_t;

/* Accumulate stats of REP in STATS.
 */
static void
add_rep_pack_stats(rep_pack_stats_t *stats,
                   rep_stats_t *rep)
{
  stats->count++;

  stats->packed_size += rep->size;
  stats->expanded_size += rep->expanded_size;
  stats->overhead_size += rep->header_size + 7 /* ENDREP\n */;
}

/* Accumulate stats of REP in STATS.
 */
static void
add_rep_stats(representation_stats_t *stats,
              rep_stats_t *rep)
{
  add_rep_pack_stats(&stats->total, rep);
  if (rep->ref_count == 1)
    add_rep_pack_stats(&stats->uniques, rep);
  else
    add_rep_pack_stats(&stats->shared, rep);

  stats->references += rep->ref_count;
  stats->expanded_size += rep->ref_count * rep->expanded_size;
}

/* Print statistics for the given group of representations to console.
 * Use POOL for allocations.
 */
static void
print_rep_stats(representation_stats_t *stats,
                apr_pool_t *pool)
{
  printf(_("%20s bytes in %12s reps\n"
           "%20s bytes in %12s shared reps\n"
           "%20s bytes expanded size\n"
           "%20s bytes expanded shared size\n"
           "%20s bytes with rep-sharing off\n"
           "%20s shared references\n"),
         svn__i64toa_sep(stats->total.packed_size, ',', pool),
         svn__i64toa_sep(stats->total.count, ',', pool),
         svn__i64toa_sep(stats->shared.packed_size, ',', pool),
         svn__i64toa_sep(stats->shared.count, ',', pool),
         svn__i64toa_sep(stats->total.expanded_size, ',', pool),
         svn__i64toa_sep(stats->shared.expanded_size, ',', pool),
         svn__i64toa_sep(stats->expanded_size, ',', pool),
         svn__i64toa_sep(stats->references - stats->total.count, ',', pool));
}

/* Print the (used) contents of CHANGES.  Use POOL for allocations.
 */
static void
print_largest_reps(largest_changes_t *changes,
                   apr_pool_t *pool)
{
  apr_size_t i;
  for (i = 0; i < changes->count && changes->changes[i]->size; ++i)
    printf(_("%12s r%-8ld %s\n"),
           svn__i64toa_sep(changes->changes[i]->size, ',', pool),
           changes->changes[i]->revision,
           changes->changes[i]->path->data);
}

/* Print the non-zero section of HISTOGRAM to console.
 * Use POOL for allocations.
 */
static void
print_histogram(histogram_t *histogram,
                apr_pool_t *pool)
{
  int first = 0;
  int last = 63;
  int i;

  /* identify non-zero range */
  while (last > 0 && histogram->lines[last].count == 0)
    --last;

  while (first <= last && histogram->lines[first].count == 0)
    ++first;

  /* display histogram lines */
  for (i = last; i >= first; --i)
    printf(_("  [2^%2d, 2^%2d)   %15s (%2d%%) bytes in %12s (%2d%%) items\n"),
           i-1, i,
           svn__i64toa_sep(histogram->lines[i].sum, ',', pool),
           (int)(histogram->lines[i].sum * 100 / histogram->total.sum),
           svn__i64toa_sep(histogram->lines[i].count, ',', pool),
           (int)(histogram->lines[i].count * 100 / histogram->total.count));
}

/* COMPARISON_FUNC for svn_sort__hash.
 * Sort extension_info_t values by total count in descending order.
 */
static int
compare_count(const svn_sort__item_t *a,
              const svn_sort__item_t *b)
{
  const extension_info_t *lhs = a->value;
  const extension_info_t *rhs = b->value;
  apr_int64_t diff = lhs->node_histogram.total.count
                   - rhs->node_histogram.total.count;

  return diff > 0 ? -1 : (diff < 0 ? 1 : 0);
}

/* COMPARISON_FUNC for svn_sort__hash.
 * Sort extension_info_t values by total uncompressed size in descending order.
 */
static int
compare_node_size(const svn_sort__item_t *a,
                  const svn_sort__item_t *b)
{
  const extension_info_t *lhs = a->value;
  const extension_info_t *rhs = b->value;
  apr_int64_t diff = lhs->node_histogram.total.sum
                   - rhs->node_histogram.total.sum;

  return diff > 0 ? -1 : (diff < 0 ? 1 : 0);
}

/* COMPARISON_FUNC for svn_sort__hash.
 * Sort extension_info_t values by total prep count in descending order.
 */
static int
compare_rep_size(const svn_sort__item_t *a,
                 const svn_sort__item_t *b)
{
  const extension_info_t *lhs = a->value;
  const extension_info_t *rhs = b->value;
  apr_int64_t diff = lhs->rep_histogram.total.sum
                   - rhs->rep_histogram.total.sum;

  return diff > 0 ? -1 : (diff < 0 ? 1 : 0);
}

/* Return an array of extension_info_t* for the (up to) 16 most prominent
 * extensions in FS according to the sort criterion COMPARISON_FUNC.
 * Allocate results in POOL.
 */
static apr_array_header_t *
get_by_extensions(fs_t *fs,
                  int (*comparison_func)(const svn_sort__item_t *,
                                         const svn_sort__item_t *),
                  apr_pool_t *pool)
{
  /* sort all data by extension */
  apr_array_header_t *sorted
    = svn_sort__hash(fs->by_extension, comparison_func, pool);

  /* select the top (first) 16 entries */
  int count = MIN(sorted->nelts, 16);
  apr_array_header_t *result
    = apr_array_make(pool, count, sizeof(extension_info_t*));
  int i;

  for (i = 0; i < count; ++i)
    APR_ARRAY_PUSH(result, extension_info_t*)
     = APR_ARRAY_IDX(sorted, i, svn_sort__item_t).value;

  return result;
}

/* Add all extension_info_t* entries of TO_ADD not already in TARGET to
 * TARGET.
 */
static void
merge_by_extension(apr_array_header_t *target,
                   apr_array_header_t *to_add)
{
  int i, k, count;

  count = target->nelts;
  for (i = 0; i < to_add->nelts; ++i)
    {
      extension_info_t *info = APR_ARRAY_IDX(to_add, i, extension_info_t *);
      for (k = 0; k < count; ++k)
        if (info == APR_ARRAY_IDX(target, k, extension_info_t *))
          break;

      if (k == count)
        APR_ARRAY_PUSH(target, extension_info_t*) = info;
    }
}

/* Print the (up to) 16 extensions in FS with the most changes.
 * Use POOL for allocations.
 */
static void
print_extensions_by_changes(fs_t *fs,
                            apr_pool_t *pool)
{
  apr_array_header_t *data = get_by_extensions(fs, compare_count, pool);
  apr_int64_t sum = 0;
  int i;

  for (i = 0; i < data->nelts; ++i)
    {
      extension_info_t *info = APR_ARRAY_IDX(data, i, extension_info_t *);
      sum += info->node_histogram.total.count;
      printf(_("%11s %20s (%2d%%) representations\n"),
             info->extension,
             svn__i64toa_sep(info->node_histogram.total.count, ',', pool),
             (int)(info->node_histogram.total.count * 100 /
                   fs->file_histogram.total.count));
    }

  printf(_("%11s %20s (%2d%%) representations\n"),
         "(others)",
         svn__i64toa_sep(fs->file_histogram.total.count - sum, ',', pool),
         (int)((fs->file_histogram.total.count - sum) * 100 /
               fs->file_histogram.total.count));
}

/* Print the (up to) 16 extensions in FS with the largest total size of
 * changed file content.  Use POOL for allocations.
 */
static void
print_extensions_by_nodes(fs_t *fs,
                          apr_pool_t *pool)
{
  apr_array_header_t *data = get_by_extensions(fs, compare_node_size, pool);
  apr_int64_t sum = 0;
  int i;

  for (i = 0; i < data->nelts; ++i)
    {
      extension_info_t *info = APR_ARRAY_IDX(data, i, extension_info_t *);
      sum += info->node_histogram.total.sum;
      printf(_("%11s %20s (%2d%%) bytes\n"),
             info->extension,
             svn__i64toa_sep(info->node_histogram.total.sum, ',', pool),
             (int)(info->node_histogram.total.sum * 100 /
                   fs->file_histogram.total.sum));
    }

  printf(_("%11s %20s (%2d%%) bytes\n"),
         "(others)",
         svn__i64toa_sep(fs->file_histogram.total.sum - sum, ',', pool),
         (int)((fs->file_histogram.total.sum - sum) * 100 /
               fs->file_histogram.total.sum));
}

/* Print the (up to) 16 extensions in FS with the largest total size of
 * changed file content.  Use POOL for allocations.
 */
static void
print_extensions_by_reps(fs_t *fs,
                         apr_pool_t *pool)
{
  apr_array_header_t *data = get_by_extensions(fs, compare_rep_size, pool);
  apr_int64_t sum = 0;
  int i;

  for (i = 0; i < data->nelts; ++i)
    {
      extension_info_t *info = APR_ARRAY_IDX(data, i, extension_info_t *);
      sum += info->rep_histogram.total.sum;
      printf(_("%11s %20s (%2d%%) bytes\n"),
             info->extension,
             svn__i64toa_sep(info->rep_histogram.total.sum, ',', pool),
             (int)(info->rep_histogram.total.sum * 100 /
                   fs->rep_size_histogram.total.sum));
    }

  printf(_("%11s %20s (%2d%%) bytes\n"),
         "(others)",
         svn__i64toa_sep(fs->rep_size_histogram.total.sum - sum, ',', pool),
         (int)((fs->rep_size_histogram.total.sum - sum) * 100 /
               fs->rep_size_histogram.total.sum));
}

/* Print per-extension histograms for the most frequent extensions in FS.
 * Use POOL for allocations. */
static void
print_histograms_by_extension(fs_t *fs,
                              apr_pool_t *pool)
{
  apr_array_header_t *data = get_by_extensions(fs, compare_count, pool);
  int i;

  merge_by_extension(data, get_by_extensions(fs, compare_node_size, pool));
  merge_by_extension(data, get_by_extensions(fs, compare_rep_size, pool));

  for (i = 0; i < data->nelts; ++i)
    {
      extension_info_t *info = APR_ARRAY_IDX(data, i, extension_info_t *);
      printf("\nHistogram of '%s' file sizes:\n", info->extension);
      print_histogram(&info->node_histogram, pool);
      printf("\nHistogram of '%s' file representation sizes:\n",
             info->extension);
      print_histogram(&info->rep_histogram, pool);
    }
}

/* Post-process stats for FS and print them to the console.
 * Use POOL for allocations.
 */
static void
print_stats(fs_t *fs,
            apr_pool_t *pool)
{
  int i, k;

  /* initialize stats to collect */
  representation_stats_t file_rep_stats = { { 0 } };
  representation_stats_t dir_rep_stats = { { 0 } };
  representation_stats_t file_prop_rep_stats = { { 0 } };
  representation_stats_t dir_prop_rep_stats = { { 0 } };
  representation_stats_t total_rep_stats = { { 0 } };

  node_stats_t dir_node_stats = { 0 };
  node_stats_t file_node_stats = { 0 };
  node_stats_t total_node_stats = { 0 };

  apr_int64_t total_size = 0;
  apr_int64_t change_count = 0;
  apr_int64_t change_len = 0;

  /* aggregate info from all revisions */
  for (i = 0; i < fs->revisions->nelts; ++i)
    {
      revision_info_t *revision = APR_ARRAY_IDX(fs->revisions, i,
                                                revision_info_t *);

      /* data gathered on a revision level */
      change_count += revision->change_count;
      change_len += revision->changes_len;
      total_size += revision->end - revision->offset;

      dir_node_stats.count += revision->dir_noderev_count;
      dir_node_stats.size += revision->dir_noderev_size;
      file_node_stats.count += revision->file_noderev_count;
      file_node_stats.size += revision->file_noderev_size;
      total_node_stats.count += revision->dir_noderev_count
                              + revision->file_noderev_count;
      total_node_stats.size += revision->dir_noderev_size
                             + revision->file_noderev_size;

      /* process representations */
      for (k = 0; k < revision->representations->nelts; ++k)
        {
          rep_stats_t *rep = APR_ARRAY_IDX(revision->representations,
                                                k, rep_stats_t *);

          /* accumulate in the right bucket */
          switch(rep->kind)
            {
              case file_rep:
                add_rep_stats(&file_rep_stats, rep);
                break;
              case dir_rep:
                add_rep_stats(&dir_rep_stats, rep);
                break;
              case file_property_rep:
                add_rep_stats(&file_prop_rep_stats, rep);
                break;
              case dir_property_rep:
                add_rep_stats(&dir_prop_rep_stats, rep);
                break;
              default:
                break;
            }

          add_rep_stats(&total_rep_stats, rep);
        }
    }

  /* print results */
  printf("\nGlobal statistics:\n");
  printf(_("%20s bytes in %12s revisions\n"
           "%20s bytes in %12s changes\n"
           "%20s bytes in %12s node revision records\n"
           "%20s bytes in %12s representations\n"
           "%20s bytes expanded representation size\n"
           "%20s bytes with rep-sharing off\n"),
         svn__i64toa_sep(total_size, ',', pool),
         svn__i64toa_sep(fs->revisions->nelts, ',', pool),
         svn__i64toa_sep(change_len, ',', pool),
         svn__i64toa_sep(change_count, ',', pool),
         svn__i64toa_sep(total_node_stats.size, ',', pool),
         svn__i64toa_sep(total_node_stats.count, ',', pool),
         svn__i64toa_sep(total_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(total_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(total_rep_stats.total.expanded_size, ',', pool),
         svn__i64toa_sep(total_rep_stats.expanded_size, ',', pool));

  printf("\nNoderev statistics:\n");
  printf(_("%20s bytes in %12s nodes total\n"
           "%20s bytes in %12s directory noderevs\n"
           "%20s bytes in %12s file noderevs\n"),
         svn__i64toa_sep(total_node_stats.size, ',', pool),
         svn__i64toa_sep(total_node_stats.count, ',', pool),
         svn__i64toa_sep(dir_node_stats.size, ',', pool),
         svn__i64toa_sep(dir_node_stats.count, ',', pool),
         svn__i64toa_sep(file_node_stats.size, ',', pool),
         svn__i64toa_sep(file_node_stats.count, ',', pool));

  printf("\nRepresentation statistics:\n");
  printf(_("%20s bytes in %12s representations total\n"
           "%20s bytes in %12s directory representations\n"
           "%20s bytes in %12s file representations\n"
           "%20s bytes in %12s representations of added file nodes\n"
           "%20s bytes in %12s directory property representations\n"
           "%20s bytes in %12s file property representations\n"
           "%20s bytes in header & footer overhead\n"),
         svn__i64toa_sep(total_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(total_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(dir_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(dir_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(file_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(file_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(fs->added_rep_size_histogram.total.sum, ',', pool),
         svn__i64toa_sep(fs->added_rep_size_histogram.total.count, ',', pool),
         svn__i64toa_sep(dir_prop_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(dir_prop_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(file_prop_rep_stats.total.packed_size, ',', pool),
         svn__i64toa_sep(file_prop_rep_stats.total.count, ',', pool),
         svn__i64toa_sep(total_rep_stats.total.overhead_size, ',', pool));

  printf("\nDirectory representation statistics:\n");
  print_rep_stats(&dir_rep_stats, pool);
  printf("\nFile representation statistics:\n");
  print_rep_stats(&file_rep_stats, pool);
  printf("\nDirectory property representation statistics:\n");
  print_rep_stats(&dir_prop_rep_stats, pool);
  printf("\nFile property representation statistics:\n");
  print_rep_stats(&file_prop_rep_stats, pool);

  printf("\nLargest representations:\n");
  print_largest_reps(fs->largest_changes, pool);
  printf("\nExtensions by number of representations:\n");
  print_extensions_by_changes(fs, pool);
  printf("\nExtensions by size of changed files:\n");
  print_extensions_by_nodes(fs, pool);
  printf("\nExtensions by size of representations:\n");
  print_extensions_by_reps(fs, pool);

  printf("\nHistogram of expanded node sizes:\n");
  print_histogram(&fs->node_size_histogram, pool);
  printf("\nHistogram of representation sizes:\n");
  print_histogram(&fs->rep_size_histogram, pool);
  printf("\nHistogram of file sizes:\n");
  print_histogram(&fs->file_histogram, pool);
  printf("\nHistogram of file representation sizes:\n");
  print_histogram(&fs->file_rep_histogram, pool);
  printf("\nHistogram of file property sizes:\n");
  print_histogram(&fs->file_prop_histogram, pool);
  printf("\nHistogram of file property representation sizes:\n");
  print_histogram(&fs->file_prop_rep_histogram, pool);
  printf("\nHistogram of directory sizes:\n");
  print_histogram(&fs->dir_histogram, pool);
  printf("\nHistogram of directory representation sizes:\n");
  print_histogram(&fs->dir_rep_histogram, pool);
  printf("\nHistogram of directory property sizes:\n");
  print_histogram(&fs->dir_prop_histogram, pool);
  printf("\nHistogram of directory property representation sizes:\n");
  print_histogram(&fs->dir_prop_rep_histogram, pool);

  print_histograms_by_extension(fs, pool);
}

/* This implements `svn_opt_subcommand_t'. */
svn_error_t *
subcommand__stats(apr_getopt_t *os, void *baton, apr_pool_t *pool)
{
  svnfsfs__opt_state *opt_state = baton;
  fs_t *fs;

  printf("Reading revisions\n");
  SVN_ERR(read_revisions(&fs, opt_state->repository_path, pool));

  print_stats(fs, pool);

  return SVN_NO_ERROR;
}
