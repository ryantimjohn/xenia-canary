# NUI tracking metadata observer

`--nui_observe_tracking=true` enables a private, process-lifetime diagnostic.
It is off by default. It observes the existing depth feed and the title's
`0x2B002` HUD skeleton message. It does not alter tracking, calibration,
guest memory, callback results, or the Guide's engagement decision.

The camera feed emits at most 360 roughly one-second aggregate rows and one
snapshot of the cumulative total through the **first** feed end: at most 361
observer log lines per process, including repeated feed sessions. Counters
continue accumulating after that snapshot; later sessions have bounded rows
but no second end snapshot. Each interval row includes a one-based row index,
steady-clock start and end microseconds, and actual elapsed milliseconds, so
a delayed flush can be interpreted as a longer interval. The first-end
snapshot uses row index zero and names its covered duration. Matching history
resets when a feed session starts. Snapshots are copied under the observer's
own mutex and logged only after guest callbacks return. The observer stores a
fixed cache of 16 guest
depth timestamps for matching a HUD message. A match means the HUD timestamp
equals a depth stamp that xenia copied into a camera result; it does **not**
prove that the game consumed that source frame. Equal stamps from multiple
deliveries are counted as ambiguous. A missing or future host capture stamp
gives an unknown host age. HUD lag is measured from the result copy to the HUD
message on the host steady clock only for a unique stamp match. Host publish
stamps, depth-copy times, and HUD observation times all use
`std::chrono::steady_clock` microseconds from the same process epoch.

`source` counts acquired, new host frames from live Kinect or replay, and
`seq` gives the first and last published sequence in the interval. `skipped`
counts gaps in those sequence numbers; the feed uses the latest frame, so a
sequence gap need not indicate a lost capture. `seq_resets` counts a
nonincreasing sequence within a feed session. `no_buffer` means no guest
depth request was waiting. `invalid_buffer` means a request
could not receive the frame. `copied_stamped` means pixels and the result
timestamp were written. `completion_called` counts the subsequent callback
invocation, and `callback_returned` confirms it returned. These last two
counters describe successful copied-depth requests only; failed-request
callbacks are included in `invalid_buffer` but have no separate callback
stage count. These are delivery stages, not guest consumption.

`slots` groups the six HUD tracking states as 0, 1, 2, and invalid. `joints`
groups each of the 20 per-joint states for tracked slots the same way.
`ids` counts per-slot appearances, disappearances, and replacements without
printing the IDs. These slot counts alone are **not** a new-body or identity
loss rate. `tracked_set_changed` counts changes to the unique set of IDs in
tracked slots; `same_set_slot_reassigned` counts slot movement while that set
stays the same. `guide` counts frames where the local Guide decision did not
take a skeleton and, when it did, whether its selected ID matches exactly one
tracked slot, none, or multiple slots. The HUD message's engagement fields
are output fields and are not read by this observer. `quality_nonzero` counts
tracked slots with any quality flag set. `nonfinite` and `zero` count tracked
joint vectors with a nonfinite spatial component or all three spatial components
zero.
No coordinates, IDs, image buffers, guest pointers, profile data, or replay
paths are stored in the aggregate or printed by this observer. The separate
`nui_log_messages` and `nui_log_play_frames` diagnostics have their own output
and should remain off when only aggregate metadata is wanted.

The SDK backend currently publishes depth images; the guest runtime and title
construct the skeletons that appear in the HUD message. This observer cannot
identify why a particular body was or was not tracked from depth alone. Replay
publishes frames on a new clock and may omit live tilt and acceleration history,
so replay results are diagnostic comparisons rather than an exact live rerun.
