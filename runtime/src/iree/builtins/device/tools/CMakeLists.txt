################################################################################
# Autogenerated by build_tools/bazel_to_cmake/bazel_to_cmake.py from           #
# runtime/src/iree/builtins/device/tools/BUILD.bazel                           #
#                                                                              #
# Use iree_cmake_extra_content from iree/build_defs.oss.bzl to add arbitrary   #
# CMake-only content.                                                          #
#                                                                              #
# To disable autogeneration for this file entirely, delete this header.        #
################################################################################

iree_add_all_subdirs()

iree_cc_binary_benchmark(
  NAME
    libdevice_benchmark
  SRCS
    "libdevice_benchmark.c"
  DEPS
    iree::base
    iree::base::internal::flags
    iree::builtins::device
    iree::testing::benchmark
  TESTONLY
)

iree_cc_test(
  NAME
    libdevice_test
  SRCS
    "libdevice_test.cc"
  DEPS
    iree::base
    iree::base::internal
    iree::base::internal::flags
    iree::builtins::device
    iree::testing::gtest
    iree::testing::gtest_main
)

### BAZEL_TO_CMAKE_PRESERVES_ALL_CONTENT_BELOW_THIS_LINE ###
