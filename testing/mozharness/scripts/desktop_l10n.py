#!/usr/bin/env python
# ***** BEGIN LICENSE BLOCK *****
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.
# ***** END LICENSE BLOCK *****
"""desktop_l10n.py

This script manages Desktop repacks for nightly builds.
"""

import glob
import os
import shlex
import sys

# load modules from parent dir
sys.path.insert(1, os.path.dirname(sys.path[0]))  # noqa

from mozharness.base.errors import MakefileErrorList
from mozharness.base.script import BaseScript
from mozharness.base.vcs.vcsbase import VCSMixin
from mozharness.mozilla.automation import AutomationMixin
from mozharness.mozilla.building.buildbase import (
    MakeUploadOutputParser,
    get_mozconfig_path,
)
from mozharness.mozilla.l10n.locales import LocalesMixin

try:
    import simplejson as json

    assert json
except ImportError:
    import json


# needed by _map
SUCCESS = 0
FAILURE = 1

SUCCESS_STR = "Success"
FAILURE_STR = "Failed"


# DesktopSingleLocale {{{1
class DesktopSingleLocale(LocalesMixin, AutomationMixin, VCSMixin, BaseScript):
    """Manages desktop repacks"""

    config_options = [
        [
            [
                "--locale",
            ],
            {
                "action": "extend",
                "dest": "locales",
                "type": "string",
                "help": "Specify the locale(s) to sign and update. Optionally pass"
                " revision separated by colon, en-GB:default.",
            },
        ],
        [
            [
                "--en-us-installer-url",
            ],
            {
                "action": "store",
                "dest": "en_us_installer_url",
                "type": "string",
                "help": "Specify the url of the en-us binary",
            },
        ],
    ]

    def __init__(self, require_config_file=True):
        # fxbuild style:
        buildscript_kwargs = {
            "all_actions": [
                "clone-locales",
                "list-locales",
                "setup",
                "repack",
                "summary",
            ],
            "config": {
                "ignore_locales": ["en-US"],
                "locales_dir": "browser/locales",
                "log_name": "single_locale",
                "hg_l10n_base": "https://hg.mozilla.org/l10n-central",
                # If set, takes precedence over `hg_l10n_base`
                "git_repository": "https://github.com/mozilla-l10n/firefox-l10n",
            },
        }

        LocalesMixin.__init__(self)
        BaseScript.__init__(
            self,
            config_options=self.config_options,
            require_config_file=require_config_file,
            **buildscript_kwargs,
        )

        self.bootstrap_env = None
        self.upload_env = None
        self.upload_urls = {}
        self.pushdate = None
        # upload_files is a dictionary of files to upload, keyed by locale.
        self.upload_files = {}

    # Helper methods {{{2
    def query_bootstrap_env(self):
        """returns the env for repacks"""
        if self.bootstrap_env:
            return self.bootstrap_env
        config = self.config
        abs_dirs = self.query_abs_dirs()

        bootstrap_env = self.query_env(
            partial_env=config.get("bootstrap_env"), replace_dict=abs_dirs
        )

        bootstrap_env["L10NBASEDIR"] = abs_dirs["abs_l10n_dir"]
        if self.query_is_nightly():
            # we might set update_channel explicitly
            if config.get("update_channel"):
                update_channel = config["update_channel"]
            else:  # Let's just give the generic channel based on branch.
                update_channel = "nightly-%s" % (config["branch"],)
            if not isinstance(update_channel, bytes):
                update_channel = update_channel.encode("utf-8")
            bootstrap_env["MOZ_UPDATE_CHANNEL"] = update_channel
            self.info(
                "Update channel set to: {}".format(bootstrap_env["MOZ_UPDATE_CHANNEL"])
            )
        self.bootstrap_env = bootstrap_env
        return self.bootstrap_env

    def _query_upload_env(self):
        """returns the environment used for the upload step"""
        if self.upload_env:
            return self.upload_env
        config = self.config

        upload_env = self.query_env(partial_env=config.get("upload_env"))
        # check if there are any extra option from the platform configuration
        # and append them to the env

        if "upload_env_extra" in config:
            for extra in config["upload_env_extra"]:
                upload_env[extra] = config["upload_env_extra"][extra]

        self.upload_env = upload_env
        return self.upload_env

    def query_l10n_env(self):
        l10n_env = self._query_upload_env().copy()
        l10n_env.update(self.query_bootstrap_env())
        return l10n_env

    def _query_make_variable(self, variable, make_args=None):
        """returns the value of make echo-variable-<variable>
        it accepts extra make arguements (make_args)
        """
        dirs = self.query_abs_dirs()
        make_args = make_args or []
        target = ["echo-variable-%s" % variable] + make_args
        cwd = dirs["abs_locales_dir"]
        raw_output = self._get_output_from_make(
            target, cwd=cwd, env=self.query_bootstrap_env()
        )
        # we want to log all the messages from make
        output = []
        for line in raw_output.split("\n"):
            output.append(line.strip())
        output = " ".join(output).strip()
        self.info("echo-variable-%s: %s" % (variable, output))
        return output

    def _map(self, func, items):
        """runs func for any item in items, calls the add_failure() for each
        error. It assumes that function returns 0 when successful.
        returns a two element tuple with (success_count, total_count)"""
        success_count = 0
        total_count = len(items)
        name = func.__name__
        for item in items:
            result = func(item)
            if result == SUCCESS:
                #  success!
                success_count += 1
            else:
                #  func failed...
                message = "failure: %s(%s)" % (name, item)
                self.add_failure(item, message)
        return (success_count, total_count)

    # Actions {{{2
    def clone_locales(self):
        self.pull_locale_source()

    def setup(self):
        """setup step"""
        self._run_tooltool()
        self._copy_mozconfig()
        self._mach_configure()
        self._run_make_in_config_dir()
        self.make_wget_en_US()
        self.make_unpack_en_US()

    def _run_make_in_config_dir(self):
        """this step creates nsinstall, needed my make_wget_en_US()"""
        dirs = self.query_abs_dirs()
        config_dir = os.path.join(dirs["abs_obj_dir"], "config")
        env = self.query_bootstrap_env()
        return self._make(target=["export"], cwd=config_dir, env=env)

    def _copy_mozconfig(self):
        """copies the mozconfig file into abs_src_dir/.mozconfig
        and logs the content
        """
        config = self.config
        dirs = self.query_abs_dirs()
        src = get_mozconfig_path(self, config, dirs)
        dst = os.path.join(dirs["abs_src_dir"], ".mozconfig")
        self.copyfile(src, dst)
        self.read_from_file(dst, verbose=True)

    def _mach(self, target, env, halt_on_failure=True, output_parser=None):
        dirs = self.query_abs_dirs()
        mach = self._get_mach_executable()
        return self.run_command(
            mach + target,
            halt_on_failure=True,
            env=env,
            cwd=dirs["abs_src_dir"],
            output_parser=None,
        )

    def _mach_configure(self):
        """calls mach configure"""
        env = self.query_bootstrap_env()
        target = ["configure"]
        return self._mach(target=target, env=env)

    def _get_mach_executable(self):
        return [sys.executable, "mach"]

    def _make(
        self,
        target,
        cwd,
        env,
        error_list=MakefileErrorList,
        halt_on_failure=True,
        output_parser=None,
    ):
        """Runs make. Returns the exit code"""
        make = ["make"]
        if target:
            make = make + target
        return self.run_command(
            make,
            cwd=cwd,
            env=env,
            error_list=error_list,
            halt_on_failure=halt_on_failure,
            output_parser=output_parser,
        )

    def _get_output_from_make(
        self, target, cwd, env, halt_on_failure=True, ignore_errors=False
    ):
        """runs make and returns the output of the command"""
        return self.get_output_from_command(
            ["make"] + target,
            cwd=cwd,
            env=env,
            silent=True,
            halt_on_failure=halt_on_failure,
            ignore_errors=ignore_errors,
        )

    def make_unpack_en_US(self):
        """wrapper for make unpack"""
        config = self.config
        dirs = self.query_abs_dirs()
        env = self.query_bootstrap_env()
        cwd = os.path.join(dirs["abs_obj_dir"], config["locales_dir"])
        return self._make(target=["unpack"], cwd=cwd, env=env)

    def make_wget_en_US(self):
        """wrapper for make wget-en-US"""
        env = self.query_bootstrap_env()
        dirs = self.query_abs_dirs()
        cwd = dirs["abs_locales_dir"]
        return self._make(target=["wget-en-US"], cwd=cwd, env=env)

    def make_upload(self, locale):
        """wrapper for make upload command"""
        env = self.query_l10n_env()
        dirs = self.query_abs_dirs()
        target = ["upload", "AB_CD=%s" % (locale)]
        cwd = dirs["abs_locales_dir"]
        parser = MakeUploadOutputParser(config=self.config, log_obj=self.log_obj)
        retval = self._make(
            target=target, cwd=cwd, env=env, halt_on_failure=False, output_parser=parser
        )
        if retval == SUCCESS:
            self.info("Upload successful (%s)" % locale)
            ret = SUCCESS
        else:
            self.error("failed to upload %s" % locale)
            ret = FAILURE

        if ret == FAILURE:
            # If we failed above, we shouldn't even attempt a SIMPLE_NAME move
            # even if we are configured to do so
            return ret

        # XXX Move the files to a SIMPLE_NAME format until we can enable
        #     Simple names in the build system
        if self.config.get("simple_name_move"):
            # Assume an UPLOAD PATH
            upload_target = self.config["upload_env"]["UPLOAD_PATH"]
            target_path = os.path.join(upload_target, locale)
            self.mkdir_p(target_path)
            glob_name = "*.%s.*" % locale
            matches = (
                glob.glob(os.path.join(upload_target, glob_name))
                + glob.glob(os.path.join(upload_target, "update", glob_name))
                + glob.glob(os.path.join(upload_target, "*", "xpi", glob_name))
                + glob.glob(os.path.join(upload_target, "install", "sea", glob_name))
                + glob.glob(os.path.join(upload_target, "setup.exe"))
                + glob.glob(os.path.join(upload_target, "setup-stub.exe"))
            )
            targets_exts = [
                "tar.bz2",
                "dmg",
                "langpack.xpi",
                "checksums",
                "zip",
                "installer.exe",
                "installer-stub.exe",
            ]
            targets = [(".%s" % (ext,), "target.%s" % (ext,)) for ext in targets_exts]
            targets.extend([(f, f) for f in ("setup.exe", "setup-stub.exe")])
            for f in matches:
                possible_targets = [
                    (tail, target_file)
                    for (tail, target_file) in targets
                    if f.endswith(tail)
                ]
                if len(possible_targets) == 1:
                    _, target_file = possible_targets[0]
                    # Remove from list of available options for this locale
                    targets.remove(possible_targets[0])
                else:
                    # wasn't valid (or already matched)
                    raise RuntimeError(
                        "Unexpected matching file name encountered: %s" % f
                    )
                self.move(os.path.join(f), os.path.join(target_path, target_file))
            self.log("Converted uploads for %s to simple names" % locale)
        return ret

    def set_upload_files(self, locale):
        # The tree doesn't have a good way of exporting the list of files
        # created during locale generation, but we can grab them by echoing the
        # UPLOAD_FILES variable for each locale.
        env = self.query_l10n_env()
        target = [
            "echo-variable-UPLOAD_FILES",
            "echo-variable-CHECKSUM_FILES",
            "AB_CD=%s" % locale,
        ]
        dirs = self.query_abs_dirs()
        cwd = dirs["abs_locales_dir"]
        # Bug 1242771 - echo-variable-UPLOAD_FILES via mozharness fails when stderr is found
        #    we should ignore stderr as unfortunately it's expected when parsing for values
        output = self._get_output_from_make(
            target=target, cwd=cwd, env=env, ignore_errors=True
        )
        self.info('UPLOAD_FILES is "%s"' % output)
        files = shlex.split(output)
        if not files:
            self.error("failed to get upload file list for locale %s" % locale)
            return FAILURE

        self.upload_files[locale] = [
            os.path.abspath(os.path.join(cwd, f)) for f in files
        ]
        return SUCCESS

    def make_installers(self, locale):
        """wrapper for make installers-(locale)"""
        env = self.query_l10n_env()
        env["PYTHONIOENCODING"] = "utf-8"
        self._copy_mozconfig()
        dirs = self.query_abs_dirs()
        cwd = os.path.join(dirs["abs_locales_dir"])
        target = [
            "installers-%s" % locale,
        ]
        return self._make(target=target, cwd=cwd, env=env, halt_on_failure=False)

    def repack_locale(self, locale):
        """wraps the logic for make installers and generating
        complete updates."""

        # run make installers
        if self.make_installers(locale) != SUCCESS:
            self.error("make installers-%s failed" % (locale))
            return FAILURE

        # now try to upload the artifacts
        if self.make_upload(locale):
            self.error("make upload for locale %s failed!" % (locale))
            return FAILURE

        # set_upload_files() should be called after make upload, to make sure
        # we have all files in place (checksums, etc)
        if self.set_upload_files(locale):
            self.error("failed to get list of files to upload for locale %s" % locale)
            return FAILURE

        return SUCCESS

    def repack(self):
        """creates the repacks and udpates"""
        self._map(self.repack_locale, self.query_locales())

    def _run_tooltool(self):
        env = self.query_bootstrap_env()
        config = self.config
        dirs = self.query_abs_dirs()
        manifest_src = os.environ.get("TOOLTOOL_MANIFEST")
        if not manifest_src:
            manifest_src = config.get("tooltool_manifest_src")
        if not manifest_src:
            return
        python = sys.executable

        cmd = [
            python,
            "-u",
            os.path.join(dirs["abs_src_dir"], "mach"),
            "artifact",
            "toolchain",
            "-v",
            "--retry",
            "4",
            "--artifact-manifest",
            os.path.join(dirs["abs_src_dir"], "toolchains.json"),
        ]
        if manifest_src:
            cmd.extend(
                [
                    "--tooltool-manifest",
                    os.path.join(dirs["abs_src_dir"], manifest_src),
                ]
            )
        cache = config["bootstrap_env"].get("TOOLTOOL_CACHE")
        if cache:
            cmd.extend(["--cache-dir", cache])
        self.info(str(cmd))
        self.run_command(cmd, cwd=dirs["abs_src_dir"], halt_on_failure=True, env=env)


# main {{{
if __name__ == "__main__":
    single_locale = DesktopSingleLocale()
    single_locale.run_and_exit()
