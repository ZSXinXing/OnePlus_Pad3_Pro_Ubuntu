#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"

uuid=opd2513-torch@local
[ "$(id -u)" = 0 ]
root= live=1
case $# in
    0) ;;
    2) [ "$1" = --root ] && [ -n "$2" ] && [ "$2" != / ] || exit 2
       root=$2 live=0 ;;
    *) exit 2 ;;
esac
test -f "$root/etc/os-release"
case "$(chroot "${root:-/}" /usr/bin/dpkg-query -W -f='${Version}' gnome-shell)" in
    50.*) ;;
    *) echo 'This flashlight build requires GNOME Shell 50.x' >&2; exit 1 ;;
esac

target="$root/usr/share/gnome-shell/extensions/$uuid"
schemas="$root/usr/share/glib-2.0/schemas"
units="$root/etc/systemd/system"
install -D -m 0755 opd2513-torch "$root/usr/local/libexec/opd2513-torch"
install -d -m 0755 "$target" "$units"
install -m 0644 opd2513-torch.socket opd2513-torch@.service "$units/"
install -m 0644 extension/extension.js extension/metadata.json extension/stylesheet.css \
    extension/torch-symbolic.svg "$target/"
install -m 0644 90-opd2513-torch.gschema.override "$schemas/"
chroot "${root:-/}" /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas

if [ "$live" = 0 ]; then
    chroot "$root" /usr/bin/systemctl --root=/ enable opd2513-torch.socket
    exit 0
fi

systemctl daemon-reload
systemctl enable --now opd2513-torch.socket
user=${SUDO_USER:-jixing}
home=$(getent passwd "$user" | cut -d: -f6)
uid=$(id -u "$user")
[ -S "/run/user/$uid/bus" ] || exit 0
runuser -u "$user" -- env "HOME=$home" "XDG_RUNTIME_DIR=/run/user/$uid" \
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus" python3 - "$uuid" <<'PY'
import sys
from gi.repository import Gio

settings = Gio.Settings.new('org.gnome.shell')
uuid = sys.argv[1]
enabled = settings.get_strv('enabled-extensions')
if uuid not in enabled or settings.get_user_value('enabled-extensions') is None:
    assert settings.set_strv('enabled-extensions', list(dict.fromkeys([*enabled, uuid])))
Gio.Settings.sync()
PY
