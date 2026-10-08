[ -n "$XDG_RUNTIME_DIR" ] || [ ! -d "/run/user/$(id -u)" ] || export XDG_RUNTIME_DIR="/run/user/$(id -u)"
grep -qs '^DRIVER=virtio_gpu$' /sys/devices/card0/uevent || export WLR_RENDERER_ALLOW_SOFTWARE=1
