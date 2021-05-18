#!/bin/sh

# it is supposed to be run by travis-ci
# expects a few env variables to be set:
#   BUILD_DIR - the working directory, where to build
#   INSTALL_DIR - the installation prefix.
#   SRC_DIR - read-only directory with git checkout to compile
#   CC, CXX, CFLAGS, CXXFLAGS are not required, should make sense too
#   TARGET - either build or usermanual
#   ECO - some other flags for cmake

set -ex

CMAKE_BUILD_TYPE="RelWithDebInfo"
GENERATOR="Ninja"
VERBOSE="-v"
KEEPGOING="-k0"

case "$FLAVOR" in
  "Coverage")
    CMAKE_BUILD_TYPE="Coverage"
    G="Unix Makefiles"
    ;;
  *)
    ;;
esac

case "$TARGET" in
  "WWW")
    G="Unix Makefiles"
    ECO="${ECO} -DBUILD_DOCS=ON"
    ;;
  *)
    ;;
esac

if [ ! -z "${G+x}" ];
then
  GENERATOR="$G"
fi

if [ "$GENERATOR" = "Unix Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
fi;

if [ -z "${MAKEFLAGS+x}" ];
then
  MAKEFLAGS="-j2 $VERBOSE"
fi

target_build()
{
  # to get as much of the issues into the log as possible
  cmake --build "$BUILD_DIR" -- $MAKEFLAGS || cmake --build "$BUILD_DIR" -- -j1 $VERBOSE $KEEPGOING

  ctest --output-on-failure || ctest --rerun-failed -V -VV

  # and now check that it installs where told and only there.
  cmake --build "$BUILD_DIR" --target install -- $MAKEFLAGS || cmake --build "$BUILD_DIR" --target install -- -j1 $VERBOSE $KEEPGOING
}

target_www()
{
  cmake --build "$BUILD_DIR" -- $VERBOSE docs
}

handle_coverage_data()
{
  cmake --build "$BUILD_DIR" --target gcov
  mkdir "$BUILD_DIR/gcov-reports-unittest"
  find "$BUILD_DIR" -maxdepth 1 -iname '*.gcov' -exec mv "{}" "$BUILD_DIR/gcov-reports-unittest" \;
}

handle_sample_coverage_data()
{
  cmake --build "$BUILD_DIR" --target gcov-clean
  cmake --build "$BUILD_DIR" --target rstest-check
  cmake --build "$BUILD_DIR" --target gcov
  mkdir "$BUILD_DIR/gcov-reports-rsa"
  find "$BUILD_DIR" -maxdepth 1 -iname '*.gcov' -exec mv "{}" "$BUILD_DIR/gcov-reports-rsa" \;
}

diskspace()
{
  df
  du -hcs "$SRC_DIR"
  du -hcs "$BUILD_DIR"
  du -hcs "$INSTALL_PREFIX"
}

diskspace

cd "$BUILD_DIR"
cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -G"$GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)

case "$TARGET" in
  "build")
    target_build
    ;;
  "WWW")
    target_www
    ;;
  *)
    exit 1
    ;;
esac

case "$FLAVOR" in
  "Coverage")
    handle_coverage_data

    substring="RAWSPEED_ENABLE_SAMPLE_BASED_TESTING"
    if [ "${ECO#*$substring}" != "$ECO" ];
    then
      handle_sample_coverage_data
    fi
    ;;
  *)
    ;;
esac

diskspace
