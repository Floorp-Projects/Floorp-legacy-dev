# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Defines artifacts to sign before repackage.
"""

from taskgraph.util.taskcluster import get_artifact_path

from gecko_taskgraph.util.declarative_artifacts import get_geckoview_upstream_artifacts

LANGPACK_SIGN_PLATFORMS = {  # set
    "linux64-shippable",
    "linux64-devedition",
    "macosx64-shippable",
    "macosx64-devedition",
}


def is_partner_kind(kind):
    if kind and kind.startswith(("release-partner", "release-eme-free")):
        return True


def is_notarization_kind(kind):
    if kind and "notarization" in kind:
        return True


def is_mac_signing_king(kind):
    return kind and "mac-signing" in kind


def generate_specifications_of_artifacts_to_sign(
    config, job, keep_locale_template=True, kind=None, dep_kind=None
):
    build_platform = job["attributes"].get("build_platform")
    use_stub = job["attributes"].get("stub-installer")
    # Get locales to know if we want to sign ja-JP-mac langpack
    locales = job["attributes"].get("chunk_locales", [])
    if kind == "release-source-signing":
        artifacts_specifications = [
            {
                "artifacts": [get_artifact_path(job, "source.tar.xz")],
                "formats": ["autograph_gpg"],
            }
        ]
    elif "android" in build_platform:
        artifacts_specifications = [
            {
                "artifacts": get_geckoview_artifacts_to_sign(config, job),
                "formats": ["autograph_gpg"],
            }
        ]
    # XXX: Mars aren't signed here (on any platform) because internals will be
    # signed at after this stage of the release
    elif "macosx" in build_platform:
        langpack_formats = []
        if is_notarization_kind(config.kind):
            formats = ["apple_notarization"]
            artifacts_specifications = [
                {
                    "artifacts": [
                        get_artifact_path(job, "{locale}/target.tar.gz"),
                        get_artifact_path(job, "{locale}/target.pkg"),
                    ],
                    "formats": formats,
                }
            ]
        else:
            # This task is mac-signing
            if is_partner_kind(kind):
                extension = "tar.gz"
            else:
                extension = "dmg"
            artifacts_specifications = [
                {
                    "artifacts": [
                        get_artifact_path(job, f"{{locale}}/target.{extension}")
                    ],
                    "formats": ["macapp", "autograph_widevine", "autograph_omnija"],
                }
            ]
            langpack_formats = ["autograph_langpack"]

        if "ja-JP-mac" in locales and build_platform in LANGPACK_SIGN_PLATFORMS:
            artifacts_specifications += [
                {
                    "artifacts": [
                        get_artifact_path(job, "ja-JP-mac/target.langpack.xpi")
                    ],
                    "formats": langpack_formats,
                }
            ]
    elif "win" in build_platform:
        artifacts_specifications = [
            {
                "artifacts": [
                    get_artifact_path(job, "{locale}/setup.exe"),
                ],
                "formats": ["autograph_authenticode_202404"],
            },
            {
                "artifacts": [
                    get_artifact_path(job, "{locale}/target.zip"),
                ],
                "formats": [
                    "autograph_authenticode_202404",
                    "autograph_widevine",
                    "autograph_omnija",
                ],
            },
        ]

        if use_stub:
            artifacts_specifications[0]["artifacts"] += [
                get_artifact_path(job, "{locale}/setup-stub.exe")
            ]
    elif "linux" in build_platform:
        artifacts_specifications = [
            {
                "artifacts": [get_artifact_path(job, "{locale}/target.tar.bz2")],
                "formats": ["autograph_gpg", "autograph_widevine", "autograph_omnija"],
            }
        ]
        if build_platform in LANGPACK_SIGN_PLATFORMS:
            artifacts_specifications += [
                {
                    "artifacts": [
                        get_artifact_path(job, "{locale}/target.langpack.xpi")
                    ],
                    "formats": ["autograph_langpack"],
                }
            ]
    else:
        raise Exception("Platform not implemented for signing")

    if not keep_locale_template:
        artifacts_specifications = _strip_locale_template(artifacts_specifications)

    if is_partner_kind(kind):
        artifacts_specifications = _strip_widevine_for_partners(
            artifacts_specifications
        )

    return artifacts_specifications


def _strip_locale_template(artifacts_without_locales):
    for spec in artifacts_without_locales:
        for index, artifact in enumerate(spec["artifacts"]):
            stripped_artifact = artifact.format(locale="")
            stripped_artifact = stripped_artifact.replace("//", "/")
            spec["artifacts"][index] = stripped_artifact

    return artifacts_without_locales


def _strip_widevine_for_partners(artifacts_specifications):
    """Partner repacks should not resign that's previously signed for fear of breaking partial
    updates
    """
    for spec in artifacts_specifications:
        if "autograph_widevine" in spec["formats"]:
            spec["formats"].remove("autograph_widevine")
        if "autograph_omnija" in spec["formats"]:
            spec["formats"].remove("autograph_omnija")

    return artifacts_specifications


def get_signed_artifacts(input, formats, behavior=None):
    """
    Get the list of signed artifacts for the given input and formats.
    """
    artifacts = set()
    if input.endswith(".dmg"):
        artifacts.add(input.replace(".dmg", ".tar.gz"))
        if behavior and behavior != "mac_sign":
            artifacts.add(input.replace(".dmg", ".pkg"))
    else:
        artifacts.add(input)
    if "autograph_gpg" in formats:
        artifacts.add(f"{input}.asc")

    return artifacts


def get_geckoview_artifacts_to_sign(config, job):
    upstream_artifacts = []
    for package in job["attributes"]["maven_packages"]:
        upstream_artifacts += get_geckoview_upstream_artifacts(config, job, package)
    return [
        path
        for upstream_artifact in upstream_artifacts
        for path in upstream_artifact["paths"]
        if not path.endswith(".md5") and not path.endswith(".sha1")
    ]
