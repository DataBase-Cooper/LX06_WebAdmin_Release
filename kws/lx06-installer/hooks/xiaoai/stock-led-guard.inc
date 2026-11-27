# LX06 dialogue light lease
# Keep native cleanup for every other effect. The managed session clears
# effects 1–3 itself while it still owns the light lease.
case "$0:${1:-}" in
    */shut_led:1|*/shut_led:2|*/shut_led:3)
        _lx06_led_dir=/tmp/lx06-dialog-led
        if [ -x /usr/bin/flock ]; then
            (umask 077; mkdir "$_lx06_led_dir") 2>/dev/null || :
            if [ -d "$_lx06_led_dir" ] && [ ! -L "$_lx06_led_dir" ] &&
               [ -O "$_lx06_led_dir" ] && [ ! -L "$_lx06_led_dir/lease" ]; then
                umask 077
                exec 9>>"$_lx06_led_dir/lease"
                /usr/bin/flock -n -x 9 || exit 0
            fi
        fi
        ;;
esac
