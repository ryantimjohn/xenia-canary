/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/nui/nui_flags.h"

DEFINE_bool(nui_allow_tilt, true, "Allow titles to move the Kinect tilt motor.",
            "Kinect");

DEFINE_bool(nui_log_messages, false,
            "Log every NUI (Kinect) xam message and XamNui* call with a hex "
            "dump of its buffer.",
            "Kinect");

DEFINE_int32(nui_tilt_override, 1000,
             "If between -27 and 27, report this tilt angle to titles instead "
             "of the sensor's.",
             "Kinect");

DEFINE_bool(nui_show_depth_preview, false,
            "Show an overlay with the depth frames fed to the guest.",
            "Kinect");
