# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Transform the repackage signing task into an actual task description.
"""

import os

from taskgraph.transforms.base import TransformSequence
from voluptuous import Optional

from gecko_taskgraph.loader.single_dep import schema
from gecko_taskgraph.transforms.task import task_description_schema
from gecko_taskgraph.util.attributes import copy_attributes_from_dependent_job
from gecko_taskgraph.util.scriptworker import get_signing_cert_scope_per_platform

repackage_signing_description_schema = schema.extend(
    {
        Optional("label"): str,
        Optional("treeherder"): task_description_schema["treeherder"],
        Optional("shipping-product"): task_description_schema["shipping-product"],
        Optional("shipping-phase"): task_description_schema["shipping-phase"],
    }
)

SIGNING_FORMATS = {
    "target.installer.exe": ["autograph_authenticode_202404_stub"],
    "target.stub-installer.exe": ["autograph_authenticode_202404_stub"],
    "target.installer.msi": ["autograph_authenticode_202404"],
    "target.installer.msix": ["autograph_authenticode_202404"],
}

transforms = TransformSequence()
transforms.add_validate(repackage_signing_description_schema)


@transforms.add
def make_repackage_signing_description(config, jobs):
    for job in jobs:
        dep_job = job["primary-dependency"]
        attributes = copy_attributes_from_dependent_job(dep_job)
        locale = attributes.get("locale", dep_job.attributes.get("locale"))
        attributes["repackage_type"] = "repackage-signing"

        treeherder = job.get("treeherder", {})
        treeherder.setdefault("symbol", "rs(B)")
        dep_th_platform = dep_job.task.get("extra", {}).get("treeherder-platform")
        treeherder.setdefault("platform", dep_th_platform)
        treeherder.setdefault(
            "tier", dep_job.task.get("extra", {}).get("treeherder", {}).get("tier", 1)
        )
        treeherder.setdefault("kind", "build")

        if locale:
            treeherder["symbol"] = f"rs({locale})"

        if config.kind == "repackage-signing-msi":
            treeherder["symbol"] = "MSIs({})".format(locale or "N")

        elif config.kind in (
            "repackage-signing-msix",
            "repackage-signing-shippable-l10n-msix",
        ):
            # Like "MSIXs(Bs-multi)".
            treeherder["symbol"] = "MSIXs({})".format(
                dep_job.task.get("extra", {}).get("treeherder", {}).get("symbol", "B")
            )

        label = job["label"]

        dep_kind = dep_job.kind
        if "l10n" in dep_kind:
            dep_kind = "repackage"

        dependencies = {dep_kind: dep_job.label}

        signing_dependencies = dep_job.dependencies
        # This is so we get the build task etc in our dependencies to have better beetmover
        # support.  But for multi-locale MSIX packages, we don't want the signing task to directly
        # depend on the langpack tasks.
        dependencies.update(
            {
                k: v
                for k, v in signing_dependencies.items()
                if k != "docker-image"
                and not k.startswith("shippable-l10n-signing-linux64")
            }
        )

        description = (
            "Signing of repackaged artifacts for locale '{locale}' for build '"
            "{build_platform}/{build_type}'".format(
                locale=attributes.get("locale", "en-US"),
                build_platform=attributes.get("build_platform"),
                build_type=attributes.get("build_type"),
            )
        )

        build_platform = dep_job.attributes.get("build_platform")
        is_shippable = dep_job.attributes.get("shippable")
        signing_cert_scope = get_signing_cert_scope_per_platform(
            build_platform, is_shippable, config
        )
        scopes = [signing_cert_scope]

        upstream_artifacts = []
        for artifact in sorted(dep_job.attributes.get("release_artifacts")):
            basename = os.path.basename(artifact)
            if basename in SIGNING_FORMATS:
                upstream_artifacts.append(
                    {
                        "taskId": {"task-reference": f"<{dep_kind}>"},
                        "taskType": "repackage",
                        "paths": [artifact],
                        "formats": SIGNING_FORMATS[os.path.basename(artifact)],
                    }
                )

        task = {
            "label": label,
            "description": description,
            "worker-type": "linux-signing" if is_shippable else "linux-depsigning",
            "worker": {
                "implementation": "scriptworker-signing",
                "upstream-artifacts": upstream_artifacts,
                "max-run-time": 3600,
            },
            "scopes": scopes,
            "dependencies": dependencies,
            "attributes": attributes,
            "run-on-projects": dep_job.attributes.get("run_on_projects"),
            "optimization": dep_job.optimization,
            "treeherder": treeherder,
        }

        yield task
