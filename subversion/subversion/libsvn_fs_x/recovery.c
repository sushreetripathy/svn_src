/* recovery.c --- FSX recovery functionality
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

#include "recovery.h"

#include "svn_hash.h"
#include "svn_pools.h"
#include "private/svn_string_private.h"

#include "low_level.h"
#include "rep-cache.h"
#include "revprops.h"
#include "transaction.h"
#include "util.h"
#include "cached_data.h"
#include "index.h"

#include "../libsvn_fs/fs-loader.h"

#include "svn_private_config.h"

/* Part of the recovery procedure.  Return the largest revision *REV in
   filesystem FS.  Use POOL for temporary allocation. */
static svn_error_t *
recover_get_largest_revision(svn_fs_t *fs, svn_revnum_t *rev, apr_pool_t *pool)
{
  /* Discovering the largest revision in the filesystem would be an
     expensive operation if we did a readdir() or searched linearly,
     so we'll do a form of binary search.  left is a revision that we
     know exists, right a revision that we know does not exist. */
  apr_pool_t *iterpool;
  svn_revnum_t left, right = 1;

  iterpool = svn_pool_create(pool);
  /* Keep doubling right, until we find a revision that doesn't exist. */
  while (1)
    {
      svn_error_t *err;
      apr_file_t *file;
      svn_pool_clear(iterpool);

      err = svn_fs_x__open_pack_or_rev_file(&file, fs, right, iterpool);
      if (err && err->apr_err == SVN_ERR_FS_NO_SUCH_REVISION)
        {
          svn_error_clear(err);
          break;
        }
      else
        SVN_ERR(err);

      right <<= 1;
    }

  left = right >> 1;

  /* We know that left exists and right doesn't.  Do a normal bsearch to find
     the last revision. */
  while (left + 1 < right)
    {
      svn_revnum_t probe = left + ((right - left) / 2);
      svn_error_t *err;
      apr_file_t *file;
      svn_pool_clear(iterpool);

      err = svn_fs_x__open_pack_or_rev_file(&file, fs, probe, iterpool);
      if (err && err->apr_err == SVN_ERR_FS_NO_SUCH_REVISION)
        {
          svn_error_clear(err);
          right = probe;
        }
      else
        {
          SVN_ERR(err);
          left = probe;
        }
    }

  svn_pool_destroy(iterpool);

  /* left is now the largest revision that exists. */
  *rev = left;
  return SVN_NO_ERROR;
}

/* Baton used for recover_body below. */
struct recover_baton {
  svn_fs_t *fs;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;
};

/* The work-horse for svn_fs_x__recover, called with the FS
   write lock.  This implements the svn_fs_x__with_write_lock()
   'body' callback type.  BATON is a 'struct recover_baton *'. */
static svn_error_t *
recover_body(void *baton, apr_pool_t *pool)
{
  struct recover_baton *b = baton;
  svn_fs_t *fs = b->fs;
  fs_x_data_t *ffd = fs->fsap_data;
  svn_revnum_t max_rev;
  svn_revnum_t youngest_rev;
  svn_node_kind_t youngest_revprops_kind;

  /* Lose potentially corrupted data in temp files */
  SVN_ERR(svn_fs_x__cleanup_revprop_namespace(fs));

  /* We need to know the largest revision in the filesystem. */
  SVN_ERR(recover_get_largest_revision(fs, &max_rev, pool));

  /* Get the expected youngest revision */
  SVN_ERR(svn_fs_x__youngest_rev(&youngest_rev, fs, pool));

  /* Policy note:

     Since the revprops file is written after the revs file, the true
     maximum available revision is the youngest one for which both are
     present.  That's probably the same as the max_rev we just found,
     but if it's not, we could, in theory, repeatedly decrement
     max_rev until we find a revision that has both a revs and
     revprops file, then write db/current with that.

     But we choose not to.  If a repository is so corrupt that it's
     missing at least one revprops file, we shouldn't assume that the
     youngest revision for which both the revs and revprops files are
     present is healthy.  In other words, we're willing to recover
     from a missing or out-of-date db/current file, because db/current
     is truly redundant -- it's basically a cache so we don't have to
     find max_rev each time, albeit a cache with unusual semantics,
     since it also officially defines when a revision goes live.  But
     if we're missing more than the cache, it's time to back out and
     let the admin reconstruct things by hand: correctness at that
     point may depend on external things like checking a commit email
     list, looking in particular working copies, etc.

     This policy matches well with a typical naive backup scenario.
     Say you're rsyncing your FSX repository nightly to the same
     location.  Once revs and revprops are written, you've got the
     maximum rev; if the backup should bomb before db/current is
     written, then db/current could stay arbitrarily out-of-date, but
     we can still recover.  It's a small window, but we might as well
     do what we can. */

  /* Even if db/current were missing, it would be created with 0 by
     get_youngest(), so this conditional remains valid. */
  if (youngest_rev > max_rev)
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Expected current rev to be <= %ld "
                               "but found %ld"), max_rev, youngest_rev);

  /* Before setting current, verify that there is a revprops file
     for the youngest revision.  (Issue #2992) */
  SVN_ERR(svn_io_check_path(svn_fs_x__path_revprops(fs, max_rev, pool),
                            &youngest_revprops_kind, pool));
  if (youngest_revprops_kind == svn_node_none)
    {
      svn_boolean_t missing = TRUE;
      if (!svn_fs_x__packed_revprop_available(&missing, fs, max_rev, pool))
        {
          if (missing)
            {
              return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                                      _("Revision %ld has a revs file but no "
                                        "revprops file"),
                                      max_rev);
            }
          else
            {
              return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                                      _("Revision %ld has a revs file but the "
                                        "revprops file is inaccessible"),
                                      max_rev);
            }
          }
    }
  else if (youngest_revprops_kind != svn_node_file)
    {
      return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                               _("Revision %ld has a non-file where its "
                                 "revprops file should be"),
                               max_rev);
    }

  /* Prune younger-than-(newfound-youngest) revisions from the rep
     cache if sharing is enabled taking care not to create the cache
     if it does not exist. */
  if (ffd->rep_sharing_allowed)
    {
      svn_boolean_t rep_cache_exists;

      SVN_ERR(svn_fs_x__exists_rep_cache(&rep_cache_exists, fs, pool));
      if (rep_cache_exists)
        SVN_ERR(svn_fs_x__del_rep_reference(fs, max_rev, pool));
    }

  /* Now store the discovered youngest revision, and the next IDs if
     relevant, in a new 'current' file. */
  return svn_fs_x__write_current(fs, max_rev, pool);
}

/* This implements the fs_library_vtable_t.recover() API. */
svn_error_t *
svn_fs_x__recover(svn_fs_t *fs,
                  svn_cancel_func_t cancel_func, void *cancel_baton,
                  apr_pool_t *pool)
{
  struct recover_baton b;

  /* We have no way to take out an exclusive lock in FSX, so we're
     restricted as to the types of recovery we can do.  Luckily,
     we just want to recreate the 'current' file, and we can do that just
     by blocking other writers. */
  b.fs = fs;
  b.cancel_func = cancel_func;
  b.cancel_baton = cancel_baton;
  return svn_fs_x__with_all_locks(fs, recover_body, &b, pool);
}
