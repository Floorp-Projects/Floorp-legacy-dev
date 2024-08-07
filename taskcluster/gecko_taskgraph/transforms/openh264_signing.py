# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Transform the repackage signing task into an actual task description.
"""

from taskgraph.transforms.base import TransformSequence
from taskgraph.util.treeherder import inherit_treeherder_from_dep
from voluptuous import Optional

from gecko_taskgraph.loader.single_dep import schema
from gecko_taskgraph.transforms.task import task_description_schema
from gecko_taskgraph.util.attributes import copy_attributes_from_dependent_job
from gecko_taskgraph.util.scriptworker import get_signing_cert_scope_per_platform

transforms = TransformSequence()

signing_description_schema = schema.extend(
    {
        Optional("label"): str,
        Optional("extra"): object,
        Optional("shipping-product"): task_description_schema["shipping-product"],
        Optional("shipping-phase"): task_description_schema["shipping-phase"],
    }
)

transforms.add_validate(signing_description_schema)


@transforms.add
def make_signing_description(config, jobs):
    for job in jobs:
        dep_job = job["primary-dependency"]
        attributes = dep_job.attributes
        build_platform = dep_job.attributes.get("build_platform")
        is_nightly = True  # cert_scope_per_platform uses this to choose the right cert

        description = (
            "Signing of OpenH264 Binaries for '"
            "{build_platform}/{build_type}'".format(
                build_platform=attributes.get("build_platform"),
                build_type=attributes.get("build_type"),
            )
        )

        # we have a genuine repackage job as our parent
        dependencies = {"openh264": dep_job.label}

        my_attributes = copy_attributes_from_dependent_job(dep_job)

        signing_cert_scope = get_signing_cert_scope_per_platform(
            build_platform, is_nightly, config
        )

        scopes = [signing_cert_scope]
        worker_type = "linux-signing"
        worker = {
            "implementation": "scriptworker-signing",
            "max-run-time": 3600,
        }
        rev = attributes["openh264_rev"]
        upstream_artifact = {
            "taskId": {"task-reference": "<openh264>"},
            "taskType": "build",
        }

        if "win" in build_platform:
            # job['primary-dependency'].task['payload']['command']
            upstream_artifact["formats"] = ["autograph_authenticode_202404"]
        elif "mac" in build_platform:
            upstream_artifact["formats"] = ["mac_single_file"]
            upstream_artifact["singleFileGlobs"] = ["libgmpopenh264.dylib"]
            worker_type = "mac-signing"
            worker["mac-behavior"] = "mac_notarize_single_file"
        else:
            upstream_artifact["formats"] = ["autograph_gpg"]

        upstream_artifact["paths"] = [
            f"private/openh264/openh264-{build_platform}-{rev}.zip",
        ]
        worker["upstream-artifacts"] = [upstream_artifact]

        treeherder = inherit_treeherder_from_dep(job, dep_job)
        treeherder.setdefault(
            "symbol",
            _generate_treeherder_symbol(
                dep_job.task.get("extra", {}).get("treeherder", {}).get("symbol")
            ),
        )

        task = {
            "label": job["label"],
            "description": description,
            "worker-type": worker_type,
            "worker": worker,
            "scopes": scopes,
            "dependencies": dependencies,
            "attributes": my_attributes,
            "run-on-projects": dep_job.attributes.get("run_on_projects"),
            "treeherder": treeherder,
        }

        yield task


def _generate_treeherder_symbol(build_symbol):
    symbol = build_symbol + "s"
    return symbol
