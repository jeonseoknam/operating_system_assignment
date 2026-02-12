#!/usr/bin/env bash
set -euo pipefail

XV6_REPO_URL="https://github.com/mit-pdos/xv6-public.git"
ASSIGN_REPO_URL="https://github.com/jeonseoknam/operating_system_assignment.git"

XV6_DIR="xv6-public_assignment_3"
ASSIGN_DIR="operating_system_assignment"
ASSIGN_SUBDIR="assignment_3/sourcecode"

# 1) clone xv6-public (없으면)
if [ ! -d "$XV6_DIR/.git" ]; then
	git clone "$XV6_REPO_URL" "$XV6_DIR"
else
	echo "[INFO] $XV6_DIR exists. Skipping clone."
fi

# 2) clone assignment repo (없으면)
#if [! -d "$ASSIGN_DIR/.git" ]; then
#	git clone "$ASSIGN_REPO_URL" "$ASSIGN_DIR"
#else
#	echo "[INFO] $ASSIGN_DIR exists. Pulling latest..."
#	git -C "$ASSIGN_DIR" pull
#fi

SRC_PATH="$ASSIGN_SUBDIR"
DST_PATH="$XV6_DIR"

# 3) sourcecode에 있는 파일만 xv6-public에 덮어쓰기
if [ ! -d "$SRC_PATH" ]; then
	echo "[ERROR] Source path not found: $SRC_PATH"
	exit 1
fi

echo "[INFO] Overwriting files from $SRC_PATH -> $DST_PATH ..."
# -a: 권한/타임스템프 유지, 디렉토리 재귀 포함
# src 뒤에 /.를 붙이면 "sourcecode 폴더 자체"가 아니라 "그 안의 내용"만 복사하는 것
cp -a "$SRC_PATH"/. "$DST_PATH"/

# 4) build
echo "[INFO] Running make in $XV6_DIR ..."
make -C "$XV6_DIR"

echo "[DONE] Build completed."

