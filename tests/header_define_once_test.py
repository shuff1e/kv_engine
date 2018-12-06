#!/usr/bin/env python3

#     Copyright 2018 Couchbase, Inc
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

#   Test to check that KV-engine tests have a define only-once guard, e.g.
#   they contain a #pragma once directive or contain a #ifndef, #define
#   and #endif wrapper

from __future__ import print_function
import os
import sys
import argparse


def check_for_only_def_once(file):
    """
    Function to check for the presence of #pragma once or a #ifndef guard in a
    file
    :param file: file to search for #pragma once or (#ifndef X, #define X and
    #endif)
    :return: returns true if the correct macros are found or false if not
    """
    header_file = open(file, "r")
    source = header_file.read()
    # re-set the seek location to the beginning of the file as read()
    # will have made it point to the end
    header_file.seek(0)
    for line in header_file:
        # if there is a #pragma once then were good
        if line == str("#pragma once\n"):
            return True
        # otherwise look for #ifndef
        elif line.startswith("#ifndef"):
            # get hold of the defined name
            split_macros = line.split(" ")
            macros_name = split_macros[1]
            # bail if #ifndef WIN32 as this is a unique case
            if macros_name == "WIN32":
                continue
            # find the #define macrosName and the #endif
            if source.find(str("#define " + macros_name)) and \
                    source.find(("#endif /*  " + macros_name + " */")):
                return True
    return False


def find_and_check_headers_in_dir(top_level_dir, exclusions):
    """
    Function to iterate though all the header files in a directory and its
    sub-directories
    """
    # if check_for_only_def_once() ever returns false then this will be set to
    # false to indicate a failure
    test_pass = True

    for root, dirs, files in os.walk(top_level_dir):
        for current_file in files:
            full_path = os.path.join(root, current_file)
            if current_file.endswith(".h") and not (full_path in exclusions):
                if not check_for_only_def_once(full_path):
                    print("TEST FAIL - Header file found without "
                          "#pragma once\" or \"#ifndef\" wrapper: " +
                          full_path, file=sys.stderr)
                    test_pass = False
    return test_pass


# create argparser so the user can specify which files to exclude if needed
argParser = argparse.ArgumentParser()
argParser.add_argument('--rootdir',
                       metavar='/Users/user/source/couchbase/kv_engine',
                       type=str, nargs=1,
                       help='Directory to check header files in, defaults to '
                            'the current working directory.',
                       default=[os.getcwd()])
argParser.add_argument('--exclude',
                       metavar='engines/ep/src/tasks.def.h',
                       type=str, nargs='+',
                       help='List of files to exclude from checking, ' +
                            'defined by their path within --rootdir or ' +
                            'if --rootdir is not specified their path within' +
                            ' the working directory.',
                       default=[])

args = argParser.parse_args()

# get the grand-parent dir of the file which should be the kv_engine directory
dirToSearchForHeaders = args.rootdir[0]

if not os.path.isabs(dirToSearchForHeaders):
    dirToSearchForHeaders = os.path.abspath(dirToSearchForHeaders)
dirToSearchForHeaders = os.path.normpath(dirToSearchForHeaders)

listOfExclusions = args.exclude
# fully expand exclusion file paths
listOfExclusions = [
    os.path.normpath(
        os.path.abspath(os.path.join(dirToSearchForHeaders, path)))
    for path in listOfExclusions]

if find_and_check_headers_in_dir(dirToSearchForHeaders, listOfExclusions):
    exit(0)  # exit with 0 for pass
else:
    exit(1)  # exit with a general error code if there was a test failure