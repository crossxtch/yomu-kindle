#!/bin/sh
# Launch yomu on a jailbroken Kindle.
#
# The USB volume is a fuse mount. KOReader copies its launcher to /var/tmp
# before doing anything hard to undo, because an update or a flaky write can
# truncate the script that is still running. This script does the same.
#
# With the framework left up, pillow and awesome are paused so the Kindle UI
# does not paint over the framebuffer. --framework_stop tears lab126_gui down
# instead, which is the "no framework" KUAL entry and frees more RAM.
#
# Each step is appended to yomu.log and synced. A start that dies or hangs
# leaves the last completed step on the USB volume.

export LC_ALL=C

# Fixed path so a failed directory lookup still leaves a line on the USB volume.
LOG=/mnt/us/yomu/yomu.log
printf '%s\n' "yomu: enter pid=$$ from=$0 args=${*:-<none>}" >>"$LOG" 2>/dev/null || true
sync >/dev/null 2>&1 || true
eips 1 20 "yomu" >/dev/null 2>&1 || true

if [ -z "${YOMU_DIR:-}" ]; then
  YOMU_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd) || {
    printf '%s\n' "yomu: cannot cd from $0" >>"$LOG" 2>/dev/null || true
    sync >/dev/null 2>&1 || true
    exit 1
  }
  export YOMU_DIR
fi

LOG="$YOMU_DIR/yomu.log"
log() {
  ts=$(date '+%H:%M:%S' 2>/dev/null || printf '??')
  printf '%s\n' "yomu: $ts $1" >>"$LOG" 2>/dev/null || true
  sync >/dev/null 2>&1 || true
  printf '%s\n' "yomu: $ts $1"
}

log "enter pid=$$ from=$0 args=${*:-<none>} dir=$YOMU_DIR"

if [ "$(dirname "$0")" != "/var/tmp" ]; then
  log "copy launcher to /var/tmp"
  if ! cp -f "$0" /var/tmp/yomu.sh; then
    log "copy launcher to /var/tmp failed"
    exit 1
  fi
  if ! chmod 755 /var/tmp/yomu.sh; then
    log "chmod /var/tmp/yomu.sh failed"
    exit 1
  fi
  log "re-exec /var/tmp/yomu.sh"
  exec /var/tmp/yomu.sh "$@"
fi

log "launcher is /var/tmp/yomu.sh"
if ! cd "$YOMU_DIR"; then
  log "cd $YOMU_DIR failed"
  exit 1
fi

FROM_KUAL=no
STOP_FRAMEWORK=no
if [ "${1:-}" = "--kual" ]; then
  FROM_KUAL=yes
  shift
fi
if [ "${1:-}" = "--framework_stop" ]; then
  STOP_FRAMEWORK=yes
  shift
fi
log "flags kual=$FROM_KUAL framework_stop=$STOP_FRAMEWORK rest=${*:-<none>}"

if [ -e /proc/keypad ]; then
  log "unlock keypad"
  echo unlock >/proc/keypad
fi
if [ -e /proc/fiveway ]; then
  log "unlock fiveway"
  echo unlock >/proc/fiveway
fi

if [ "$(nice 2>/dev/null)" = "5" ]; then
  log "renice"
  renice -n -5 $$ >/dev/null 2>&1 || true
fi

INIT=sysv
if [ -d /etc/upstart ]; then
  INIT=upstart
fi
log "init=$INIT"

AWESOME_STOPPED=no
PILLOW_DISABLED=no
VOLUMD_STOPPED=no
PASSCODE=no
SAVER=no
SAVER_TIMEOUT=""
STOPPED_JOBS=""

cleanup() {
  log "cleanup begin awesome=$AWESOME_STOPPED pillow=$PILLOW_DISABLED framework_stop=$STOP_FRAMEWORK volumd=$VOLUMD_STOPPED"
  if [ "$VOLUMD_STOPPED" = yes ]; then
    log "cleanup continue volumd"
    killall -CONT volumd >/dev/null 2>&1 || true
  fi
  if [ "$SAVER" = yes ]; then
    log "cleanup restore screenSaverTimeout $SAVER_TIMEOUT"
    lipc-set-prop -i com.lab126.powerd screenSaverTimeout "$SAVER_TIMEOUT" >/dev/null 2>&1 || true
  fi
  if [ -f /var/tmp/yomu-fl ]; then
    FL=$(tr -cd '0-9' < /var/tmp/yomu-fl || true)
    if [ -n "$FL" ] && [ "$FL" != 0 ]; then
      log "frontlight restore $FL"
      lipc-set-prop -i com.lab126.powerd flIntensity "$FL" >/dev/null 2>&1 || true
    fi
    rm -f /var/tmp/yomu-fl
  fi
  log "cleanup preventScreenSaver 0"
  lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || true
  if [ "$STOP_FRAMEWORK" = yes ]; then
    log "restarting framework"
    if [ "$INIT" = sysv ]; then
      /etc/init.d/framework start >/dev/null 2>&1 || true
    else
      start lab126_gui >/dev/null 2>&1 || true
    fi
    log "cleanup framework start returned"
  else
    for job in $STOPPED_JOBS; do
      log "cleanup start $job"
      start "$job" >/dev/null 2>&1 || true
    done
    if [ "$AWESOME_STOPPED" = yes ]; then
      log "cleanup continue awesome"
      killall -CONT awesome >/dev/null 2>&1 || true
    fi
    if [ "$PILLOW_DISABLED" = yes ]; then
      if [ -f /var/tmp/yomu-fb.dump ]; then
        log "cleanup restore framebuffer"
        cat /var/tmp/yomu-fb.dump >/dev/fb0 2>/dev/null || true
        rm -f /var/tmp/yomu-fb.dump
      fi
      log "cleanup enable pillow"
      lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
      log "cleanup start home"
      lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || true
    fi
  fi
  if [ "$PASSCODE" = yes ]; then
    log "cleanup restore passcode"
    touch /var/local/system/userpasswdenabled
  fi
  log "cleanup remove /var/tmp copies"
  rm -f /var/tmp/yomu.sh /var/tmp/yomu.bin
  log "cleanup done"
}

trap cleanup EXIT INT TERM

if [ -f /var/local/system/userpasswdenabled ]; then
  log "pause system passcode"
  rm -f /var/local/system/userpasswdenabled
  PASSCODE=yes
  log "system passcode paused"
fi

if [ "$STOP_FRAMEWORK" = yes ]; then
  log "stopping framework"
  trap '' TERM
  if [ "$INIT" = sysv ]; then
    /etc/init.d/framework stop >/dev/null 2>&1 || true
  else
    stop lab126_gui >/dev/null 2>&1 || true
  fi
  log "framework stop returned"
  sleep 1
  trap cleanup EXIT INT TERM
elif [ "$INIT" = upstart ]; then
  log "dump framebuffer"
  cat /dev/fb0 >/var/tmp/yomu-fb.dump 2>/dev/null || true
  log "framebuffer dumped"
  log "disable pillow"
  if lipc-set-prop com.lab126.pillow disableEnablePillow disable >/dev/null 2>&1; then
    PILLOW_DISABLED=yes
    log "pillow disabled"
  else
    log "pillow disable failed"
  fi
  log "stop awesome"
  if killall -STOP awesome >/dev/null 2>&1; then
    AWESOME_STOPPED=yes
    log "awesome stopped"
  else
    log "awesome not stopped"
  fi
  JOBS="stored webreader kfxreader kfxview todo tmd rcm archive scanner otav3 otaupd"
  if [ -f /etc/upstart/statusbar.conf ]; then
    JOBS="$JOBS statusbar"
  fi
  for job in $JOBS; do
    log "stop job $job"
    if stop "$job" >/dev/null 2>&1; then
      STOPPED_JOBS="$STOPPED_JOBS $job"
      log "stopped $job"
    else
      log "job $job not stopped"
    fi
  done
  if [ "$FROM_KUAL" = yes ]; then
    log "wait 2s for KUAL"
    sleep 2
  fi
fi

log "stop volumd"
if killall -STOP volumd >/dev/null 2>&1; then
  VOLUMD_STOPPED=yes
  log "volumd stopped"
else
  log "volumd not stopped"
fi

log "clear preventScreenSaver"
lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || log "preventScreenSaver set failed"
log "read screenSaverTimeout"
SAVER_TIMEOUT=$(lipc-get-prop -i com.lab126.powerd screenSaverTimeout 2>/dev/null | tr -cd '0-9' || true)
log "screenSaverTimeout was ${SAVER_TIMEOUT:-empty}"
if [ -n "$SAVER_TIMEOUT" ] && lipc-set-prop -i com.lab126.powerd screenSaverTimeout 86400 >/dev/null 2>&1; then
  SAVER=yes
  log "idle sleep held off"
else
  log "idle sleep not held"
fi
log "read power state"
POWER_STATE=$(lipc-get-prop -s com.lab126.powerd state 2>&1 | tr -d '\r' || true)
POWER_PREVENT=$(lipc-get-prop -i com.lab126.powerd preventScreenSaver 2>&1 | tr -cd '0-9' || true)
POWER_TIMEOUT=$(lipc-get-prop -i com.lab126.powerd screenSaverTimeout 2>&1 | tr -cd '0-9' || true)
log "power state ${POWER_STATE:-unread} prevent ${POWER_PREVENT:-unread} timeout ${SAVER_TIMEOUT:-unread}->${POWER_TIMEOUT:-unread} awesome $AWESOME_STOPPED framework $STOP_FRAMEWORK"

chmod 755 "$YOMU_DIR/yomu" 2>/dev/null || true
if [ ! -f "$YOMU_DIR/yomu" ]; then
  log "missing $YOMU_DIR/yomu"
  exit 1
fi
log "copy binary to /var/tmp bytes=$(wc -c < "$YOMU_DIR/yomu" 2>/dev/null | tr -d ' ')"
if ! cp -f "$YOMU_DIR/yomu" /var/tmp/yomu.bin; then
  log "copy binary to /var/tmp failed"
  exit 1
fi
if ! chmod 755 /var/tmp/yomu.bin; then
  log "chmod /var/tmp/yomu.bin failed"
  exit 1
fi

export YOMU_HOME="$YOMU_DIR"
export YOMU_ROOT="${YOMU_ROOT:-/mnt/us/yomu/books}"
export AWESOME_STOPPED
export STOP_FRAMEWORK
log "mkdir $YOMU_ROOT"
mkdir -p "$YOMU_ROOT" || { log "mkdir $YOMU_ROOT failed"; exit 1; }

log "starting"
/var/tmp/yomu.bin "$@"
RC=$?
log "exit $RC"
exit "$RC"
