/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_repos_private.h
 * @brief Subversion-internal repos APIs.
 */

#ifndef SVN_REPOS_PRIVATE_H
#define SVN_REPOS_PRIVATE_H

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_repos.h"
#include "svn_editor.h"
#include "svn_config.h"

#include "private/svn_string_private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/** Validate that property @a name with @a value is valid (as an addition
 * or edit or deletion) in a Subversion repository.  Return an error if not.
 *
 * If @a value is NULL, return #SVN_NO_ERROR to indicate that any property
 * may be deleted, even an invalid one.  Otherwise, if the @a name is not
 * of kind #svn_prop_regular_kind (see #svn_prop_kind_t), return
 * #SVN_ERR_REPOS_BAD_ARGS.  Otherwise, for some "svn:" properties, also
 * perform some validations on the @a value (e.g., for such properties,
 * typically the @a value must be in UTF-8 with LF linefeeds), and return
 * #SVN_ERR_BAD_PROPERTY_VALUE if it is not valid.
 *
 * Validations may be added in future releases, for example, for
 * newly-added #SVN_PROP_PREFIX properties.  However, user-defined
 * (non-#SVN_PROP_PREFIX) properties will never have their @a value
 * validated in any way.
 *
 * Use @a pool for temporary allocations.
 *
 * @note This function is used to implement server-side validation.
 * Consequently, if you make this function stricter in what it accepts, you
 * (a) break svnsync'ing of existing repositories that contain now-invalid
 * properties, (b) do not preclude such invalid values from entering the
 * repository via tools that use the svn_fs_* API directly (possibly
 * including svnadmin and svnlook).  This has happened before and there
 * are known (documented, but unsupported) upgrade paths in some cases.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_repos__validate_prop(const char *name,
                         const svn_string_t *value,
                         apr_pool_t *pool);

/**
 * Given the error @a err from svn_repos_fs_commit_txn(), return an
 * string containing either or both of the svn_fs_commit_txn() error
 * and the SVN_ERR_REPOS_POST_COMMIT_HOOK_FAILED wrapped error from
 * the post-commit hook.  Any error tracing placeholders in the error
 * chain are skipped over.
 *
 * This function does not modify @a err.
 *
 * ### This method should not be necessary, but there are a few
 * ### places, e.g. mod_dav_svn, where only a single error message
 * ### string is returned to the caller and it is useful to have both
 * ### error messages included in the message.
 *
 * Use @a pool to do any allocations in.
 *
 * @since New in 1.7.
 */
const char *
svn_repos__post_commit_error_str(svn_error_t *err,
                                 apr_pool_t *pool);

/* A repos version of svn_fs_type */
svn_error_t *
svn_repos__fs_type(const char **fs_type,
                   const char *repos_path,
                   apr_pool_t *pool);


/* Create a commit editor for REPOS, based on REVISION.  */
svn_error_t *
svn_repos__get_commit_ev2(svn_editor_t **editor,
                          svn_repos_t *repos,
                          svn_authz_t *authz,
                          const char *authz_repos_name,
                          const char *authz_user,
                          apr_hash_t *revprops,
                          svn_commit_callback2_t commit_cb,
                          void *commit_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);

svn_error_t *
svn_repos__replay_ev2(svn_fs_root_t *root,
                      const char *base_dir,
                      svn_revnum_t low_water_mark,
                      svn_editor_t *editor,
                      svn_repos_authz_func_t authz_read_func,
                      void *authz_read_baton,
                      apr_pool_t *scratch_pool);

/* Given a PATH which might be a relative repo URL (^/), an absolute
 * local repo URL (file://), an absolute path outside of the repo
 * or a location in the Windows registry.
 *
 * Retrieve the configuration data that PATH points at and parse it into
 * CFG_P allocated in POOL.
 *
 * If PATH cannot be parsed as a config file then an error is returned.  The
 * contents of CFG_P is then undefined.  If MUST_EXIST is TRUE, a missing
 * authz file is also an error.  The CASE_SENSITIVE controls the lookup
 * behavior for section and option names alike.
 *
 * REPOS_ROOT points at the root of the repos you are
 * going to apply the authz against, can be NULL if you are sure that you
 * don't have a repos relative URL in PATH. */
svn_error_t *
svn_repos__retrieve_config(svn_config_t **cfg_p,
                           const char *path,
                           svn_boolean_t must_exist,
                           svn_boolean_t case_sensitive,
                           apr_pool_t *pool);

/**
 * @defgroup svn_config_pool Configuration object pool API
 * @{
 */

/* Opaque thread-safe factory and container for configuration objects.
 *
 * Instances handed out are read-only and may be given to multiple callers
 * from multiple threads.  Configuration objects no longer referenced by
 * any user may linger for a while before being cleaned up.
 */
typedef struct svn_repos__config_pool_t svn_repos__config_pool_t;

/* Create a new configuration pool object with a lifetime determined by
 * POOL and return it in *CONFIG_POOL.
 * 
 * The THREAD_SAFE flag indicates whether the pool actually needs to be
 * thread-safe and POOL must be also be thread-safe if this flag is set.
 */
svn_error_t *
svn_repos__config_pool_create(svn_repos__config_pool_t **config_pool,
                              svn_boolean_t thread_safe,
                              apr_pool_t *pool);

/* Set *CFG to a read-only reference to the current contents of the
 * configuration specified by PATH.  If the latter is a URL, we read the
 * data from a local repository.  CONFIG_POOL will store the configuration
 * and make further callers use the same instance if the content matches.
 * If KEY is not NULL, *KEY will be set to a unique ID - if available.
 *
 * If MUST_EXIST is TRUE, a missing config file is also an error, *CFG
 * is otherwise simply NULL.  The CASE_SENSITIVE controls the lookup
 * behavior for section and option names alike.
 *
 * PREFERRED_REPOS is only used if it is not NULL and PATH is a URL.
 * If it matches the URL, access the repository through this object
 * instead of creating a new repo instance.  Note that this might not
 * return the latest content.
 *
 * POOL determines the minimum lifetime of *CFG (may remain cached after
 * release) but must not exceed the lifetime of the pool provided to
 * #svn_repos__config_pool_create.
 */
svn_error_t *
svn_repos__config_pool_get(svn_config_t **cfg,
                           svn_membuf_t **key,
                           svn_repos__config_pool_t *config_pool,
                           const char *path,
                           svn_boolean_t must_exist,
                           svn_boolean_t case_sensitive,
                           svn_repos_t *preferred_repos,
                           apr_pool_t *pool);

/** @} */

/**
 * @defgroup svn_authz_pool Authz object pool API
 * @{
 */

/* Opaque thread-safe factory and container for authorization objects.
 *
 * Instances handed out are read-only and may be given to multiple callers
 * from multiple threads.  Authorization objects no longer referenced by
 * any user may linger for a while before being cleaned up.
 */
typedef struct svn_repos__authz_pool_t svn_repos__authz_pool_t;

/* Create a new authorization pool object with a lifetime determined by
 * POOL and return it in *AUTHZ_POOL.  CONFIG_POOL will be the common
 * source for the configuration data underlying the authz objects and must
 * remain valid at least until POOL cleanup.
 *
 * The THREAD_SAFE flag indicates whether the pool actually needs to be
 * thread-safe and POOL must be also be thread-safe if this flag is set.
 */
svn_error_t *
svn_repos__authz_pool_create(svn_repos__authz_pool_t **authz_pool,
                             svn_repos__config_pool_t *config_pool,
                             svn_boolean_t thread_safe,
                             apr_pool_t *pool);

/* Set *AUTHZ_P to a read-only reference to the current contents of the
 * authorization specified by PATH and GROUPS_PATH.  If these are URLs,
 * we read the data from a local repository (see #svn_repos_authz_read2).
 * AUTHZ_POOL will store the authz data and make further callers use the
 * same instance if the content matches.
 *
 * If MUST_EXIST is TRUE, a missing config file is also an error, *AUTHZ_P
 * is otherwise simply NULL.
 *
 * PREFERRED_REPOS is only used if it is not NULL and PATH is a URL.
 * If it matches the URL, access the repository through this object
 * instead of creating a new repo instance.  Note that this might not
 * return the latest content.
 *
 * POOL determines the minimum lifetime of *AUTHZ_P (may remain cached
 * after release) but must not exceed the lifetime of the pool provided to
 * svn_repos__authz_pool_create.
 */
svn_error_t *
svn_repos__authz_pool_get(svn_authz_t **authz_p,
                          svn_repos__authz_pool_t *authz_pool,
                          const char *path,
                          const char *groups_path,
                          svn_boolean_t must_exist,
                          svn_repos_t *preferred_repos,
                          apr_pool_t *pool);

/** @} */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_REPOS_PRIVATE_H */
