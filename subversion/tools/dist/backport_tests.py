#!/usr/bin/env python
# py:encoding=utf-8
#
#  backport_tests.py:  Test backport.pl
#
#  Subversion is a tool for revision control.
#  See http://subversion.apache.org for more information.
#
# ====================================================================
#    Licensed to the Apache Software Foundation (ASF) under one
#    or more contributor license agreements.  See the NOTICE file
#    distributed with this work for additional information
#    regarding copyright ownership.  The ASF licenses this file
#    to you under the Apache License, Version 2.0 (the
#    "License"); you may not use this file except in compliance
#    with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing,
#    software distributed under the License is distributed on an
#    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#    KIND, either express or implied.  See the License for the
#    specific language governing permissions and limitations
#    under the License.
######################################################################

# General modules
import contextlib
import functools
import os
import re
import sys

@contextlib.contextmanager
def chdir(dir):
  try:
    saved_dir = os.getcwd()
    os.chdir(dir)
    yield
  finally:
    os.chdir(saved_dir)

# Our testing module
# HACK: chdir to cause svntest.main.svn_binary to be set correctly
sys.path.insert(0, os.path.abspath('../../subversion/tests/cmdline'))
with chdir('../../subversion/tests/cmdline'):
  import svntest

# (abbreviations)
Skip = svntest.testcase.Skip_deco
SkipUnless = svntest.testcase.SkipUnless_deco
XFail = svntest.testcase.XFail_deco
Issues = svntest.testcase.Issues_deco
Issue = svntest.testcase.Issue_deco
Wimp = svntest.testcase.Wimp_deco

######################################################################
# Helper functions

BACKPORT_PL = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                           'backport.pl'))
STATUS = 'branch/STATUS'

class BackportTest(object):
  """Decorator.  See self.__call__()."""

  def __init__(self, uuid):
    """The argument is the UUID embedded in the dump file.
    If the argument is None, then there is no dump file."""
    self.uuid = uuid

  def __call__(self, test_func):
    """Return a decorator that: builds TEST_FUNC's sbox, creates
    ^/subversion/trunk, and calls TEST_FUNC, then compare its output to the
    expected dump file named after TEST_FUNC."""
  
    # .wraps() propagates the wrappee's docstring to the wrapper.
    @functools.wraps(test_func) 
    def wrapped_test_func(sbox):
      expected_dump_file = './%s.dump' % (test_func.func_name,)

      sbox.build()
  
      # r2: prepare ^/subversion/ tree
      sbox.simple_mkdir('subversion', 'subversion/trunk')
      sbox.simple_mkdir('subversion/tags', 'subversion/branches')
      sbox.simple_move('A', 'subversion/trunk')
      sbox.simple_move('iota', 'subversion/trunk')
      sbox.simple_commit(message='Create trunk')
  
      # r3: branch
      sbox.simple_copy('subversion/trunk', 'branch')
      sbox.simple_append('branch/STATUS', '')
      sbox.simple_add('branch/STATUS')
      sbox.simple_commit(message='Create branch, with STATUS file')
  
      # r4: random change on trunk
      sbox.simple_append('subversion/trunk/iota', 'First change\n')
      sbox.simple_commit(message='First change')
  
      # r5: random change on trunk
      sbox.simple_append('subversion/trunk/A/mu', 'Second change\n')
      sbox.simple_commit(message='Second change')
  
      # Do the work.
      test_func(sbox)
  
      # Verify it.
      verify_backport(sbox, expected_dump_file, self.uuid)
    return wrapped_test_func

def make_entry(revisions=None, logsummary=None, notes=None, branch=None, votes=None):
  assert revisions
  if logsummary is None:
    logsummary = "default logsummary"
  if votes is None:
    votes = {+1 : ['jrandom']}

  entry = {
    'revisions': revisions,
    'logsummary': logsummary,
    'notes': notes,
    'branch': branch,
    'votes': votes,
  }

  return entry

def serialize_entry(entry):
  return ''.join([

    # revisions,
    ' * %s\n'
    % (", ".join("r%ld" % revision for revision in entry['revisions'])),

    # logsummary
    '   %s\n' % (entry['logsummary'],),

    # notes
    '   Notes: %s\n' % (entry['notes'],) if entry['notes'] else '',
     
    # branch
    '   Branch: %s\n' % (entry['branch'],) if entry['branch'] else '',

    # votes
    '   Votes:\n',
    ''.join('     '
        '%s: %s\n' % ({1: '+1', 0: '+0', -1: '-1', -0: '-0'}[vote],
                      ", ".join(entry['votes'][vote]))
        for vote in entry['votes']),

    '\n', # empty line after entry
  ])

def serialize_STATUS(approveds,
                     serialize_entry=serialize_entry):
  """Construct and return the contents of a STATUS file.

  APPROVEDS is an iterable of ENTRY dicts.  The dicts are defined
  to have the following keys: 'revisions', a list of revision numbers (ints);
  'logsummary'; and 'votes', a dict mapping ±1/±0 (int) to list of voters.
  """

  strings = []
  strings.append("Status of 1.8.x:\n\n")

  strings.append("Candidate changes:\n")
  strings.append("==================\n\n")

  strings.append("Random new subheading:\n")
  strings.append("======================\n\n")

  strings.append("Veto-blocked changes:\n")
  strings.append("=====================\n\n")

  strings.append("Approved changes:\n")
  strings.append("=================\n\n")

  strings.extend(map(serialize_entry, approveds))

  return "".join(strings)

def run_backport(sbox, error_expected=False, extra_env=[]):
  """Run backport.pl.  EXTRA_ENV is a list of key=value pairs (str) to set in
  the child's environment.  ERROR_EXPECTED is propagated to run_command()."""
  # TODO: if the test is run in verbose mode, pass DEBUG=1 in the environment,
  #       and pass error_expected=True to run_command() to not croak on
  #       stderr output from the child (because it uses 'sh -x').
  args = [
    '/usr/bin/env',
    'SVN=' + svntest.main.svn_binary,
    'YES=1', 'MAY_COMMIT=1', 'AVAILID=jrandom',
  ] + list(extra_env) + [
    'perl', BACKPORT_PL,
  ]
  with chdir(sbox.ospath('branch')):
    return svntest.main.run_command(args[0], error_expected, False, *(args[1:]))

def verify_backport(sbox, expected_dump_file, uuid):
  """Compare the contents of the SBOX repository with EXPECTED_DUMP_FILE.
  Set the UUID of SBOX to UUID beforehand.
  Based on svnsync_tests.py:verify_mirror."""

  if uuid is None:
    # There is no expected dump file.
    return

  # Remove some SVNSync-specific housekeeping properties from the
  # mirror repository in preparation for the comparison dump.
  svntest.actions.enable_revprop_changes(sbox.repo_dir)
  for revnum in range(0, 1+int(sbox.youngest())):
    svntest.actions.run_and_verify_svnadmin(None, [], [],
      "delrevprop", "-r", revnum, sbox.repo_dir, "svn:date")

  # Create a dump file from the mirror repository.
  dest_dump = open(expected_dump_file).readlines()
  svntest.actions.run_and_verify_svnadmin(None, None, [],
                                          'setuuid', '--', sbox.repo_dir, uuid)
  src_dump = svntest.actions.run_and_verify_dump(sbox.repo_dir)

  svntest.verify.compare_dump_files(
    "Dump files", "DUMP", src_dump, dest_dump)

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.

#----------------------------------------------------------------------
@BackportTest('76cee987-25c9-4d6c-ad40-000000000001')
def backport_indented_entry(sbox):
  "parsing of entries with nonstandard indentation"

  # r6: nominate r4
  approved_entries = [
    make_entry([4]),
  ]
  def reindenting_serialize_entry(*args, **kwargs):
    entry = serialize_entry(*args, **kwargs)
    return ('\n' + entry).replace('\n ', '\n')[1:]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries,
                                              serialize_entry=reindenting_serialize_entry))
  sbox.simple_commit(message='Nominate r4')

  # Run it.
  run_backport(sbox)


#----------------------------------------------------------------------
@BackportTest('76cee987-25c9-4d6c-ad40-000000000002')
def backport_two_approveds(sbox):
  "backport with two approveds"

  # r6: Enter votes
  approved_entries = [
    make_entry([4]),
    make_entry([5]),
  ]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries))
  sbox.simple_commit(message='Nominate r4.  Nominate r5.')

  # r7, r8: Run it.
  run_backport(sbox)

  # Now back up and do three entries.
  # r9: revert r7, r8
  svntest.actions.run_and_verify_svnlook(None, ["8\n"], [],
                                         'youngest', sbox.repo_dir)
  sbox.simple_update()
  svntest.main.run_svn(None, 'merge', '-r8:6',
                       '^/branch', sbox.ospath('branch'))
  sbox.simple_commit(message='Revert the merges.')

  # r10: Another change on trunk.
  # (Note that this change must be merged after r5.)
  sbox.simple_rm('subversion/trunk/A')
  sbox.simple_commit(message='Third change on trunk.')

  # r11: Nominate r10.
  sbox.simple_append(STATUS, serialize_entry(make_entry([10])))
  sbox.simple_commit(message='Nominate r10.')

  # r12, r13, r14: Run it.
  run_backport(sbox)



#----------------------------------------------------------------------
@BackportTest('76cee987-25c9-4d6c-ad40-000000000003')
def backport_accept(sbox):
  "test --accept parsing"

  # r6: conflicting change on branch
  sbox.simple_append('branch/iota', 'Conflicts with first change\n')
  sbox.simple_commit(message="Conflicting change on iota")
  
  # r7: nominate r4 with --accept (because of r6)
  approved_entries = [
    make_entry([4], notes="Merge with --accept=theirs-conflict."),
  ]
  def reindenting_serialize_entry(*args, **kwargs):
    entry = serialize_entry(*args, **kwargs)
    return ('\n' + entry).replace('\n ', '\n')[1:]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries,
                                              serialize_entry=reindenting_serialize_entry))
  sbox.simple_commit(message='Nominate r4')

  # Run it.
  run_backport(sbox)


#----------------------------------------------------------------------
@BackportTest('76cee987-25c9-4d6c-ad40-000000000004')
def backport_branches(sbox):
  "test branches"

  # r6: conflicting change on branch
  sbox.simple_append('branch/iota', 'Conflicts with first change')
  sbox.simple_commit(message="Conflicting change on iota")
  
  # r7: backport branch
  sbox.simple_update()
  sbox.simple_copy('branch', 'subversion/branches/r4')
  sbox.simple_commit(message='Create a backport branch')

  # r8: merge into backport branch
  sbox.simple_update()
  svntest.main.run_svn(None, 'merge', '--record-only', '-c4',
                       '^/subversion/trunk', sbox.ospath('subversion/branches/r4'))
  sbox.simple_mkdir('subversion/branches/r4/A_resolved')
  sbox.simple_append('subversion/branches/r4/iota', "resolved\n", truncate=1)
  sbox.simple_commit(message='Conflict resolution via mkdir')

  # r9: nominate r4 with branch
  approved_entries = [
    make_entry([4], branch="r4")
  ]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries))
  sbox.simple_commit(message='Nominate r4')

  # Run it.
  run_backport(sbox)


#----------------------------------------------------------------------
@BackportTest('76cee987-25c9-4d6c-ad40-000000000005')
def backport_multirevisions(sbox):
  "test multirevision entries"

  # r6: nominate r4,r5
  approved_entries = [
    make_entry([4,5])
  ]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries))
  sbox.simple_commit(message='Nominate a group.')

  # Run it.
  run_backport(sbox)


#----------------------------------------------------------------------
@BackportTest(None) # would be 000000000006
def backport_conflicts_detection(sbox):
  "test the conflicts detector"

  # r6: conflicting change on branch
  sbox.simple_append('branch/iota', 'Conflicts with first change\n')
  sbox.simple_commit(message="Conflicting change on iota")
  
  # r7: nominate r4, but without the requisite --accept
  approved_entries = [
    make_entry([4], notes="This will conflict."),
  ]
  sbox.simple_append(STATUS, serialize_STATUS(approved_entries))
  sbox.simple_commit(message='Nominate r4')

  # Run it.
  exit_code, output, errput = run_backport(sbox, True,
                                           # Choose conflicts mode:
                                           ["MAY_COMMIT=0"])

  # Verify
  expected_errput = (
    r'(?ms)' # re.MULTILINE | re.DOTALL
    r'.*Warning summary.*'
    r'^r4 [(]default logsummary[)]: Conflicts on iota.*'
  )
  expected_errput = svntest.verify.RegexListOutput(
                      [
                        r'Warning summary',
                        r'===============',
                        r'r4 [(]default logsummary[)]: Conflicts on iota',
                      ],
                      match_all=False)
  svntest.verify.verify_outputs(None, output, errput,
                                svntest.verify.AnyOutput, expected_errput)
  svntest.verify.verify_exit_code(None, exit_code, 1)


#----------------------------------------------------------------------

########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              backport_indented_entry,
              backport_two_approveds,
              backport_accept,
              backport_branches,
              backport_multirevisions,
              backport_conflicts_detection,
              # When adding a new test, include the test number in the last
              # 6 bytes of the UUID.
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
