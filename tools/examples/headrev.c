/*
 * headrev.c :  print out the HEAD revision of a repository.
 *
 * ====================================================================
 * Copyright (c) 2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 *
 *  To compile on unix against Subversion and APR libraries, try
 *  something like:
 *
 *  cc headrev.c -o headrev \
 *  -I/usr/local/include/subversion-1 -I/usr/local/apache2/include \
 *  -L/usr/local/apache2/lib -L/usr/local/lib \
 *  -lsvn_client-1 -lsvn_ra-1 -lsvn_subr-1 -lapr-0 -laprutil-0
 *
 */

#include "svn_client.h"
#include "svn_pools.h"
#include "svn_config.h"
#include "svn_fs.h"


/* Display a prompt and read a one-line response into the provided buffer,
   removing a trailing newline if present. */
static svn_error_t *
prompt_and_read_line(const char *prompt,
                     char *buffer,
                     size_t max)
{
  int len;
  printf("%s: ", prompt);
  if (fgets(buffer, max, stdin) == NULL)
    return svn_error_create(0, NULL, "error reading stdin");
  len = strlen(buffer);
  if (len > 0 && buffer[len-1] == '\n')
    buffer[len-1] = 0;
  return SVN_NO_ERROR;
}

/* A tiny callback function of type 'svn_auth_simple_prompt_func_t'. For
   a much better example, see svn_cl__auth_simple_prompt in the official
   svn cmdline client. */
static svn_error_t *
my_simple_prompt_callback (svn_auth_cred_simple_t **cred,
                           void *baton,
                           const char *realm,
                           const char *username,
                           svn_boolean_t may_save,
                           apr_pool_t *pool)
{
  svn_auth_cred_simple_t *ret = apr_pcalloc (pool, sizeof (*ret));
  char answerbuf[100];

  if (realm)
    {
      printf ("Authentication realm: %s\n", realm);
    }

  if (username)
    ret->username = apr_pstrdup (pool, username);
  else
    {
      SVN_ERR (prompt_and_read_line("Username", answerbuf, sizeof(answerbuf)));
      ret->username = apr_pstrdup (pool, answerbuf);
    }

  SVN_ERR (prompt_and_read_line("Password", answerbuf, sizeof(answerbuf)));
  ret->password = apr_pstrdup (pool, answerbuf);

  *cred = ret;
  return SVN_NO_ERROR;
}


/* A tiny callback function of type 'svn_auth_username_prompt_func_t'. For
   a much better example, see svn_cl__auth_username_prompt in the official
   svn cmdline client. */
static svn_error_t *
my_username_prompt_callback (svn_auth_cred_username_t **cred,
                             void *baton,
                             const char *realm,
                             svn_boolean_t may_save,
                             apr_pool_t *pool)
{
  svn_auth_cred_username_t *ret = apr_pcalloc (pool, sizeof (*ret));
  char answerbuf[100];

  if (realm)
    {
      printf ("Authentication realm: %s\n", realm);
    }

  SVN_ERR (prompt_and_read_line("Username", answerbuf, sizeof(answerbuf)));
  ret->username = apr_pstrdup (pool, answerbuf);

  *cred = ret;
  return SVN_NO_ERROR;
}


int
main (int argc, const char **argv)
{
  apr_pool_t *pool;
  svn_error_t *err;
  apr_hash_t *dirents;
  apr_hash_index_t *hi;
  svn_client_ctx_t *ctx;
  const char *URL;
  svn_ra_plugin_t *ra_lib;  
  void *session, *ra_baton;
  svn_ra_callbacks_t *cbtable;
  svn_revnum_t rev;
  apr_hash_t *cfg_hash;
  svn_auth_baton_t *auth_baton;

  if (argc <= 1)
    {
      printf ("Usage:  %s URL\n", argv[0]);  
      printf ("    Print HEAD revision of URL's repository.\n");
      return EXIT_FAILURE;
    }
  else
    URL = argv[1];

  /* Initialize the app.  Send all error messages to 'stderr'.  */
  if (svn_cmdline_init ("headrev", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create top-level memory pool. Be sure to read the HACKING file to
     understand how to properly use/free subpools. */
  pool = svn_pool_create (NULL);

  /* Initialize the FS library. */
  err = svn_fs_initialize (pool);
  if (err) goto hit_error;

  /* Make sure the ~/.subversion run-time config files exist, and load. */  
  err = svn_config_ensure (NULL, pool);
  if (err) goto hit_error;

  err = svn_config_get_config (&cfg_hash, NULL, pool);
  if (err) goto hit_error;
    
  /* Build an authentication baton. */
  {
    /* There are many different kinds of authentication back-end
       "providers".  See svn_auth.h for a full overview. */
    svn_auth_provider_object_t *provider;
    apr_array_header_t *providers
      = apr_array_make (pool, 4, sizeof (svn_auth_provider_object_t *));
    
    svn_client_get_simple_prompt_provider (&provider,
                                           my_simple_prompt_callback,
                                           NULL, /* baton */
                                           2, /* retry limit */ pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
    
    svn_client_get_username_prompt_provider (&provider,
                                             my_username_prompt_callback,
                                             NULL, /* baton */
                                             2, /* retry limit */ pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
    
    /* Register the auth-providers into the context's auth_baton. */
    svn_auth_open (&auth_baton, providers, pool);      
  }

  /* Create a table of callbacks for the RA session, mostly nonexistent. */
  cbtable = apr_pcalloc (pool, sizeof(*cbtable));
  cbtable->auth_baton = auth_baton;

  /* Now do the real work. */

  err = svn_ra_init_ra_libs (&ra_baton, pool);
  if (err) goto hit_error;

  err = svn_ra_get_ra_library (&ra_lib, ra_baton, URL, pool);
  if (err) goto hit_error;
  
  err = ra_lib->open (&session, URL, cbtable, NULL, cfg_hash, pool);
  if (err) goto hit_error;

  err = ra_lib->get_latest_revnum (session, &rev, pool);
  if (err) goto hit_error;

  printf ("The latest revision is %ld.\n", rev);

  return EXIT_SUCCESS;

 hit_error:
  svn_handle_error2 (err, stderr, FALSE, "headrev: ");
  return EXIT_FAILURE;
}
