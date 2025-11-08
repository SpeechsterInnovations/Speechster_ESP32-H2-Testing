#!/usr/bin/env bash
# Speechster-B1: Final Ignition Ritual (Seamless Version)
# --------------------------------------------------------
# Full first-boot ritual for Speechster-B1.
# Handles ESP-IDF export, flash, cinematic monitor, and cleanup.
# --------------------------------------------------------

set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
BAUD="${2:-115200}"
shift 2 || true

NO_REVEAL=false
for arg in "$@"; do
  if [ "$arg" = "--no-reveal" ]; then NO_REVEAL=true; fi
done

# --- Paths & dependencies ---
PY_MONITOR="./speechster_monitor_ritual.py"
SOCAT_LINK="/tmp/speechster_proxy"
FLASH_LOG="/tmp/speechster_flash.log"
SOCAT_LOG="/tmp/speechster_socat.log"

# --- Styling ---
cyan="\033[96m"; magenta="\033[95m"; yellow="\033[93m"; red="\033[91m"; reset="\033[0m"

# --- 1️⃣ Ensure ESP-IDF environment is loaded ---
if ! command -v idf.py >/dev/null 2>&1; then
  echo -e "${yellow}[WARN] ESP-IDF not exported — trying auto-export...${reset}"
  if [ -f /opt/esp-idf/export.sh ]; then
    bash -c ". /opt/esp-idf/export.sh" >/dev/null 2>&1 &
    sleep 1
    if ! command -v idf.py >/dev/null 2>&1; then
      echo -e "${red}[ERROR] Failed to auto-export ESP-IDF. Source it manually and rerun.${reset}"
      exit 1
    fi
  else
    echo -e "${red}[ERROR] /opt/esp-idf/export.sh not found. Please fix your installation.${reset}"
    exit 1
  fi
fi

# --- 2️⃣ Dependency check ---
for cmd in python3 socat; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo -e "${red}[ERROR] Missing dependency: $cmd${reset}"
    exit 1
  fi
done

# --- 3️⃣ Ritual header ---
clear
echo -e "${cyan}╔════════════════════════════════════════════════════════════╗${reset}"
echo -e "${cyan}║${reset}                        ${magenta}SPEECHSTER-B1${reset}                       ${cyan}║${reset}"
echo -e "${cyan}║${reset}            ${yellow}“Where overheating meets overengineering.”${reset}       ${cyan}║${reset}"
echo -e "${cyan}╚════════════════════════════════════════════════════════════╝${reset}"
echo
echo -e "${cyan}[INIT]${reset} Port: $PORT    Baud: $BAUD"
echo -e "${cyan}[INIT]${reset} Flash log: $FLASH_LOG"
echo -e "${cyan}[INIT]${reset} Proxy: $SOCAT_LINK"
echo

# --- 4️⃣ Flash firmware ---
echo -e "${cyan}[FLASH]${reset} Starting idf.py flash..."
idf.py -p "$PORT" flash >"$FLASH_LOG" 2>&1 &
FLASH_PID=$!

# Spinner while flashing
sp="/-\\|"
i=0
while kill -0 $FLASH_PID 2>/dev/null; do
  printf "\r${yellow}[ANXIETY]${reset} %s Waiting for ESP to accept its fate..." "${sp:i++%${#sp}:1}"
  sleep 0.2
done
wait $FLASH_PID || {
  echo -e "\n${red}[FAIL]${reset} Flash failed. See $FLASH_LOG"
  tail -n 20 "$FLASH_LOG" || true
  exit 2
}
echo -e "\n${cyan}[OK]${reset} Flash complete."

# --- 5️⃣ Setup socat proxy ---
rm -f "$SOCAT_LINK" "$SOCAT_LOG"
socat -d -d PTY,raw,echo=0,link="$SOCAT_LINK" FILE:"$PORT",raw,echo=0 >"$SOCAT_LOG" 2>&1 &
SOCAT_PID=$!
sleep 0.5
if [ ! -e "$SOCAT_LINK" ]; then
  echo -e "${red}[ERROR]${reset} Failed to create serial proxy."
  tail -n 10 "$SOCAT_LOG" || true
  kill "$SOCAT_PID" 2>/dev/null || true
  exit 3
fi

# --- 6️⃣ Launch cinematic monitor ---
echo -e "${cyan}[RITUAL]${reset} Beginning monitor sequence..."
sleep 0.5
clear
PY_ARGS=("$SOCAT_LINK" "$BAUD")
if [ "$NO_REVEAL" = true ]; then PY_ARGS+=("--no-reveal"); fi

python3 "$PY_MONITOR" "${PY_ARGS[@]}"

# --- 7️⃣ Cleanup ---
echo
echo -e "${cyan}[CLEANUP]${reset} Closing proxy and restoring sanity..."
kill "$SOCAT_PID" 2>/dev/null || true
rm -f "$SOCAT_LINK" || true
tput cnorm || true

echo -e "${magenta}[DONE]${reset} Speechster-B1 ignition sequence complete."
echo
