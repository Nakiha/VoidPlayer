#!/bin/sh
set -eu

FFMPEG_LIB_DIR="${PROJECT_DIR}/../third_party/ffmpeg/lib"
FFMPEG_ROOT="${PROJECT_DIR}/../third_party/ffmpeg"
FRAMEWORKS_DIR="${TARGET_BUILD_DIR}/${FRAMEWORKS_FOLDER_PATH}"
NOTICE_DIR="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/ThirdParty/ffmpeg"

mkdir -p "${FRAMEWORKS_DIR}"

copy_dylib() {
  src="${FFMPEG_LIB_DIR}/$1"
  dst="${FRAMEWORKS_DIR}/$1"
  if [ ! -f "${src}" ]; then
    echo "error: missing FFmpeg dylib: ${src}" >&2
    exit 1
  fi

  cp -f "${src}" "${dst}"
  chmod u+w "${dst}"

  if [ "${CODE_SIGNING_ALLOWED:-YES}" != "NO" ]; then
    identity="${EXPANDED_CODE_SIGN_IDENTITY:-}"
    if [ -n "${identity}" ]; then
      codesign --force --sign "${identity}" --timestamp=none "${dst}"
    else
      codesign --force --sign - --timestamp=none "${dst}"
    fi
  fi
}

copy_symlink() {
  src="${FFMPEG_LIB_DIR}/$1"
  dst="${FRAMEWORKS_DIR}/$1"
  if [ ! -L "${src}" ]; then
    echo "error: missing FFmpeg dylib symlink: ${src}" >&2
    exit 1
  fi

  ln -sfn "$(readlink "${src}")" "${dst}"
}

copy_dylib libavcodec.62.28.100.dylib
copy_dylib libavformat.62.12.100.dylib
copy_dylib libavutil.60.26.100.dylib
copy_dylib libswresample.6.3.100.dylib

copy_symlink libavcodec.62.dylib
copy_symlink libavcodec.dylib
copy_symlink libavformat.62.dylib
copy_symlink libavformat.dylib
copy_symlink libavutil.60.dylib
copy_symlink libavutil.dylib
copy_symlink libswresample.6.dylib
copy_symlink libswresample.dylib

mkdir -p "${NOTICE_DIR}"
cp -f "${FFMPEG_ROOT}/README.txt" "${NOTICE_DIR}/README.txt"
cp -f "${FFMPEG_ROOT}/VOIDPLAYER_BUILD.md" "${NOTICE_DIR}/VOIDPLAYER_BUILD.md"
cp -f "${FFMPEG_ROOT}/voidplayer-ffmpeg-manifest.json" \
  "${NOTICE_DIR}/voidplayer-ffmpeg-manifest.json"
rm -rf "${NOTICE_DIR}/LICENSES"
cp -R "${FFMPEG_ROOT}/LICENSES" "${NOTICE_DIR}/LICENSES"
