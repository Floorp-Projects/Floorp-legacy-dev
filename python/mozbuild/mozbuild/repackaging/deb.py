# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

import datetime
import json
import logging
import os
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
from email.utils import format_datetime
from pathlib import Path
from string import Template

import mozfile
import mozpack.path as mozpath
import requests
from mozilla_version.gecko import GeckoVersion
from redo import retry

from mozbuild.repackaging.application_ini import get_application_ini_values


class NoDebPackageFound(Exception):
    """Raised when no .deb is found after calling dpkg-buildpackage"""

    def __init__(self, deb_file_path) -> None:
        super().__init__(
            f"No {deb_file_path} package found after calling dpkg-buildpackage"
        )


class HgServerError(Exception):
    """Raised when Hg responds with an error code that is not 404 (i.e. when there is an outage)"""

    def __init__(self, msg) -> None:
        super().__init__(msg)


_DEB_ARCH = {
    "all": "all",
    "x86": "i386",
    "x86_64": "amd64",
}
# At the moment the Firefox build baseline is jessie.
# The debian-repackage image defined in taskcluster/docker/debian-repackage/Dockerfile
# bootstraps the /srv/jessie-i386 and /srv/jessie-amd64 chroot environments we use to
# create the `.deb` repackages. By running the repackage using chroot we generate shared
# library dependencies that match the Firefox build baseline
# defined in taskcluster/scripts/misc/build-sysroot.sh
_DEB_DIST = "jessie"


def repackage_deb(
    log,
    infile,
    output,
    template_dir,
    arch,
    version,
    build_number,
    release_product,
    release_type,
    fluent_localization,
    fluent_resource_loader,
):
    if not tarfile.is_tarfile(infile):
        raise Exception("Input file %s is not a valid tarfile." % infile)

    tmpdir = _create_temporary_directory(arch)
    source_dir = os.path.join(tmpdir, "source")
    try:
        mozfile.extract_tarball(infile, source_dir)
        application_ini_data = _extract_application_ini_data(infile)
        build_variables = _get_build_variables(
            application_ini_data,
            arch,
            version,
            build_number,
            depends="${shlibs:Depends},",
        )

        _copy_plain_deb_config(template_dir, source_dir)
        _render_deb_templates(template_dir, source_dir, build_variables)

        app_name = application_ini_data["name"]
        with open(
            mozpath.join(source_dir, app_name.lower(), "is-packaged-app"), "w"
        ) as f:
            f.write("This is a packaged app.\n")

        _inject_deb_distribution_folder(source_dir, app_name)
        _inject_deb_desktop_entry_file(
            log,
            source_dir,
            build_variables,
            release_product,
            release_type,
            fluent_localization,
            fluent_resource_loader,
        )
        _generate_deb_archive(
            source_dir,
            target_dir=tmpdir,
            output_file_path=output,
            build_variables=build_variables,
            arch=arch,
        )

    finally:
        shutil.rmtree(tmpdir)


def repackage_deb_l10n(
    input_xpi_file, input_tar_file, output, template_dir, version, build_number
):
    arch = "all"

    tmpdir = _create_temporary_directory(arch)
    source_dir = os.path.join(tmpdir, "source")
    try:
        langpack_metadata = _extract_langpack_metadata(input_xpi_file)
        langpack_dir = mozpath.join(source_dir, "firefox", "distribution", "extensions")
        application_ini_data = _extract_application_ini_data(input_tar_file)
        langpack_id = langpack_metadata["langpack_id"]
        build_variables = _get_build_variables(
            application_ini_data,
            arch,
            version,
            build_number,
            depends=application_ini_data["remoting_name"],
            # Debian package names are only lowercase
            package_name_suffix=f"-l10n-{langpack_id.lower()}",
            description_suffix=f" - {langpack_metadata['description']}",
        )
        _copy_plain_deb_config(template_dir, source_dir)
        _render_deb_templates(template_dir, source_dir, build_variables)

        os.makedirs(langpack_dir, exist_ok=True)
        shutil.copy(
            input_xpi_file,
            mozpath.join(
                langpack_dir,
                f"{langpack_metadata['browser_specific_settings']['gecko']['id']}.xpi",
            ),
        )
        _generate_deb_archive(
            source_dir=source_dir,
            target_dir=tmpdir,
            output_file_path=output,
            build_variables=build_variables,
            arch=arch,
        )
    finally:
        shutil.rmtree(tmpdir)


def _extract_application_ini_data(input_tar_file):
    with tempfile.TemporaryDirectory() as d:
        with tarfile.open(input_tar_file) as tar:
            application_ini_files = [
                tar_info
                for tar_info in tar.getmembers()
                if tar_info.name.endswith("/application.ini")
            ]
            if len(application_ini_files) == 0:
                raise ValueError(
                    f"Cannot find any application.ini file in archive {input_tar_file}"
                )
            if len(application_ini_files) > 1:
                raise ValueError(
                    f"Too many application.ini files found in archive {input_tar_file}. "
                    f"Found: {application_ini_files}"
                )

            tar.extract(application_ini_files[0], path=d)

        return _extract_application_ini_data_from_directory(d)


def _extract_application_ini_data_from_directory(application_directory):
    values = get_application_ini_values(
        application_directory,
        dict(section="App", value="Name"),
        dict(section="App", value="CodeName", fallback="Name"),
        dict(section="App", value="Vendor"),
        dict(section="App", value="RemotingName"),
        dict(section="App", value="BuildID"),
    )

    data = {
        "name": next(values),
        "display_name": next(values),
        "vendor": next(values),
        "remoting_name": next(values),
        "build_id": next(values),
    }
    data["timestamp"] = datetime.datetime.strptime(data["build_id"], "%Y%m%d%H%M%S")

    return data


def _get_build_variables(
    application_ini_data,
    arch,
    version_string,
    build_number,
    depends,
    package_name_suffix="",
    description_suffix="",
):
    version = GeckoVersion.parse(version_string)
    # Nightlies don't have build numbers
    deb_pkg_version = (
        f"{version}~{application_ini_data['build_id']}"
        if version.is_nightly
        else f"{version}~build{build_number}"
    )
    remoting_name = application_ini_data["remoting_name"].lower()

    return {
        "DEB_DESCRIPTION": f"{application_ini_data['vendor']} {application_ini_data['display_name']}"
        f"{description_suffix}",
        "DEB_PKG_INSTALL_PATH": f"usr/lib/{remoting_name}",
        "DEB_PKG_NAME": f"{remoting_name}{package_name_suffix}",
        "DEB_PKG_VERSION": deb_pkg_version,
        "DEB_CHANGELOG_DATE": format_datetime(application_ini_data["timestamp"]),
        "DEB_ARCH_NAME": _DEB_ARCH[arch],
        "DEB_DEPENDS": depends,
    }


def _copy_plain_deb_config(input_template_dir, source_dir):
    template_dir_filenames = os.listdir(input_template_dir)
    plain_filenames = [
        mozpath.basename(filename)
        for filename in template_dir_filenames
        if not filename.endswith(".in")
    ]
    os.makedirs(mozpath.join(source_dir, "debian"), exist_ok=True)

    for filename in plain_filenames:
        shutil.copy(
            mozpath.join(input_template_dir, filename),
            mozpath.join(source_dir, "debian", filename),
        )


def _render_deb_templates(
    input_template_dir, source_dir, build_variables, exclude_file_names=None
):
    exclude_file_names = [] if exclude_file_names is None else exclude_file_names

    template_dir_filenames = os.listdir(input_template_dir)
    template_filenames = [
        mozpath.basename(filename)
        for filename in template_dir_filenames
        if filename.endswith(".in") and filename not in exclude_file_names
    ]
    os.makedirs(mozpath.join(source_dir, "debian"), exist_ok=True)

    for file_name in template_filenames:
        with open(mozpath.join(input_template_dir, file_name)) as f:
            template = Template(f.read())
        with open(mozpath.join(source_dir, "debian", Path(file_name).stem), "w") as f:
            f.write(template.substitute(build_variables))


def _inject_deb_distribution_folder(source_dir, app_name):
    with tempfile.TemporaryDirectory() as git_clone_dir:
        subprocess.check_call(
            [
                "git",
                "clone",
                "https://github.com/mozilla-partners/deb.git",
                git_clone_dir,
            ],
        )
        shutil.copytree(
            mozpath.join(git_clone_dir, "desktop/deb/distribution"),
            mozpath.join(source_dir, app_name.lower(), "distribution"),
        )


def _inject_deb_desktop_entry_file(
    log,
    source_dir,
    build_variables,
    release_product,
    release_type,
    fluent_localization,
    fluent_resource_loader,
):
    desktop_entry_file_text = _generate_browser_desktop_entry_file_text(
        log,
        build_variables,
        release_product,
        release_type,
        fluent_localization,
        fluent_resource_loader,
    )
    desktop_entry_file_filename = f"{build_variables['DEB_PKG_NAME']}.desktop"
    os.makedirs(mozpath.join(source_dir, "debian"), exist_ok=True)
    with open(
        mozpath.join(source_dir, "debian", desktop_entry_file_filename), "w"
    ) as f:
        f.write(desktop_entry_file_text)


def _generate_browser_desktop_entry_file_text(
    log,
    build_variables,
    release_product,
    release_type,
    fluent_localization,
    fluent_resource_loader,
):
    localizations = _create_fluent_localizations(
        fluent_resource_loader, fluent_localization, release_type, release_product, log
    )
    desktop_entry = _generate_browser_desktop_entry(build_variables, localizations)
    desktop_entry_file_text = "\n".join(desktop_entry)
    return desktop_entry_file_text


def _create_fluent_localizations(
    fluent_resource_loader, fluent_localization, release_type, release_product, log
):
    brand_fluent_filename = "brand.ftl"
    l10n_central_url = "https://raw.githubusercontent.com/mozilla-l10n/firefox-l10n"
    desktop_entry_fluent_filename = "linuxDesktopEntry.ftl"

    l10n_dir = tempfile.mkdtemp()

    loader = fluent_resource_loader(os.path.join(l10n_dir, "{locale}"))

    localizations = {}
    linux_l10n_changesets = _load_linux_l10n_changesets(
        "browser/locales/l10n-changesets.json"
    )
    locales = ["en-US"]
    locales.extend(linux_l10n_changesets.keys())
    en_US_brand_fluent_filename = _get_en_US_brand_fluent_filename(
        brand_fluent_filename, release_type, release_product
    )

    for locale in locales:
        locale_dir = os.path.join(l10n_dir, locale)
        os.mkdir(locale_dir)
        localized_desktop_entry_filename = os.path.join(
            locale_dir, desktop_entry_fluent_filename
        )
        if locale == "en-US":
            en_US_desktop_entry_fluent_filename = os.path.join(
                "browser/locales/en-US/browser", desktop_entry_fluent_filename
            )
            shutil.copyfile(
                en_US_desktop_entry_fluent_filename,
                localized_desktop_entry_filename,
            )
        else:
            non_en_US_fluent_resource_file_url = f"{l10n_central_url}/{linux_l10n_changesets[locale]['revision']}/{locale}/browser/browser/{desktop_entry_fluent_filename}"
            response = requests.get(non_en_US_fluent_resource_file_url)
            response = retry(
                requests.get,
                args=[non_en_US_fluent_resource_file_url],
                attempts=5,
                sleeptime=3,
                jitter=2,
            )
            mgs = "Missing {fluent_resource_file_name} for {locale}: received HTTP {status_code} for GET {resource_file_url}"
            params = {
                "fluent_resource_file_name": desktop_entry_fluent_filename,
                "locale": locale,
                "resource_file_url": non_en_US_fluent_resource_file_url,
                "status_code": response.status_code,
            }
            action = "repackage-deb"
            if response.status_code == 404:
                log(
                    logging.WARNING,
                    action,
                    params,
                    mgs,
                )
                continue
            if response.status_code != 200:
                log(
                    logging.ERROR,
                    action,
                    params,
                    mgs,
                )
                raise HgServerError(mgs.format(**params))

            with open(localized_desktop_entry_filename, "w", encoding="utf-8") as f:
                f.write(response.text)

        shutil.copyfile(
            en_US_brand_fluent_filename,
            os.path.join(locale_dir, brand_fluent_filename),
        )

        fallbacks = [locale]
        if locale != "en-US":
            fallbacks.append("en-US")
        localizations[locale] = fluent_localization(
            fallbacks, [desktop_entry_fluent_filename, brand_fluent_filename], loader
        )

    return localizations


def _get_en_US_brand_fluent_filename(
    brand_fluent_filename, release_type, release_product
):
    branding_fluent_filename_template = os.path.join(
        "browser/branding/{brand}/locales/en-US", brand_fluent_filename
    )
    if release_type == "nightly":
        return branding_fluent_filename_template.format(brand="nightly")
    elif release_type == "beta" and release_product == "firefox":
        return branding_fluent_filename_template.format(brand="official")
    elif release_type == "beta" and release_product == "devedition":
        return branding_fluent_filename_template.format(brand="aurora")
    else:
        return branding_fluent_filename_template.format(brand="unofficial")


def _load_linux_l10n_changesets(l10n_changesets_filename):
    with open(l10n_changesets_filename) as l10n_changesets_file:
        l10n_changesets = json.load(l10n_changesets_file)
        return {
            locale: changeset
            for locale, changeset in l10n_changesets.items()
            if any(platform.startswith("linux") for platform in changeset["platforms"])
        }


def _generate_browser_desktop_entry(build_variables, localizations):
    mime_types = [
        "application/json",
        "application/pdf",
        "application/rdf+xml",
        "application/rss+xml",
        "application/x-xpinstall",
        "application/xhtml+xml",
        "application/xml",
        "audio/flac",
        "audio/ogg",
        "audio/webm",
        "image/avif",
        "image/gif",
        "image/jpeg",
        "image/png",
        "image/svg+xml",
        "image/webp",
        "text/html",
        "text/xml",
        "video/ogg",
        "video/webm",
        "x-scheme-handler/chrome",
        "x-scheme-handler/http",
        "x-scheme-handler/https",
    ]

    categories = [
        "GNOME",
        "GTK",
        "Network",
        "WebBrowser",
    ]

    actions = [
        {
            "name": "new-window",
            "message": "desktop-action-new-window-name",
            "command": f"{build_variables['DEB_PKG_NAME']} --new-window %u",
        },
        {
            "name": "new-private-window",
            "message": "desktop-action-new-private-window-name",
            "command": f"{build_variables['DEB_PKG_NAME']} --private-window %u",
        },
        {
            "name": "open-profile-manager",
            "message": "desktop-action-open-profile-manager",
            "command": f"{build_variables['DEB_PKG_NAME']} --ProfileManager",
        },
    ]

    desktop_entry = _desktop_entry_section(
        "Desktop Entry",
        [
            {
                "key": "Version",
                "value": "1.0",
            },
            {
                "key": "Type",
                "value": "Application",
            },
            {
                "key": "Exec",
                "value": f"{build_variables['DEB_PKG_NAME']} %u",
            },
            {
                "key": "Terminal",
                "value": "false",
            },
            {
                "key": "X-MultipleArgs",
                "value": "false",
            },
            {
                "key": "Icon",
                "value": build_variables["DEB_PKG_NAME"],
            },
            {
                "key": "StartupWMClass",
                "value": build_variables["DEB_PKG_NAME"],
            },
            {
                "key": "Categories",
                "value": _desktop_entry_list(categories),
            },
            {
                "key": "MimeType",
                "value": _desktop_entry_list(mime_types),
            },
            {
                "key": "StartupNotify",
                "value": "true",
            },
            {
                "key": "Actions",
                "value": _desktop_entry_list([action["name"] for action in actions]),
            },
            {"key": "Name", "value": "desktop-entry-name", "l10n": True},
            {"key": "Comment", "value": "desktop-entry-comment", "l10n": True},
            {"key": "GenericName", "value": "desktop-entry-generic-name", "l10n": True},
            {"key": "Keywords", "value": "desktop-entry-keywords", "l10n": True},
            {
                "key": "X-GNOME-FullName",
                "value": "desktop-entry-x-gnome-full-name",
                "l10n": True,
            },
        ],
        localizations,
    )

    for action in actions:
        desktop_entry.extend(
            _desktop_entry_section(
                f"Desktop Action {action['name']}",
                [
                    {
                        "key": "Name",
                        "value": action["message"],
                        "l10n": True,
                    },
                    {
                        "key": "Exec",
                        "value": action["command"],
                    },
                ],
                localizations,
            )
        )

    return desktop_entry


def _desktop_entry_list(iterable):
    delimiter = ";"
    return f"{delimiter.join(iterable)}{delimiter}"


def _desktop_entry_attribute(key, value, locale=None, localizations=None):
    if not locale and not localizations:
        return f"{key}={value}"
    if locale and locale == "en-US":
        return f"{key}={localizations[locale].format_value(value)}"
    else:
        return f"{key}[{locale.replace('-', '_')}]={localizations[locale].format_value(value)}"


def _desktop_entry_section(header, attributes, localizations):
    desktop_entry_section = [f"[{header}]"]
    l10n_attributes = [attribute for attribute in attributes if attribute.get("l10n")]
    non_l10n_attributes = [
        attribute for attribute in attributes if not attribute.get("l10n")
    ]
    for attribute in non_l10n_attributes:
        desktop_entry_section.append(
            _desktop_entry_attribute(attribute["key"], attribute["value"])
        )
    for attribute in l10n_attributes:
        for locale in localizations:
            desktop_entry_section.append(
                _desktop_entry_attribute(
                    attribute["key"], attribute["value"], locale, localizations
                )
            )
    desktop_entry_section.append("")
    return desktop_entry_section


def _generate_deb_archive(
    source_dir, target_dir, output_file_path, build_variables, arch
):
    command = _get_command(arch)
    subprocess.check_call(command, cwd=source_dir)
    deb_arch = _DEB_ARCH[arch]
    deb_file_name = f"{build_variables['DEB_PKG_NAME']}_{build_variables['DEB_PKG_VERSION']}_{deb_arch}.deb"
    deb_file_path = mozpath.join(target_dir, deb_file_name)

    if not os.path.exists(deb_file_path):
        raise NoDebPackageFound(deb_file_path)

    subprocess.check_call(["dpkg-deb", "--info", deb_file_path])
    shutil.move(deb_file_path, output_file_path)


def _get_command(arch):
    deb_arch = _DEB_ARCH[arch]
    command = [
        "dpkg-buildpackage",
        # TODO: Use long options once we stop supporting Debian Jesse. They're more
        # explicit.
        #
        # Long options were added in dpkg 1.18.8 which is part of Debian Stretch.
        #
        # https://git.dpkg.org/cgit/dpkg/dpkg.git/commit/?h=1.18.x&id=293bd243a19149165fc4fd8830b16a51d471a5e9
        # https://packages.debian.org/stretch/dpkg-dev
        "-us",  # --unsigned-source
        "-uc",  # --unsigned-changes
        "-b",  # --build=binary
    ]

    if deb_arch != "all":
        command.append(f"--host-arch={deb_arch}")

    if _is_chroot_available(arch):
        flattened_command = " ".join(command)
        command = [
            "chroot",
            _get_chroot_path(arch),
            "bash",
            "-c",
            f"cd /tmp/*/source; {flattened_command}",
        ]

    return command


def _create_temporary_directory(arch):
    if _is_chroot_available(arch):
        return tempfile.mkdtemp(dir=f"{_get_chroot_path(arch)}/tmp")
    else:
        return tempfile.mkdtemp()


def _is_chroot_available(arch):
    return os.path.isdir(_get_chroot_path(arch))


def _get_chroot_path(arch):
    deb_arch = "amd64" if arch == "all" else _DEB_ARCH[arch]
    return f"/srv/{_DEB_DIST}-{deb_arch}"


_MANIFEST_FILE_NAME = "manifest.json"


def _extract_langpack_metadata(input_xpi_file):
    with tempfile.TemporaryDirectory() as d:
        with zipfile.ZipFile(input_xpi_file) as zip:
            zip.extract(_MANIFEST_FILE_NAME, path=d)

        with open(mozpath.join(d, _MANIFEST_FILE_NAME)) as f:
            return json.load(f)
