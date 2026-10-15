// SDK-DERIVED FORK-ONLY WORK: Eligible for this fork's kinect after validation.
// MUST NOT enter upstream PRs/cherry-pick series, including
// xenia-canary:canary_experimental. CONTRIBUTING.md excludes derived
// information. See SDK_DERIVED_PRIVATE.md. No Xbox SDK include/link dependency
// is used.

#ifndef XENIA_KERNEL_SDK_PRIVATE_FIRST_CHANCE_H_
#define XENIA_KERNEL_SDK_PRIVATE_FIRST_CHANCE_H_

namespace xe::kernel {
class KernelState;

// Install after kernel initialization, detach before teardown. As with normal
// KernelState guest execution, guest threads must be stopped before
// destruction. Both this private search and the CPU scalar offer must be
// explicitly enabled. This is a primary-executable-only experiment under normal
// guest execution lifetime rules: no concurrent host load/setup/patch or
// executable mutation. Copy/revalidation checks do not provide a
// code-generation lease or prevent a concurrent write after a check. There is
// no resume/unwind disposition. A fresh process must start with
// apply_patches=false before setup (empty PatchDB); toggling it off after rules
// have loaded does not meet this contract.
void InstallPrivateFirstChanceDispatcher(KernelState* kernel);
void UninstallPrivateFirstChanceDispatcher(KernelState* kernel);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_SDK_PRIVATE_FIRST_CHANCE_H_
