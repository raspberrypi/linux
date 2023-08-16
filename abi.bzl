# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
ABI aware build rules.
"""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("@bazel_skylib//rules:copy_file.bzl", "copy_file")

visibility("private")

_ALL_ABIS = ["arm64", "x86_64"]

def _build_with_abi(name, build_rule, path_prefix, abis, visibility, **kwargs):
    build_rule(name = name, visibility = visibility, **kwargs)

    for abi in abis:
        copy_file(
            name = "{name}_{abi}".format(name = name, abi = abi),
            src = ":{name}".format(name = name),
            out = paths.join(path_prefix, abi, name),
            allow_symlink = True,
            visibility = visibility,
        )

def cc_binary_with_abi(name, path_prefix = None, abis = None, visibility = None, **kwargs):
    """A cc_binary replacement that generates output in each subdirectory named by abi.

    For example:
    ```
      cc_binary_with_abi(
        name = "a_binary",
        abis = ["x86_64", "arm64"],
        path_prefix = "my/path",
      )
    ```
    generates 2 rules:
    * Rule a_binary_x86_64: Builds the cc_binary and put output in my/path/x86_64/a_binary.
    * Rule a_binary_arm64: Builds the cc_binary and put output in my/path/arm64/a_binary.

    Args:
        name: the name of the build rule.
        path_prefix: [Nonconfigurable](https://bazel.build/reference/be/common-definitions#configurable-attributes).
          The path prefix to attach to output.
        abis: [Nonconfigurable](https://bazel.build/reference/be/common-definitions#configurable-attributes).
          The intended abis to generate.
        visibility: [Nonconfigurable](https://bazel.build/reference/be/common-definitions#configurable-attributes).
          The visibility attribute on a target controls whether the target can be used in other packages.
        **kwargs: the rest args that cc_binary uses.
    """
    if not path_prefix:
        path_prefix = ""
    if not abis:
        abis = _ALL_ABIS

    _build_with_abi(
        name = name,
        build_rule = native.cc_binary,
        path_prefix = path_prefix,
        abis = abis,
        visibility = visibility,
        **kwargs
    )
