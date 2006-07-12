/*
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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
 * svn_ra.i: SWIG interface file for svn_ra.h
 */

#if defined(SWIGPERL)
%module "SVN::_Ra"
#elif defined(SWIGRUBY)
%module "svn::ext::ra"
#else
%module ra
#endif

%include svn_global.swg
%import core.i
%import svn_delta.i

/* bad pool convention, also these should not be public interface at all
   as commented by sussman. */
%ignore svn_ra_svn_init;
%ignore svn_ra_local_init;
%ignore svn_ra_dav_init;
%ignore svn_ra_serf_init;

%apply Pointer NONNULL { svn_ra_callbacks2_t *callbacks };

/* -----------------------------------------------------------------------
   %apply-ing of typemaps defined elsewhere
*/
%apply apr_hash_t **DIRENTHASH { apr_hash_t **dirents };

%apply const char *MAY_BE_NULL {
    const char *comment,
    const char *lock_token
};

#ifdef SWIGPYTHON
%apply svn_stream_t *WRAPPED_STREAM { svn_stream_t * };
#endif

/* -----------------------------------------------------------------------
   thunk ra_callback
*/
%apply const char **OUTPUT {
    const char **url,
    const char **uuid
};

#ifdef SWIGPERL
%typemap(in) (const svn_delta_editor_t *update_editor,
              void *update_baton) {
    svn_delta_make_editor(&$1, &$2, $input, _global_pool);
}
%typemap(in) (const svn_delta_editor_t *diff_editor,
              void *diff_baton) {
    svn_delta_make_editor(&$1, &$2, $input, _global_pool);
}
#endif

#ifdef SWIGPERL
%typemap(in) (const svn_ra_callbacks_t *callbacks,
              void *callback_baton) {
    svn_ra_make_callbacks(&$1, &$2, $input, _global_pool);
}
#endif

#ifdef SWIGRUBY
%typemap(in) (const svn_ra_callbacks2_t *callbacks,
                    void *callback_baton)
{
  svn_swig_rb_setup_ra_callbacks(&$1, &$2, $input, _global_pool);
}
#endif

#ifdef SWIGPYTHON
%typemap(in) (const svn_ra_callbacks2_t *callbacks,
                      void *callback_baton) {
  svn_swig_py_setup_ra_callbacks(&$1, &$2, $input, _global_pool);
}
#endif

#ifdef SWIGRUBY
%typemap(in) (svn_ra_lock_callback_t lock_func, void *lock_baton)
{
  $1 = svn_swig_rb_ra_lock_callback;
  $2 = (void *)svn_swig_rb_make_baton($input, _global_svn_swig_rb_pool);
}

%typemap(in) (svn_ra_file_rev_handler_t handler, void *handler_baton)
{
  $1 = svn_swig_rb_ra_file_rev_handler;
  $2 = (void *)svn_swig_rb_make_baton($input, _global_svn_swig_rb_pool);
}
#endif

#ifdef SWIGPYTHON
%typemap(in) (svn_ra_file_rev_handler_t handler, void *handler_baton)
{
   $1 = svn_swig_py_ra_file_rev_handler_func;
   $2 = (void *)$input;
}
#endif

#ifdef SWIGPYTHON
%typemap(in) (const svn_ra_reporter2_t *reporter, void *report_baton)
{
  $1 = (svn_ra_reporter2_t *)&swig_py_ra_reporter2;
  $2 = (void *)$input;
}
#endif

/* ----------------------------------------------------------------------- */

%include svn_ra_h.swg
