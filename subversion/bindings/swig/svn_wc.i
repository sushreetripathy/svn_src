/*
 * svn_wc.i :  SWIG interface file for svn_wc.h
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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
 */

%module _wc

%import apr.i
%import svn_types.i
%import svn_string.i
%import svn_delta.i

/* ### temporarily put this here rather than svn_types.i so that it
   ### doesn't interfere with 'svn_fs_id_t *' params in svn_fs.h */
%apply long *OUTPUT { svn_revnum_t * };

%include svn_wc.h
%{
#include "svn_wc.h"
%}
