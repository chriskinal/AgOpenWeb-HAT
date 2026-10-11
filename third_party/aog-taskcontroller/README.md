# AOG-TaskController (part)

`task_controller.cpp` and `task_controller.hpp` from
https://github.com/AgOpenGPS-Official/AOG-TaskController at commit
`4ede403` (2026-10), GPL-3.0, copyright Daan Steenbergen. **Unmodified**: keep them that way so
upstream fixes can be copied over.

The two headers the file expects from the rest of that project, `logging_utils.hpp` and
`settings.hpp`, are replaced by the daemon's own in `daemon/isobus_compat/`. The rest of
upstream (its UDP layer, settings file, virtual terminal page, Windows CAN drivers) is not used;
`daemon/isobus.cpp` does that job inside `aow-hatd`. See docs/PLAN.md section 8.
