#!/usr/bin/env python
# ***** BEGIN LICENSE BLOCK *****
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.
# ***** END LICENSE BLOCK *****
"""Generic VCS support.
"""

import os
import sys
from copy import deepcopy

from mozharness.base.errors import VCSException
from mozharness.base.log import FATAL
from mozharness.base.script import BaseScript
from mozharness.base.vcs.gittool import GittoolVCS
from mozharness.base.vcs.mercurial import MercurialVCS

sys.path.insert(1, os.path.dirname(os.path.dirname(os.path.dirname(sys.path[0]))))


# Update this with supported VCS name : VCS object
VCS_DICT = {
    "hg": MercurialVCS,
    "gittool": GittoolVCS,
}


# VCSMixin {{{1
class VCSMixin(object):
    """Basic VCS methods that are vcs-agnostic.
    The vcs_class handles all the vcs-specific tasks.
    """

    def query_dest(self, kwargs):
        if "dest" in kwargs:
            return kwargs["dest"]
        dest = os.path.basename(kwargs["repo"])
        # Git fun
        if dest.endswith(".git"):
            dest = dest.replace(".git", "")
        return dest

    def _get_revision(self, vcs_obj, dest):
        try:
            got_revision = vcs_obj.ensure_repo_and_revision()
            if got_revision:
                return got_revision
        except VCSException:
            self.rmtree(dest)
            raise

    def _get_vcs_class(self, vcs):
        vcs = vcs or self.config.get("default_vcs", getattr(self, "default_vcs", None))
        vcs_class = VCS_DICT.get(vcs)
        return vcs_class

    def vcs_checkout(self, vcs=None, error_level=FATAL, **kwargs):
        """Check out a single repo."""
        c = self.config
        vcs_class = self._get_vcs_class(vcs)
        if not vcs_class:
            self.error("Running vcs_checkout with kwargs %s" % str(kwargs))
            raise VCSException("No VCS set!")
        # need a better way to do this.
        if "dest" not in kwargs:
            kwargs["dest"] = self.query_dest(kwargs)
        if "vcs_share_base" not in kwargs:
            kwargs["vcs_share_base"] = c.get(
                "%s_share_base" % vcs, c.get("vcs_share_base")
            )
        vcs_obj = vcs_class(
            log_obj=self.log_obj,
            config=self.config,
            vcs_config=kwargs,
            script_obj=self,
        )
        return self.retry(
            self._get_revision,
            error_level=error_level,
            error_message="Automation Error: Can't checkout %s!" % kwargs["repo"],
            args=(vcs_obj, kwargs["dest"]),
        )

    def vcs_checkout_repos(self, repo_list, parent_dir=None, **kwargs):
        """Check out a list of repos."""
        orig_dir = os.getcwd()
        c = self.config
        if not parent_dir:
            parent_dir = os.path.join(c["base_work_dir"], c["work_dir"])
        self.mkdir_p(parent_dir)
        self.chdir(parent_dir)
        revision_dict = {}
        kwargs_orig = deepcopy(kwargs)
        for repo_dict in repo_list:
            kwargs = deepcopy(kwargs_orig)
            kwargs.update(repo_dict)
            dest = self.query_dest(kwargs)
            revision_dict[dest] = {"repo": kwargs["repo"]}
            revision_dict[dest]["revision"] = self.vcs_checkout(**kwargs)
        self.chdir(orig_dir)
        return revision_dict

    def vcs_query_pushinfo(self, repository, revision, vcs=None):
        """Query the pushid/pushdate of a repository/revision
        Returns a namedtuple with "pushid" and "pushdate" elements
        """
        vcs_class = self._get_vcs_class(vcs)
        if not vcs_class:
            raise VCSException("No VCS set in vcs_query_pushinfo!")
        vcs_obj = vcs_class(
            log_obj=self.log_obj,
            config=self.config,
            script_obj=self,
        )
        return vcs_obj.query_pushinfo(repository, revision)


class VCSScript(VCSMixin, BaseScript):
    def __init__(self, **kwargs):
        super(VCSScript, self).__init__(**kwargs)

    def pull(self, repos=None, parent_dir=None):
        repos = repos or self.config.get("repos")
        if not repos:
            self.info("Pull has nothing to do!")
            return
        dirs = self.query_abs_dirs()
        parent_dir = parent_dir or dirs["abs_work_dir"]
        return self.vcs_checkout_repos(repos, parent_dir=parent_dir)


# Specific VCS stubs {{{1
# For ease of use.
# This is here instead of mercurial.py because importing MercurialVCS into
# vcsbase from mercurial, and importing VCSScript into mercurial from
# vcsbase, was giving me issues.
class MercurialScript(VCSScript):
    default_vcs = "hg"


# __main__ {{{1
if __name__ == "__main__":
    pass
