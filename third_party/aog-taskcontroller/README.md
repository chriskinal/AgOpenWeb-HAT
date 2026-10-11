# AOG-TaskController (part)

`task_controller.cpp`, `task_controller.hpp`, and the status screen for a virtual terminal,
`AOG_TC.iop` (the object pool) with `AOG_TC.iop.h` (its object IDs, from `resources/`), from
https://github.com/AgOpenGPS-Official/AOG-TaskController at commit
`4ede403` (2026-10), GPL-3.0, copyright Daan Steenbergen. **Unmodified**: keep them that way so
upstream fixes can be copied over.

The two headers the file expects from the rest of that project, `logging_utils.hpp` and
`settings.hpp`, are replaced by the daemon's own in `daemon/isobus_compat/`. The rest of
upstream (its UDP layer, settings file, Windows CAN drivers) is not used; `daemon/isobus.cpp`
and `daemon/isobus_vt.cpp` do that job inside `aow-hatd`. See docs/PLAN.md section 8.
