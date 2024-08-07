#!/usr/bin/env python
# ***** BEGIN LICENSE BLOCK *****
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.
# ***** END LICENSE BLOCK *****
"""multi_locale_build.py

This should be a mostly generic multilocale build script.
"""

import os
import sys

from mozharness.base.errors import MakefileErrorList
from mozharness.base.vcs.vcsbase import MercurialScript
from mozharness.mozilla.l10n.locales import LocalesMixin

sys.path.insert(1, os.path.dirname(os.path.dirname(sys.path[0])))


# MultiLocaleBuild {{{1
class MultiLocaleBuild(LocalesMixin, MercurialScript):
    """This class targets Fennec multilocale builds.
    We were considering this for potential Firefox desktop multilocale.
    Now that we have a different approach for B2G multilocale,
    it's most likely misnamed."""

    config_options = [
        [
            ["--locale"],
            {
                "action": "extend",
                "dest": "locales",
                "type": "string",
                "help": "Specify the locale(s) to repack",
            },
        ],
        [
            ["--objdir"],
            {
                "action": "store",
                "dest": "objdir",
                "type": "string",
                "default": "objdir",
                "help": "Specify the objdir",
            },
        ],
        [
            ["--l10n-base"],
            {
                "action": "store",
                "dest": "hg_l10n_base",
                "type": "string",
                "help": "Specify the L10n repo base directory",
            },
        ],
        [
            ["--l10n-tag"],
            {
                "action": "store",
                "dest": "hg_l10n_tag",
                "type": "string",
                "help": "Specify the L10n tag",
            },
        ],
    ]

    def __init__(self, require_config_file=True):
        LocalesMixin.__init__(self)
        MercurialScript.__init__(
            self,
            config_options=self.config_options,
            all_actions=["pull-locale-source", "package-multi", "summary"],
            require_config_file=require_config_file,
        )

    # pull_locale_source() defined in LocalesMixin.

    def _run_mach_command(self, args):
        dirs = self.query_abs_dirs()

        mach = [sys.executable, "mach"]

        return_code = self.run_command(
            command=mach + ["--log-no-times"] + args,
            cwd=dirs["abs_src_dir"],
        )

        if return_code:
            self.fatal(
                "'mach %s' did not run successfully. Please check "
                "log for errors." % " ".join(args)
            )

    def package_multi(self):
        dirs = self.query_abs_dirs()
        objdir = dirs["abs_obj_dir"]

        # This will error on non-0 exit code.
        locales = list(sorted(self.query_locales()))
        self._run_mach_command(["package-multi-locale", "--locales"] + locales)

        command = "make package-tests AB_CD=multi"
        self.run_command(
            command, cwd=objdir, error_list=MakefileErrorList, halt_on_failure=True
        )
        # TODO deal with buildsymbols


# __main__ {{{1
if __name__ == "__main__":
    pass
