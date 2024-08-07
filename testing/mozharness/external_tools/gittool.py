#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
### Compressed module sources ###
module_sources = [
    (
        "util",
        "eJxlkMEKgzAQRO/5isWTQhFaSg8Ff6LnQknM2ixoItmov1+T2FLb3DY7mZkXGkbnAxjJpiclKI+K\nrOSWSAihsQM28sjBk32WXF0FrKe4YZi8hWAwrZMDuC5fJC1wkaQ+K7eIOqpXm1rTEzmU1ZahLuc/\ncwYlGS9nQNs6jfoACwUDQVIf/RdDAXmULYK0Gpo1aXAz6l3sG6VWJ/nIdjHdx45jWTR3W3xVSKTT\n8NuEE9a+DMzomZz9QOencdyDJ7LvH6zEC9SEeBQ=\n",
    ),
    (
        "util.file",
        "eJzNVk2P2zYQvftXTF0sLC9ctTbaSwAfim2BFCjSIsktCLy0SFnMSqRAUuv1v+8MP0RZ3uTQU3SwJXLmcebNm5GWy+Vb0fbCQD2oykmtLDgNDVO8FVBL/NG4y/zOcrlcyK7XxkGrTyepTulR23Rnm8HJNj01zDatPKZHJ7qeMBe10R1UWtXy1DNj8fho8Z6dH/zyv355gefAPp1WnoT7G2+FKQ4HxTpxOKwXiwUXNWJ1CCQKQt9ufOC79ZsF4IVRx11MrRF0rBPKWdA1uLP2tnYDPbNWcJAKhEQzA8x6d7yQARW4CLygK25XlbBWHhNNPXONLeEPjdsMLOaDeUYAxeSzAOsMJhFDlVarDVgNXIPSDgaLTCtomTlRMSgmopu8ZQ3SSmUdU9WY4ZFZEQBjmnT5PeSLAk6GK7PawEcziPVXwHbfANtNwXY3YP6Iw0joPiyURjBeZIvd3GI3tTDCDUbdQO1nrrHOXBpROW0uqBLHMI9iXEE2h7qWL7ns8D5gOwyYEqfqn7ASKsN4NRCO3+yNfpYcVXCWLa+Y4R4pwG6ophLrQ/3x2Fqotf75/sjMI6RCNdJhhkxdik91KRS3Z1RSEaNCe6wr6UvbspXWYQw5+PXnsTykBkTKhUD1l8IYbYrlgx5a7i1qqTjc31kCvLNLuIMixZlBrxhGzLFZ+guxW1hTobl1G7/UaS72vrgjgw+4DGhFOXu7HrtImGfSMY6FTmIH0MjAtgAnO5S+rEcsFJrXSqInjIby5vQx8+SYU5+40MbMZWaBmnYTi5CrbdjWDl1R56SSKrDgH97+/tPWTyk/DWbTwff1Y/24IZIb8UIl7Jgby43FjgOupGOS5PuxaahhjquwfG4IjfjI6R1bXT1RU/ShI37b7uAetr/sfs0JRkV40+zpvdHnaVxpyqHnzInCW15XvsTguTxhjxcjL6wWh0G1Uj355qZxmikKG5A2QJ6UNoqq7oVoUzd5hjiOPLVyIF5Q1YkcZy5vpjmg6GlCltKS7mcnjiMHW8Ro7byGbRjjNrbMmbU50g0Ksuf6rKZ6nSP5ZvMQt/u5UDGuL1qqIpxdr181v2Ksv7VBJNNRcoSSt0VrxfX5EzY8ZfZrdETUeZW8jXipRO/gnw9/UkHwZQWTU36Ev6hgApbvNI6vqolvcpNnw3JaHEHzRWmaurvrEIKCvpGMYdKKqaZm7Y2ay7Iig/bi+3ycKmiALz+SFvPfB9owHMsSu9B0gktUdJguDY5tQwTQZwfhReckuJqjKLrey3U/fmmU3ZOle5q0+8Q63pNZ4YNbvzab9PFL4VvYJ+GbeEOlqHlobDxrdcbFG+c8g2IswYK04YtXxOXITCSu1YwfwufQXAthFTOafRkVk6lJq2GAfEren9fwwx7yYy5anAnvtBLTGRFQYjz4nZXCCX/48hP+6xBZ6MM/2rGhTcXNceBEPETjIv6vfQUn+wGjmGGub4KMLhjOje009hjKTfAHVNH3kgCF8r9yOGrdfi9JUCyC3bq8lsh/nt7m3w==",
    ),
    (
        "util.commands",
        "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",
    ),
    (
        "util.retry",
        "eJytVk2P2zYQvetXDFwsLDuC4C2wORhxsUHQFgWKnHqXaYmyiUqkQ1LxGkX/e2dIivpy0h6qw1oa\nDh9nHt/MjmivSluwouVJrVULdSdLq1RjQPilm2ZX49dKJS1/s4049YvB0jLJzlwnwdqo81nIc4K/\ncOi/8jO3v+Mr12lRSNbyotgkSVLxGjS3+p6y0golM2DW8vZqzeElA9NwfqXgDu93GbTsrRgsL7AF\ntCYQH4dT8LeSPJQ0h/Tn/j3bZFA2nMnuevisJMdj9Bkd0Pznzb3+9fdm77BWq9Un1jRw9AGtgdHB\nou1aUDVaQ3hrR5qBTlrRgLBgurLkvDJDRJgb6xqLyYNV8JLDMUa/BmHAXjjIrj1xTciGI5uVIdcb\nEzainLi9cS4jL9kM9/0OmKygUt2pIRNn5cVT0W/J0C3CTbOZULrOAY5zEl2kDGx3bThuiTiRWsqD\nYfoX1TUVRgsl684Xm8NvNQwwoDBbTa4S/yjDI1AjjOUVCPnobKY5aCYMOjgJ9peSEXl3uAm8qNOA\nFVxF2/JKMMubuwvjGK7e5XLV6quo0ItYK/Gm2QkzwwsksBHrbm0KBqy2mASmELMnxD7hz4pU1bVc\nWhOBQohwZYZCwwsTnpu76nSvSV92BKf5l05o1NUSCUPEwzTKBCOSlIEjHnFckbp1ScH1WxtuTETO\nI86R9L526R+9+D3P/SU7NYnSkkBiFBQ4pQBY8YOY0HjsKVxj4bgFSpR6Q7CHwt6M16SyMXWlB9dg\n876inlY8fBj6wX6QjzrnFT9153Q19X6qwBHgJDc2r+AJ0lHbgOkxo66z8YFI7GLP7u12EUiQhA+H\nWI5DJKjd/QSWQhOyVunKCXsP1FeoRJ8MysJeXA/a41ffhPz7agISn1U4EX4IKfQN01id0u6Nf/VQ\n+CFD+LE4uO00qsNtS7fklcF2G/yjqy+/RTNdphZYj7lREQwVv4dVRl8FMXD4Q3d8Gg3ebrjt/SLf\nsJAuduBNPGL+m4T/Kr4S36QyidwSbWM1Ttih1jE/b5DNT7D7D+f9wlAfVVCQu+kq9vUTrxV1M/LE\nJYzl8T3TMyhw4UPW3K2n3/EaAj+M3rfw48JzluWkFJYZz7En7hNvGg2E7AZjLSTKf1YiEt5RbQ1z\ngHB9YOvV10vUfwWheoD1eg0f8T9hqTSz2EKQ2zBHbHLszqylTtYZHEu8/+sA7tmiA2ulRhrL8zyZ\n+8Zh5Hm3G48jz7sB5cR0utlPYEKESfQpImRRowIVxkmNebTt1Q1a3jqeIMZbyeWKA9S8dveP6tyz\nQXhh2PGbwrjjfxBjxPS39Ti7gmR21DLE5PFqyB3v+3U2OsY5EEsjBP3vIlhwFlEKYb/D0v/M0CN2\n7oLjNNTHkvwDPQB6iA==\n",
    ),
    (
        "util.git",
        "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",
    ),
]

### Load the compressed module sources ###
import sys, imp, base64, zlib

for name, source in module_sources:
    source = zlib.decompress(base64.b64decode(source))
    mod = imp.new_module(name)
    exec(source, mod.__dict__)
    sys.modules[name] = mod

### Original script follows ###
#!/usr/bin/python
"""%prog [-p|--props-file] [-r|--rev revision] [-b|--branch branch]
         [-s|--shared-dir shared_dir] repo [dest]

Tool to do safe operations with git.

revision/branch on commandline will override those in props-file"""

# Import snippet to find tools lib
import os
import site
import logging

site.addsitedir(
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "../../lib/python")
)

try:
    import simplejson as json

    assert json
except ImportError:
    import json

from util.git import git


if __name__ == "__main__":
    from optparse import OptionParser

    parser = OptionParser(__doc__)
    parser.set_defaults(
        revision=os.environ.get("GIT_REV"),
        branch=os.environ.get("GIT_BRANCH", None),
        loglevel=logging.INFO,
        shared_dir=os.environ.get("GIT_SHARE_BASE_DIR"),
        mirrors=None,
        clean=False,
    )
    parser.add_option(
        "-r", "--rev", dest="revision", help="which revision to update to"
    )
    parser.add_option("-b", "--branch", dest="branch", help="which branch to update to")
    parser.add_option(
        "-p",
        "--props-file",
        dest="propsfile",
        help="build json file containing revision information",
    )
    parser.add_option(
        "-s", "--shared-dir", dest="shared_dir", help="clone to a shared directory"
    )
    parser.add_option(
        "--mirror",
        dest="mirrors",
        action="append",
        help="add a mirror to try cloning/pulling from before repo",
    )
    parser.add_option(
        "--clean",
        dest="clean",
        action="store_true",
        default=False,
        help="run 'git clean' after updating the local repository",
    )
    parser.add_option(
        "-v", "--verbose", dest="loglevel", action="store_const", const=logging.DEBUG
    )

    options, args = parser.parse_args()

    logging.basicConfig(level=options.loglevel, format="%(asctime)s %(message)s")

    if len(args) not in (1, 2):
        parser.error("Invalid number of arguments")

    repo = args[0]
    if len(args) == 2:
        dest = args[1]
    else:
        dest = os.path.basename(repo)

    # Parse propsfile
    if options.propsfile:
        js = json.load(open(options.propsfile))
        if options.revision is None:
            options.revision = js["sourcestamp"]["revision"]
        if options.branch is None:
            options.branch = js["sourcestamp"]["branch"]

    got_revision = git(
        repo,
        dest,
        options.branch,
        options.revision,
        shareBase=options.shared_dir,
        mirrors=options.mirrors,
        clean_dest=options.clean,
    )

    print("Got revision %s" % got_revision)
