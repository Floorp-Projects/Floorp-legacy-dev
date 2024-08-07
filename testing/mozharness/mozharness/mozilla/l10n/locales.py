#!/usr/bin/env python
# ***** BEGIN LICENSE BLOCK *****
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.
# ***** END LICENSE BLOCK *****
"""Localization."""

import os
import pprint

from mozharness.base.config import parse_config_file


# LocalesMixin {{{1
class LocalesMixin(object):
    def __init__(self, **kwargs):
        """Mixins generally don't have an __init__.
        This breaks super().__init__() for children.
        However, this is needed to override the query_abs_dirs()
        """
        self.abs_dirs = None
        self.locales = None
        self.l10n_revisions = {}

    def query_locales(self):
        if self.locales is not None:
            return self.locales
        c = self.config
        ignore_locales = c.get("ignore_locales", [])
        additional_locales = c.get("additional_locales", [])
        # List of locales can be set by using different methods in the
        # following order:
        # 1. "MOZ_LOCALES" env variable: a string of locale:revision separated
        # by space
        # 2. self.config["locales"] which can be either coming from the config
        # or from --locale command line argument
        # 3. using self.config["locales_file"] l10n changesets file
        locales = None

        # Environment variable
        if not locales and "MOZ_LOCALES" in os.environ:
            self.debug("Using locales from environment: %s" % os.environ["MOZ_LOCALES"])
            locales = os.environ["MOZ_LOCALES"].split()

        # Command line or config
        if not locales and c.get("locales", []):
            locales = c["locales"]
            self.debug("Using locales from config/CLI: %s" % ", ".join(locales))

        # parse locale:revision if set
        if locales:
            for l in locales:
                if ":" in l:
                    # revision specified in locale string
                    locale, revision = l.split(":", 1)
                    self.debug("Using %s:%s" % (locale, revision))
                    self.l10n_revisions[locale] = revision
            # clean up locale by removing revisions
            locales = [l.split(":")[0] for l in locales]

        if not locales and "locales_file" in c:
            abs_dirs = self.query_abs_dirs()
            locales_file = os.path.join(abs_dirs["abs_src_dir"], c["locales_file"])
            locales = self.parse_locales_file(locales_file)

        if not locales:
            self.fatal("No locales set!")

        for locale in ignore_locales:
            if locale in locales:
                self.debug("Ignoring locale %s." % locale)
                locales.remove(locale)
                if locale in self.l10n_revisions:
                    del self.l10n_revisions[locale]

        for locale in additional_locales:
            if locale not in locales:
                self.debug("Adding locale %s." % locale)
                locales.append(locale)

        if not locales:
            return None
        self.locales = locales
        return self.locales

    def list_locales(self):
        """Stub action method."""
        self.info("Locale list: %s" % str(self.query_locales()))

    def parse_locales_file(self, locales_file):
        locales = []
        c = self.config
        self.info("Parsing locales file %s" % locales_file)
        platform = c.get("locales_platform", None)

        if locales_file.endswith("json"):
            locales_json = parse_config_file(locales_file)
            for locale in sorted(locales_json.keys()):
                if isinstance(locales_json[locale], dict):
                    if platform and platform not in locales_json[locale]["platforms"]:
                        continue
                    self.l10n_revisions[locale] = locales_json[locale]["revision"]
                else:
                    # some other way of getting this?
                    self.l10n_revisions[locale] = "default"
                locales.append(locale)
        else:
            locales = self.read_from_file(locales_file).split()
        self.info("self.l10n_revisions: %s" % pprint.pformat(self.l10n_revisions))
        self.info("locales: %s" % locales)
        return locales

    def query_abs_dirs(self):
        if self.abs_dirs:
            return self.abs_dirs
        abs_dirs = super(LocalesMixin, self).query_abs_dirs()
        c = self.config
        dirs = {}
        dirs["abs_work_dir"] = os.path.join(c["base_work_dir"], c["work_dir"])
        dirs["abs_l10n_dir"] = os.path.abspath(
            os.path.join(abs_dirs["abs_src_dir"], "../l10n-central")
        )
        dirs["abs_locales_src_dir"] = os.path.join(
            abs_dirs["abs_src_dir"],
            c["locales_dir"],
        )

        dirs["abs_obj_dir"] = os.path.join(dirs["abs_work_dir"], c["objdir"])
        dirs["abs_locales_dir"] = os.path.join(dirs["abs_obj_dir"], c["locales_dir"])

        for key in list(dirs.keys()):
            if key not in abs_dirs:
                abs_dirs[key] = dirs[key]
        self.abs_dirs = abs_dirs
        return self.abs_dirs

    # This requires self to inherit a VCSMixin.
    def pull_locale_source(self, hg_l10n_base=None, parent_dir=None):
        c = self.config
        git_repository = c.get("git_repository")
        if not hg_l10n_base:
            hg_l10n_base = c["hg_l10n_base"]
        if parent_dir is None:
            parent_dir = self.query_abs_dirs()["abs_l10n_dir"]
        self.mkdir_p(parent_dir)
        locales = self.query_locales()
        locale_repos = []
        if git_repository:
            # At the time this code was written we decided we have no use case
            # for separate revisions for different locales, and in fact, would
            # like to remove support for this from l10n-changesets.json
            # altogether. Because of this, we simply ensure that all revisions
            # given are the same, and clone the repository once at that
            # revision. This avoids unnecessary network operations and copies
            # on disk.
            revisions = set(self.l10n_revisions.values())
            if len(revisions) != 1:
                raise Exception(
                    "All l10n revisions must be the same when pulling from a git repository!"
                )

            self.vcs_checkout(
                vcs="gittool",
                repo=git_repository,
                dest=parent_dir,
                revision=revisions.pop(),
            )
        else:
            locale_repos = []
            for locale in locales:
                tag = c.get("hg_l10n_tag", "default")
                if self.l10n_revisions.get(locale):
                    tag = self.l10n_revisions[locale]
                locale_repos.append(
                    {
                        "repo": "%s/%s" % (hg_l10n_base, locale),
                        "branch": tag,
                        "vcs": "hg",
                    }
                )
            self.vcs_checkout_repos(
                repo_list=locale_repos,
                parent_dir=parent_dir,
            )


# __main__ {{{1

if __name__ == "__main__":
    pass
