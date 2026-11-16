#!/bin/bash
# Прошивка платы ESP32-S3 (Wi-Fi → USB принт-сервер). Запуск: ./proshit.sh
# Плату подключить кабелем к разъёму «COM» (левый USB-C).
set -e
cd "$(dirname "$0")"
BIN=esp-printer-full.bin
[ -f "$BIN" ] || { echo "Нет файла $BIN рядом со скриптом"; exit 1; }

# 1. esptool
if command -v esptool.py >/dev/null 2>&1; then ET=esptool.py
elif command -v esptool >/dev/null 2>&1; then ET=esptool
elif python3 -c 'import esptool' 2>/dev/null; then ET="python3 -m esptool"
else
  echo "Ставлю esptool..."
  if ! pip install --user esptool 2>/dev/null; then
    # Debian 12+ запрещает pip в систему — тогда отдельное окружение рядом
    command -v pip3 >/dev/null || sudo apt install -y python3-pip python3-venv
    python3 -m venv "$HOME/.esptool-venv" && "$HOME/.esptool-venv/bin/pip" install esptool
    ET="$HOME/.esptool-venv/bin/esptool.py"
  fi
  [ -n "$ET" ] || { export PATH="$HOME/.local/bin:$PATH"; ET=esptool.py; }
fi

# 2. порт: CH343 обычно /dev/ttyACM0, бывает /dev/ttyUSB0
PORT="${1:-}"
if [ -z "$PORT" ]; then
  for p in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do [ -e "$p" ] && { PORT=$p; break; }; done
fi
if [ -z "$PORT" ]; then
  echo "Плата не видна. Проверьте: кабель в разъёме «COM» (не «USB»), кабель с данными (не только зарядка)."
  echo "Можно указать порт вручную: ./proshit.sh /dev/ttyACM0"
  exit 1
fi
if [ ! -w "$PORT" ]; then
  echo "Нет доступа к $PORT. Один раз выполните:"
  echo "    sudo usermod -aG dialout $USER"
  echo "затем выйдите из системы и войдите снова (или перезагрузите ноутбук). Или запустите: sudo ./proshit.sh"
  exit 1
fi

echo "Прошиваю через $PORT ..."
# вывод сохраняем в proshit.log: по строкам Chip/Features/flash size видно, какая плата
set +e
$ET --chip esp32s3 -p "$PORT" -b 460800 write_flash 0x0 "$BIN" 2>&1 | tee proshit.log
RC=${PIPESTATUS[0]}
set -e
[ "$RC" = 0 ] || {
  echo
  echo "Не получилось. Зажмите кнопку BOOT, нажмите и отпустите RST, отпустите BOOT — и запустите скрипт ещё раз."
  echo "Если и так нет — попробуйте медленнее: $ET --chip esp32s3 -p $PORT -b 115200 write_flash 0x0 $BIN"
  exit 1
}
echo
echo "Готово. Нажмите на плате кнопку RST (или выньте и вставьте кабель)."
echo "Огонёк замигает синим — дальше настройка Wi-Fi через точку ESP-Printer (см. README)."
