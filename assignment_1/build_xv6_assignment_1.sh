#!/usr/bin/env bash
set -euo pipefail

XV6_REPO_URL="https://github.com/mit-pdos/xv6-public.git"
ASSIGN_REPO_URL="https://github.com/jeonseoknam/operating_system_assignment.git"

XV6_DIR="xv6-public_assignment_1"
ASSIGN_DIR="operating_system_assignment"
ASSIGN_SUBDIR="sourcecode"

# 0) required packages install
echo "[INFO] Installing required packages..."
sudo apt update
sudo apt install -y \
	build-essential \
	gcc \
	gdb \
	make \
	git \
	qemu-system-x86

# 1) clone xv6-public (없으면)
if [ ! -d "$XV6_DIR/.git" ]; then
	git clone "$XV6_REPO_URL" "$XV6_DIR"
else
	echo "[INFO] $XV6_DIR exists. Skipping clone."
fi


SRC_PATH="$ASSIGN_SUBDIR"
DST_PATH="$XV6_DIR"

# 2) sourcecode에 있는 파일만 xv6-public에 덮어쓰기
if [ ! -d "$SRC_PATH" ]; then
	echo "[ERROR] Source path not found: $SRC_PATH"
	exit 1
fi

echo "[INFO] Overwriting files from $SRC_PATH -> $DST_PATH ..."
# -a: 권한/타임스템프 유지, 디렉토리 재귀 포함
# src 뒤에 /.를 붙이면 "sourcecode 폴더 자체"가 아니라 "그 안의 내용"만 복사하는 것
cp -a "$SRC_PATH"/. "$DST_PATH"/

# 3) build
echo "[INFO] Running make in $XV6_DIR ..."
make -C "$XV6_DIR"

echo "[DONE] Build completed."

