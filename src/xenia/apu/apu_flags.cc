/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/apu_flags.h"

DEFINE_bool(mute, false, "Mutes all audio output.", "APU")
DEFINE_bool(audio_queue_trace, false,
            "Log bounded XAudio2 queue and submission timing aggregates.",
            "APU")
DEFINE_bool(audio_pump_observer, false,
            "Log bounded passive audio worker pump timing windows. Pair with "
            "audio_queue_trace for retained quick-exit captures.",
            "APU")
