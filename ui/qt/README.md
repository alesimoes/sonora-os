# sonora-ui

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Normal quit |
| 1 | The interface never ran (engine refused, QML error, refusal page closed) |
| 2 | Bad command line |
| 3 | Unfit display (ADR-0010 section 4): under `--kiosk` the platform is not Wayland. `sonora-ui.service` lists 3 in `RestartPreventExitStatus=`, so the unit fails at once and `OnFailure=` switches the console to tty2 (the terminal UI runs there only from ADR-0010 stage 2; until then tty2 is an empty console). |

## Kiosk

`--kiosk` makes no full-screen request. The window's `app_id` is `sonora-ui`
(`setDesktopFileName`), which the labwc rule in
`iso/config/includes.chroot/usr/share/sonora/labwc/rc.xml` matches to maximize
it and keep it below plugin editors. The first frame logs
`sonora-ui: first frame after N ms` on stderr (journal under the unit).

Not yet done: `sd_notify` READY/WATCHDOG through `common/platform`.
