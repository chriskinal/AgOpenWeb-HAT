// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The task controller's status screen on a tractor's terminal (an ISOBUS virtual terminal).
//
// The screen is AOG-TaskController's object pool, third_party/aog-taskcontroller/AOG_TC.iop,
// unmodified, and this does what upstream's app.cpp does with it: five pages (status, network,
// implement, diagnostics, settings), filled from the daemon's state. Nothing happens until a
// terminal is on the bus.
//
// Used from the ISOBUS thread only (daemon/isobus.cpp), in its loop.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "aow/status.h"

namespace isobus {
class InternalControlFunction;
}

namespace aow {

class IsobusVt {
public:
    // What the screen shows, gathered by the ISOBUS thread each pass.
    struct View {
        IsobusStatus status;
        uint64_t implementName = 0;  // the implement's NAME; 0 = none
        bool appConnected = false;
        int32_t speedMmS = 0;
        int32_t xteMm = 0;
        uint32_t distanceMm = 0;     // what the tractor ECU role has summed
        bool tractorEcuSetting = false;
        std::string ip;              // this unit's address
    };

    // setTractorEcu: the operator changed "TECU" on the settings page of the screen.
    IsobusVt(std::shared_ptr<isobus::InternalControlFunction> tc, std::function<void(bool)> setTractorEcu);
    ~IsobusVt();

    // Call every pass. True while the screen is on a terminal.
    bool update(const View& view);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aow
