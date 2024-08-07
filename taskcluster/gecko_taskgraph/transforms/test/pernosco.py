# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from taskgraph.transforms.base import TransformSequence

transforms = TransformSequence()


@transforms.add
def add_pernosco_route(config, tasks):
    try_config = config.params.get("try_task_config", {})
    if not try_config.get("pernosco"):
        yield from tasks
        return

    for task in tasks:
        task.setdefault("routes", []).append("notify.pulse.pernosco-v1.on-resolved")
        yield task
