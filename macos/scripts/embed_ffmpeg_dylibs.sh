#!/bin/sh
set -eu

FFMPEG_ROOT="${PROJECT_DIR}/../.toolchains/ffmpeg/macos-arm64"
FFMPEG_LIB_DIR="${FFMPEG_ROOT}/lib"
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

copy_library_family() {
  library="$1"
  real_path="$(find "${FFMPEG_LIB_DIR}" -maxdepth 1 -type f -name "${library}.*.dylib" | sort | tail -n 1)"
  if [ -z "${real_path}" ]; then
    echo "error: missing FFmpeg dylib for ${library} in ${FFMPEG_LIB_DIR}" >&2
    exit 1
  fi

  real_name="$(basename "${real_path}")"
  copy_dylib "${real_name}"

  major_name="$(printf "%s\n" "${real_name}" | sed -E "s/^(${library})\\.([0-9]+)(\\..*)?\\.dylib$/\\1.\\2.dylib/")"
  if [ "${major_name}" != "${real_name}" ]; then
    copy_symlink "${major_name}"
  fi
  copy_symlink "${library}.dylib"
}

copy_library_family libavcodec
copy_library_family libavformat
copy_library_family libavutil
copy_library_family libswresample

mkdir -p "${NOTICE_DIR}"
cp -f "${FFMPEG_ROOT}/README.txt" "${NOTICE_DIR}/README.txt"
cp -f "${FFMPEG_ROOT}/VOIDPLAYER_BUILD.md" "${NOTICE_DIR}/VOIDPLAYER_BUILD.md"
cp -f "${FFMPEG_ROOT}/voidplayer-ffmpeg-manifest.json" \
  "${NOTICE_DIR}/voidplayer-ffmpeg-manifest.json"
rm -rf "${NOTICE_DIR}/LICENSES"
cp -R "${FFMPEG_ROOT}/LICENSES" "${NOTICE_DIR}/LICENSES"
