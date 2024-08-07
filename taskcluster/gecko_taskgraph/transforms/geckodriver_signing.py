# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Transform the repackage signing task into an actual task description.
"""

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
        Optional("shipping-phase"): task_description_schema["shipping-phase"],
    }
)

transforms = TransformSequence()
transforms.add_validate(repackage_signing_description_schema)


@transforms.add
def make_signing_description(config, jobs):
    for job in jobs:
        dep_job = job["primary-dependency"]

        attributes = copy_attributes_from_dependent_job(dep_job)
        attributes["repackage_type"] = "repackage-signing"

        treeherder = job.get("treeherder", {})
        dep_treeherder = dep_job.task.get("extra", {}).get("treeherder", {})
        treeherder.setdefault(
            "symbol", "{}(gd-s)".format(dep_treeherder["groupSymbol"])
        )
        treeherder.setdefault(
            "platform", dep_job.task.get("extra", {}).get("treeherder-platform")
        )
        treeherder.setdefault("tier", dep_treeherder.get("tier", 1))
        treeherder.setdefault("kind", "build")

        dependencies = {dep_job.kind: dep_job.label}
        signing_dependencies = dep_job.dependencies
        dependencies.update(
            {k: v for k, v in signing_dependencies.items() if k != "docker-image"}
        )

        description = "Signing Geckodriver for build '{}'".format(
            attributes.get("build_platform"),
        )

        build_platform = dep_job.attributes.get("build_platform")
        is_shippable = dep_job.attributes.get("shippable")
        signing_cert_scope = get_signing_cert_scope_per_platform(
            build_platform, is_shippable, config
        )

        upstream_artifacts = _craft_upstream_artifacts(
            dep_job, dep_job.kind, build_platform
        )

        scopes = [signing_cert_scope]

        platform = build_platform.rsplit("-", 1)[0]

        task = {
            "label": job["label"],
            "description": description,
            "worker-type": "linux-signing",
            "worker": {
                "implementation": "scriptworker-signing",
                "upstream-artifacts": upstream_artifacts,
            },
            "scopes": scopes,
            "dependencies": dependencies,
            "attributes": attributes,
            "treeherder": treeherder,
            "run-on-projects": ["mozilla-central"],
            "index": {"product": "geckodriver", "job-name": platform},
        }

        if build_platform.startswith("macosx"):
            worker_type = task["worker-type"]
            worker_type_alias_map = {
                "linux-depsigning": "mac-depsigning",
                "linux-signing": "mac-signing",
            }

            assert worker_type in worker_type_alias_map, (
                "Make sure to adjust the below worker_type_alias logic for "
                "mac if you change the signing workerType aliases!"
                " ({} not found in mapping)".format(worker_type)
            )
            worker_type = worker_type_alias_map[worker_type]

            task["worker-type"] = worker_type_alias_map[task["worker-type"]]
            task["worker"]["mac-behavior"] = "mac_geckodriver"

        yield task


def _craft_upstream_artifacts(dep_job, dependency_kind, build_platform):
    if build_platform.startswith("win"):
        signing_format = "autograph_authenticode_202404"
    elif build_platform.startswith("linux"):
        signing_format = "autograph_gpg"
    elif build_platform.startswith("macosx"):
        signing_format = "mac_geckodriver"
    else:
        raise ValueError(f'Unsupported build platform "{build_platform}"')

    return [
        {
            "taskId": {"task-reference": f"<{dependency_kind}>"},
            "taskType": "build",
            "paths": [dep_job.attributes["toolchain-artifact"]],
            "formats": [signing_format],
        }
    ]
