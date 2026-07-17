#!/usr/bin/env bash
# 编译并合成 Metalio Claw4 完整固件：
#   build/（不含唤醒词） + wakeword/srmodels.bin + esp_claw_bin/
# 输出：firmware/Metalio_Claw4_{PROJECT_VER}.bin
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

FIRMWARE_DIR="firmware"

# ---------- IDF 环境 ----------
# 优先使用已激活环境；否则按候选路径 source export.sh。
# 也可手动指定：IDF_PATH=/path/to/esp-idf ./merge_firmware.sh
setup_idf() {
  if command -v idf.py >/dev/null 2>&1 && [[ -n "${IDF_PATH:-}" ]]; then
    echo "[idf] 已激活: IDF_PATH=${IDF_PATH}"
    return 0
  fi

  if [[ -z "${IDF_PATH:-}" ]] || [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    local candidates=(
      "${HOME}/esp/v5.5.4"
      "${HOME}/esp/esp-idf"
      "${HOME}/esp-idf"
      "${HOME}/.espressif/v5.5.4/esp-idf"
      "/opt/esp/idf"
    )
    local c
    for c in "${candidates[@]}"; do
      if [[ -f "${c}/export.sh" ]]; then
        IDF_PATH="$c"
        break
      fi
    done
  fi

  if [[ -z "${IDF_PATH:-}" ]] || [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    echo "错误: 未找到 ESP-IDF export.sh。请设置 IDF_PATH 后重试，例如:" >&2
    echo "  export IDF_PATH=~/esp/v5.5.4 && ./merge_firmware.sh" >&2
    exit 1
  fi

  echo "[idf] source ${IDF_PATH}/export.sh"
  # shellcheck disable=SC1091
  source "${IDF_PATH}/export.sh"

  if ! command -v idf.py >/dev/null 2>&1; then
    echo "错误: source 后仍找不到 idf.py" >&2
    exit 1
  fi
}

# ---------- 读版本 ----------
get_project_ver() {
  local ver
  ver="$(sed -n 's/^set(PROJECT_VER "\([^"]*\)").*/\1/p' CMakeLists.txt | head -1)"
  if [[ -z "$ver" ]]; then
    echo "错误: 无法从 CMakeLists.txt 解析 PROJECT_VER" >&2
    exit 1
  fi
  echo "$ver"
}

# ---------- 收集 merge 参数 ----------
# 输出空格分隔的 "addr file" 对到全局数组 MERGE_ARGS
collect_merge_args() {
  MERGE_ARGS=()

  local flash_args="build/flash_args"
  if [[ ! -f "$flash_args" ]]; then
    echo "错误: 缺少 ${flash_args}，请先完成编译" >&2
    exit 1
  fi

  local ww_addr=""
  # build/flash_args：跳过唤醒词 srmodels，其余全部纳入
  while read -r addr file; do
    [[ -z "${addr:-}" ]] && continue
    [[ "$addr" == --* ]] && continue
    if [[ "$file" == *srmodels* ]]; then
      ww_addr="$addr"
      echo "[skip] 跳过 build 唤醒词: ${addr} ${file}"
      continue
    fi
    local path="build/${file}"
    if [[ ! -f "$path" ]]; then
      echo "错误: 缺少固件文件 ${path}" >&2
      exit 1
    fi
    echo "[build] ${addr} ${path}"
    MERGE_ARGS+=("$addr" "$path")
  done < <(grep -E '^[0-9a-fxA-FX]+[[:space:]]' "$flash_args" || true)

  # 自定义唤醒词（地址优先取自 flash_args 中的 model 段）
  local ww="wakeword/srmodels.bin"
  ww_addr="${ww_addr:-0x111000}"
  if [[ ! -f "$ww" ]]; then
    echo "错误: 缺少 ${ww}" >&2
    exit 1
  fi
  echo "[wakeword] ${ww_addr} ${ww}"
  MERGE_ARGS+=("$ww_addr" "$ww")

  # esp_claw_bin：从文件名解析地址 {name}_0xADDR.bin
  local claw_dir="esp_claw_bin"
  if [[ ! -d "$claw_dir" ]]; then
    echo "错误: 缺少目录 ${claw_dir}" >&2
    exit 1
  fi
  local f name addr
  shopt -s nullglob
  for f in "${claw_dir}"/*.bin; do
    name="$(basename "$f")"
    if [[ "$name" =~ _0[xX]([0-9a-fA-F]+)\.bin$ ]]; then
      addr="0x${BASH_REMATCH[1]}"
      echo "[esp_claw] ${addr} ${f}"
      MERGE_ARGS+=("$addr" "$f")
    else
      echo "[warn] 跳过无法解析地址的文件: ${f}" >&2
    fi
  done
  shopt -u nullglob

  if [[ ${#MERGE_ARGS[@]} -eq 0 ]]; then
    echo "错误: 没有可合并的固件段" >&2
    exit 1
  fi
}

# ---------- 主流程 ----------
main() {
  setup_idf

  local ver
  ver="$(get_project_ver)"
  mkdir -p "$FIRMWARE_DIR"
  local out="${FIRMWARE_DIR}/Metalio_Claw4_${ver}.bin"
  echo "[ver] PROJECT_VER=${ver} -> ${out}"

  echo "[build] idf.py build ..."
  idf.py build

  collect_merge_args

  # 与 build/flasher_args.json 一致
  local chip="${ESPTOOL_CHIP:-esp32p4}"
  local flash_mode="${FLASH_MODE:-dio}"
  local flash_freq="${FLASH_FREQ:-40m}"
  local flash_size="${FLASH_SIZE:-32MB}"

  echo "[merge] esptool.py merge_bin -> ${out}"
  esptool.py --chip "$chip" merge_bin \
    -o "$out" \
    --flash_mode "$flash_mode" \
    --flash_freq "$flash_freq" \
    --flash_size "$flash_size" \
    "${MERGE_ARGS[@]}"

  local latest="Metalio_Claw4_Latest.bin"
  cp -f "$out" "$latest"

  local size
  size="$(wc -c < "$out" | tr -d ' ')"
  echo "[done] ${SCRIPT_DIR}/${out} (${size} bytes)"
  echo "[done] ${SCRIPT_DIR}/${latest}"
}

main "$@"
