# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os.path

import mozharness

external_tools_path = os.path.join(
    os.path.abspath(os.path.dirname(os.path.dirname(mozharness.__file__))),
    "external_tools",
)

config = {
    "simple_name_move": True,
    "vcs_share_base": "/builds/hg-shared",
    "exes": {
        "gittool.py": [os.path.join(external_tools_path, "gittool.py")],
    },
    "upload_env": {
        "UPLOAD_PATH": "/builds/worker/artifacts/",
    },
}
