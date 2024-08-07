# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

###
# This script generates a web manifest JSON file based on the xpi-stage
# directory structure. It extracts data necessary to produce the complete
# manifest file for a language pack:
# from the `langpack-manifest.ftl` file in the locale directory;
# from chrome registry entries;
# and from other information in the `xpi-stage` directory.
###

import argparse
import datetime
import io
import json
import logging
import os
import re
import sys
import time

import fluent.syntax.ast as FTL
import mozpack.path as mozpath
import mozversioncontrol
import requests
from fluent.syntax.parser import FluentParser
from hglib.error import ServerError
from mozpack.chrome.manifest import Manifest, ManifestLocale, parse_manifest

from mozbuild.configure.util import Version


def write_file(path, content):
    with io.open(path, "w", encoding="utf-8") as out:
        out.write(content + "\n")


pushlog_api_url = "{0}/json-rev/{1}"


def get_build_date():
    """Return the current date or SOURCE_DATE_EPOCH, if set."""
    return datetime.datetime.utcfromtimestamp(
        int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    )


###
# Retrieves a UTC datetime of the push for the current commit from a
# mercurial clone directory. The SOURCE_DATE_EPOCH environment
# variable is honored, for reproducibility.
#
# Args:
#    path (str) - path to a directory
#
# Returns:
#    (datetime) - a datetime object
#
# Example:
#    dt = get_dt_from_hg("/var/vcs/l10n-central/pl")
#    dt == datetime(2017, 10, 11, 23, 31, 54, 0)
###
def get_dt_from_hg(path):
    with mozversioncontrol.get_repository_object(path=path) as repo:
        phase = repo._run("log", "-r", ".", "-T" "{phase}")
        if phase.strip() != "public":
            return get_build_date()
        repo_url = repo._run("paths", "default")
        repo_url = repo_url.strip().replace("ssh://", "https://")
        repo_url = repo_url.replace("hg://", "https://")
        cs = repo._run("log", "-r", ".", "-T" "{node}")

    url = pushlog_api_url.format(repo_url, cs)
    session = requests.Session()
    try:
        response = session.get(url)
    except Exception as e:
        msg = "Failed to retrieve push timestamp using {}\nError: {}".format(url, e)
        raise Exception(msg)

    data = response.json()

    try:
        date = data["pushdate"][0]
    except KeyError as exc:
        msg = "{}\ndata is: {}".format(
            str(exc), json.dumps(data, indent=2, sort_keys=True)
        )
        raise KeyError(msg)

    return datetime.datetime.utcfromtimestamp(date)


###
# Generates timestamp for a locale based on its path.
# If possible, will use the commit timestamp from HG repository,
# and if that fails, will generate the timestamp for `now`.
#
# The timestamp format is "{year}{month}{day}{hour}{minute}{second}" and
# the datetime stored in it is using UTC timezone.
#
# Args:
#    path (str) - path to the locale directory
#
# Returns:
#    (str) - a timestamp string
#
# Example:
#    ts = get_timestamp_for_locale("/var/vcs/l10n-central/pl")
#    ts == "20170914215617"
###
def get_timestamp_for_locale(path):
    dt = None
    if os.path.isdir(os.path.join(path, ".hg")):
        dt = None
        # This can be removed once we're no longer repacking from hg l10n repos.
        try:
            dt = get_dt_from_hg(path)
        except ServerError as se:
            # This rare condition can happen if we try to repack from a
            # git l10n repository after having already repacked from an
            # hg l10n repository on the same machine.
            if "sharedpath points to nonexistent directory" not in str(se):
                raise se

    if dt is None:
        dt = get_build_date()

    dt = dt.replace(microsecond=0)
    return dt.strftime("%Y%m%d%H%M%S")


###
# Parses an FTL file into a key-value pair object.
# Does not support attributes, terms, variables, functions or selectors;
# only messages with values consisting of text elements and literals.
#
# Args:
#    path (str) - a path to an FTL file
#
# Returns:
#    (dict) - A mapping of message keys to formatted string values.
#             Empty if the file at `path` was not found.
#
# Example:
#    res = parse_flat_ftl('./browser/langpack-metadata.ftl')
#    res == {
#        'langpack-title': 'Polski',
#        'langpack-creator': 'mozilla.org',
#        'langpack-contributors': 'Joe Solon, Suzy Solon'
#    }
###
def parse_flat_ftl(path):
    parser = FluentParser(with_spans=False)
    try:
        with open(path, encoding="utf-8") as file:
            res = parser.parse(file.read())
    except FileNotFoundError as err:
        logging.warning(err)
        return {}

    result = {}
    for entry in res.body:
        if isinstance(entry, FTL.Message) and isinstance(entry.value, FTL.Pattern):
            flat = ""
            for elem in entry.value.elements:
                if isinstance(elem, FTL.TextElement):
                    flat += elem.value
                elif isinstance(elem.expression, FTL.Literal):
                    flat += elem.expression.parse()["value"]
                else:
                    name = type(elem.expression).__name__
                    raise Exception(f"Unsupported {name} for {entry.id.name} in {path}")
            result[entry.id.name] = flat.strip()
    return result


##
# Generates the title and description for the langpack.
#
# Uses data stored in a JSON file next to this source,
# which is expected to have the following format:
#   Record<string, { native: string, english?: string }>
#
# If an English name is given and is different from the native one,
# it will be included in the description and, if within the character limits,
# also in the name.
#
# Length limit for names is 45 characters, for descriptions is 132,
# return values are truncated if needed.
#
# NOTE: If you're updating the native locale names,
#       you should also update the data in
#       toolkit/components/mozintl/mozIntl.sys.mjs.
#
# Args:
#    app    (str) - Application name
#    locale (str) - Locale identifier
#
# Returns:
#    (str, str) - Tuple of title and description
#
###
def get_title_and_description(app, locale):
    dir = os.path.dirname(__file__)
    with open(os.path.join(dir, "langpack_localeNames.json"), encoding="utf-8") as nf:
        names = json.load(nf)

    nameCharLimit = 45
    descCharLimit = 132
    nameTemplate = "Language: {}"
    descTemplate = "{} Language Pack for {}"

    if locale in names:
        data = names[locale]
        native = data["native"]
        english = data["english"] if "english" in data else native

        if english != native:
            title = nameTemplate.format(f"{native} ({english})")
            if len(title) > nameCharLimit:
                title = nameTemplate.format(native)
            description = descTemplate.format(app, f"{native} ({locale}) – {english}")
        else:
            title = nameTemplate.format(native)
            description = descTemplate.format(app, f"{native} ({locale})")
    else:
        title = nameTemplate.format(locale)
        description = descTemplate.format(app, locale)

    return title[:nameCharLimit], description[:descCharLimit]


###
# Build the manifest author string based on the author string
# and optionally adding the list of contributors, if provided.
#
# Args:
#    ftl (dict) - a key-value mapping of locale-specific strings
#
# Returns:
#    (str) - a string to be placed in the author field of the manifest.json
#
# Example:
#    s = get_author({
#      'langpack-creator': 'mozilla.org',
#      'langpack-contributors': 'Joe Solon, Suzy Solon'
#    })
#    s == 'mozilla.org (contributors: Joe Solon, Suzy Solon)'
###
def get_author(ftl):
    author = ftl["langpack-creator"] if "langpack-creator" in ftl else "mozilla.org"
    contrib = ftl["langpack-contributors"] if "langpack-contributors" in ftl else ""
    if contrib:
        return f"{author} (contributors: {contrib})"
    else:
        return author


##
# Converts the list of chrome manifest entry flags to the list of platforms
# for the langpack manifest.
#
# The list of result platforms is taken from AppConstants.platform.
#
# Args:
#    flags (FlagList) - a list of Chrome Manifest entry flags
#
# Returns:
#    (list) - a list of platform the entry applies to
#
# Example:
#    str(flags) == "os==MacOS os==Windows"
#    platforms = convert_entry_flags_to_platform_codes(flags)
#    platforms == ['mac', 'win']
#
# The method supports only `os` flag name and equality operator.
# It will throw if tried with other flags or operators.
###
def convert_entry_flags_to_platform_codes(flags):
    if not flags:
        return None

    ret = []
    for key in flags:
        if key != "os":
            raise Exception("Unknown flag name")

        for value in flags[key].values:
            if value[0] != "==":
                raise Exception("Inequality flag cannot be converted")

            if value[1] == "Android":
                ret.append("android")
            elif value[1] == "LikeUnix":
                ret.append("linux")
            elif value[1] == "Darwin":
                ret.append("macosx")
            elif value[1] == "WINNT":
                ret.append("win")
            else:
                raise Exception("Unknown flag value {0}".format(value[1]))

    return ret


###
# Recursively parse a chrome manifest file appending new entries
# to the result list
#
# The function can handle two entry types: 'locale' and 'manifest'
#
# Args:
#    path           (str)  - a path to a chrome manifest
#    base_path      (str)  - a path to the base directory all chrome registry
#                            entries will be relative to
#    chrome_entries (list) - a list to which entries will be appended to
#
# Example:
#
#    chrome_entries = {}
#    parse_manifest('./chrome.manifest', './', chrome_entries)
#
#    chrome_entries == [
#        {
#            'type': 'locale',
#            'alias': 'devtools',
#            'locale': 'pl',
#            'platforms': null,
#            'path': 'chrome/pl/locale/pl/devtools/'
#        },
#        {
#            'type': 'locale',
#            'alias': 'autoconfig',
#            'locale': 'pl',
#            'platforms': ['win', 'mac'],
#            'path': 'chrome/pl/locale/pl/autoconfig/'
#        },
#    ]
###
def parse_chrome_manifest(path, base_path, chrome_entries):
    for entry in parse_manifest(None, path):
        if isinstance(entry, Manifest):
            parse_chrome_manifest(
                os.path.join(os.path.dirname(path), entry.relpath),
                base_path,
                chrome_entries,
            )
        elif isinstance(entry, ManifestLocale):
            entry_path = os.path.join(
                os.path.relpath(os.path.dirname(path), base_path), entry.relpath
            )
            chrome_entries.append(
                {
                    "type": "locale",
                    "alias": entry.name,
                    "locale": entry.id,
                    "platforms": convert_entry_flags_to_platform_codes(entry.flags),
                    "path": mozpath.normsep(entry_path),
                }
            )
        else:
            raise Exception("Unknown type {0}".format(entry.name))


###
# Gets the version to use in the langpack.
#
# This uses the env variable MOZ_BUILD_DATE if it exists to expand the version
# to be unique in automation.
#
# Args:
#    app_version - Application version
#
# Returns:
#    str - Version to use
#
###
def get_version_maybe_buildid(app_version):
    def _extract_numeric_part(part):
        matches = re.compile("[^\d]").search(part)
        if matches:
            part = part[0 : matches.start()]
        if len(part) == 0:
            return "0"
        return part

    parts = [_extract_numeric_part(part) for part in app_version.split(".")]

    buildid = os.environ.get("MOZ_BUILD_DATE")
    if buildid and len(buildid) != 14:
        print("Ignoring invalid MOZ_BUILD_DATE: %s" % buildid, file=sys.stderr)
        buildid = None

    if buildid:
        # Use simple versioning format, see: Bug 1793925 - The version string
        # should start with: <firefox major>.<firefox minor>
        version = ".".join(parts[0:2])
        # We then break the buildid into two version parts so that the full
        # version looks like: <firefox major>.<firefox minor>.YYYYMMDD.HHmmss
        date, time = buildid[:8], buildid[8:]
        # Leading zeros are not allowed.
        time = time.lstrip("0")
        if len(time) == 0:
            time = "0"
        version = f"{version}.{date}.{time}"
    else:
        version = ".".join(parts)

    return version


###
# Generates a new web manifest dict with values specific for a language pack.
#
# Args:
#    locstr         (str)  - A string with a comma separated list of locales
#                            for which resources are embedded in the
#                            language pack
#    min_app_ver    (str)  - A minimum version of the application the language
#                            resources are for
#    max_app_ver    (str)  - A maximum version of the application the language
#                            resources are for
#    app_name       (str)  - The name of the application the language
#                            resources are for
#    ftl            (dict) - A dictionary of locale-specific strings
#    chrome_entries (dict) - A dictionary of chrome registry entries
#
# Returns:
#    (dict) - a web manifest
#
# Example:
#    manifest = create_webmanifest(
#      'pl',
#      '57.0',
#      '57.0.*',
#      'Firefox',
#      '/var/vcs/l10n-central',
#      {'langpack-title': 'Polski'},
#      chrome_entries
#    )
#    manifest == {
#        'languages': {
#            'pl': {
#                'version': '201709121481',
#                'chrome_resources': {
#                    'alert': 'chrome/pl/locale/pl/alert/',
#                    'branding': 'browser/chrome/pl/locale/global/',
#                    'global-platform': {
#                      'macosx': 'chrome/pl/locale/pl/global-platform/mac/',
#                      'win': 'chrome/pl/locale/pl/global-platform/win/',
#                      'linux': 'chrome/pl/locale/pl/global-platform/unix/',
#                      'android': 'chrome/pl/locale/pl/global-platform/unix/',
#                    },
#                    'forms': 'browser/chrome/pl/locale/forms/',
#                    ...
#                }
#            }
#        },
#        'sources': {
#            'browser': {
#                'base_path': 'browser/'
#            }
#        },
#        'browser_specific_settings': {
#            'gecko':  {
#                'strict_min_version': '57.0',
#                'strict_max_version': '57.0.*',
#                'id': 'langpack-pl@mozilla.org',
#            }
#        },
#        'version': '57.0',
#        'name': 'Polski Language Pack',
#        ...
#    }
###
def create_webmanifest(
    locstr,
    version,
    min_app_ver,
    max_app_ver,
    app_name,
    l10n_basedir,
    langpack_eid,
    ftl,
    chrome_entries,
):
    locales = list(map(lambda loc: loc.strip(), locstr.split(",")))
    main_locale = locales[0]
    title, description = get_title_and_description(app_name, main_locale)
    author = get_author(ftl)

    manifest = {
        "langpack_id": main_locale,
        "manifest_version": 2,
        "browser_specific_settings": {
            "gecko": {
                "id": langpack_eid,
                "strict_min_version": min_app_ver,
                "strict_max_version": max_app_ver,
            }
        },
        "name": title,
        "description": description,
        "version": get_version_maybe_buildid(version),
        "languages": {},
        "sources": {"browser": {"base_path": "browser/"}},
        "author": author,
    }

    cr = {}
    for entry in chrome_entries:
        if entry["type"] == "locale":
            platforms = entry["platforms"]
            if platforms:
                if entry["alias"] not in cr:
                    cr[entry["alias"]] = {}
                for platform in platforms:
                    cr[entry["alias"]][platform] = entry["path"]
            else:
                assert entry["alias"] not in cr
                cr[entry["alias"]] = entry["path"]
        else:
            raise Exception("Unknown type {0}".format(entry["type"]))

    for loc in locales:
        manifest["languages"][loc] = {
            "version": get_timestamp_for_locale(os.path.join(l10n_basedir, loc)),
            "chrome_resources": cr,
        }

    return json.dumps(manifest, indent=2, ensure_ascii=False)


def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--locales", help="List of language codes provided by the langpack"
    )
    parser.add_argument("--app-version", help="Version of the application")
    parser.add_argument(
        "--max-app-ver", help="Max version of the application the langpack is for"
    )
    parser.add_argument(
        "--app-name", help="Name of the application the langpack is for"
    )
    parser.add_argument(
        "--l10n-basedir", help="Base directory for locales used in the language pack"
    )
    parser.add_argument(
        "--langpack-eid", help="Language pack id to use for this locale"
    )
    parser.add_argument(
        "--metadata",
        help="FTL file defining langpack metadata",
    )
    parser.add_argument("--input", help="Langpack directory.")

    args = parser.parse_args(args)

    chrome_entries = []
    parse_chrome_manifest(
        os.path.join(args.input, "chrome.manifest"), args.input, chrome_entries
    )

    ftl = parse_flat_ftl(args.metadata)

    # Mangle the app version to set min version (remove patch level)
    min_app_version = args.app_version
    if "a" not in min_app_version:  # Don't mangle alpha versions
        v = Version(min_app_version)
        if args.app_name == "SeaMonkey":
            # SeaMonkey is odd in that <major> hasn't changed for many years.
            # So min is <major>.<minor>.0
            min_app_version = "{}.{}.0".format(v.major, v.minor)
        else:
            # Language packs should be minversion of {major}.0
            min_app_version = "{}.0".format(v.major)

    res = create_webmanifest(
        args.locales,
        args.app_version,
        min_app_version,
        args.max_app_ver,
        args.app_name,
        args.l10n_basedir,
        args.langpack_eid,
        ftl,
        chrome_entries,
    )
    write_file(os.path.join(args.input, "manifest.json"), res)


if __name__ == "__main__":
    main(sys.argv[1:])
