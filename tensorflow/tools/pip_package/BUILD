# Description:
#  Tools for building the TensorFlow pip package.

package(default_visibility = ["//visibility:private"])

load(
    "//tensorflow:tensorflow.bzl",
    "if_not_windows",
    "transitive_hdrs",
)
load("//third_party/mkl:build_defs.bzl", "if_mkl")
load("//tensorflow/core:platform/default/build_config_root.bzl", "tf_additional_license_deps")

# This returns a list of headers of all public header libraries (e.g.,
# framework, lib), and all of the transitive dependencies of those
# public headers.  Not all of the headers returned by the filegroup
# are public (e.g., internal headers that are included by public
# headers), but the internal headers need to be packaged in the
# pip_package for the public headers to be properly included.
#
# Public headers are therefore defined by those that are both:
#
# 1) "publicly visible" as defined by bazel
# 2) Have documentation.
#
# This matches the policy of "public" for our python API.
transitive_hdrs(
    name = "included_headers",
    deps = [
        "//tensorflow/core:core_cpu",
        "//tensorflow/core:framework",
        "//tensorflow/core:lib",
        "//tensorflow/core:protos_all_cc",
        "//tensorflow/core:stream_executor",
        "//third_party/eigen3",
    ],
)

py_binary(
    name = "simple_console",
    srcs = ["simple_console.py"],
    srcs_version = "PY2AND3",
    deps = ["//tensorflow:tensorflow_py"],
)

filegroup(
    name = "licenses",
    data = [
        "//third_party/eigen3:LICENSE",
        "@boringssl//:LICENSE",
        "@com_googlesource_code_re2//:LICENSE",
        "@curl//:COPYING",
        "@eigen_archive//:COPYING.MPL2",
        "@farmhash_archive//:COPYING",
        "@gemmlowp//:LICENSE",
        "@gif_archive//:COPYING",
        "@grpc//:LICENSE",
        "@highwayhash//:LICENSE",
        "@jemalloc//:COPYING",
        "@jpeg//:LICENSE.md",
        "@libxsmm_archive//:LICENSE",
        "@local_config_sycl//sycl:LICENSE.text",
        "@nanopb_git//:LICENSE.txt",
        "@png_archive//:LICENSE",
        "@protobuf//:LICENSE",
        "@six_archive//:LICENSE",
        "@org_pocoo_werkzeug//:LICENSE",
        "@zlib_archive//:zlib.h",
    ] + if_not_windows([
        "@nccl_archive//:LICENSE.txt",
    ]) + tf_additional_license_deps(),
)

sh_binary(
    name = "build_pip_package",
    srcs = ["build_pip_package.sh"],
    data = select({
        "//tensorflow:windows": [":simple_console_for_windows"],
        "//conditions:default": [
            "licenses",
            "MANIFEST.in",
            "setup.py",
            ":included_headers",
            ":simple_console",
            "//tensorflow:tensorflow_py",
            "//tensorflow/python:util_example_parser_configuration",
            "//tensorflow/python/debug:debug_pip",
            "//tensorflow/python/saved_model:saved_model",
            "//tensorflow/python/tools:tools_pip",
        ],
    }) + if_mkl(["//third_party/mkl:intel_binary_blob"]),
)
