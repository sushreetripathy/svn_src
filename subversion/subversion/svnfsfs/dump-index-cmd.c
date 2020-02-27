/* dump-index-cmd.c -- implements the dump-index sub-command.
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
#include "svn_pools.h"

#include "../libsvn_fs_fs/fs.h"
#include "../libsvn_fs_fs/index.h"
#include "../libsvn_fs_fs/rev_file.h"
#include "../libsvn_fs_fs/util.h"
#include "../libsvn_fs/fs-loader.h"

#include "svnfsfs.h"

/* Return the 8 digit hex string for FNVV1, allocated in POOL.
 */
static const char *
fnv1_to_string(apr_uint32_t fnv1,
               apr_pool_t *pool)
{
  /* Construct a checksum object containing FNV1. */
  svn_checksum_t checksum = { NULL, svn_checksum_fnv1a_32 };
  apr_uint32_t digest = htonl(fnv1);
  checksum.digest = (const unsigned char *)&digest;

  /* Convert the digest to hex. */
  return svn_checksum_to_cstring_display(&checksum, pool);
}

/* Map svn_fs_fs__p2l_entry_t.type to C string. */
static const char *item_type_str[]
  = {"none ", "frep ", "drep ", "fprop", "dprop", "node ", "chgs ", "rep  "};

/* Read the repository at PATH beginning with revision START_REVISION and
 * return the result in *FS.  Allocate caches with MEMSIZE bytes total
 * capacity.  Use POOL for non-cache allocations.
 */
static svn_error_t *
dump_index(const char *path,
           svn_revnum_t revision,
           apr_pool_t *pool)
{
  svn_fs_fs__revision_file_t *rev_file;
  svn_fs_t *fs;
  int i;
  apr_off_t offset, max_offset;
  apr_pool_t *iterpool = svn_pool_create(pool);

  /* Check repository type and open it. */
  SVN_ERR(open_fs(&fs, path, pool));

  /* Check the FS format. */
  if (! svn_fs_fs__use_log_addressing(fs, revision))
    return svn_error_create(SVN_ERR_FS_UNSUPPORTED_FORMAT, NULL, NULL);

  /* Revision & index file access object. */
  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, fs, revision, pool,
                                           iterpool));

  /* Offset range to cover. */
  SVN_ERR(svn_fs_fs__p2l_get_max_offset(&max_offset, fs, rev_file, revision,
                                        pool));

  /* Write header line. */
  printf("       Start       Length Type   Revision     Item Checksum\n");

  /* Walk through all P2L index entries in offset order. */
  for (offset = 0; offset < max_offset; )
    {
      apr_array_header_t *entries;

      /* Read entries for the next block.  There will be no overlaps since
       * we start at the first offset not covered. */
      svn_pool_clear(iterpool);
      SVN_ERR(svn_fs_fs__p2l_index_lookup(&entries, fs, rev_file, revision,
                                          offset, INDEX_BLOCK_SIZE,
                                          iterpool));

      /* Print entries for this block, one line per entry. */
      for (i = 0; i < entries->nelts && offset < max_offset; ++i)
        {
          const svn_fs_fs__p2l_entry_t *entry
            = &APR_ARRAY_IDX(entries, i, const svn_fs_fs__p2l_entry_t);
          const char *type_str
            = entry->type < (sizeof(item_type_str) / sizeof(item_type_str[0]))
            ? item_type_str[entry->type]
            : "???";

          offset = entry->offset + entry->size;

          printf("%12" APR_UINT64_T_HEX_FMT " %12" APR_UINT64_T_HEX_FMT
                 " %s %9ld %8" APR_UINT64_T_FMT " %s\n",
                 (apr_uint64_t)entry->offset, (apr_uint64_t)entry->size,
                 type_str, entry->item.revision, entry->item.number,
                 fnv1_to_string(entry->fnv1_checksum, iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* This implements `svn_opt_subcommand_t'. */
svn_error_t *
subcommand__dump_index(apr_getopt_t *os, void *baton, apr_pool_t *pool)
{
  svnfsfs__opt_state *opt_state = baton;

  SVN_ERR(dump_index(opt_state->repository_path,
                     opt_state->start_revision.value.number, pool));

  return SVN_NO_ERROR;
}
