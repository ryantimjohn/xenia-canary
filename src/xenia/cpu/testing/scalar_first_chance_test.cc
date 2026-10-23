/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64

#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/test_module.h"
#include "xenia/cpu/testing/util.h"

DECLARE_bool(zero_page_scalar_first_chance);
DECLARE_bool(protect_zero);
DECLARE_bool(full_optimization_even_with_debug);
DECLARE_path(log_file);

namespace xe::cpu::testing {
namespace {

constexpr uint32_t kCode = 0x81000000;
constexpr uint32_t kLiR10One = (14u << 26) | (10u << 21) | 1;
constexpr uint32_t kLiR10Two = (14u << 26) | (10u << 21) | 2;
constexpr uint32_t kBlr = 0x4E800020;

uint32_t ScalarWord(bool write, uint32_t ra = 0) {
  return ((write ? 36u : 32u) << 26) | ((write ? 10u : 11u) << 21) | (ra << 16);
}

struct Options {
  bool enabled = cvars::zero_page_scalar_first_chance;
  bool protect = cvars::protect_zero;
  bool optimize = cvars::full_optimization_even_with_debug;
  Options() {
    cvars::zero_page_scalar_first_chance = true;
    cvars::protect_zero = true;
    cvars::full_optimization_even_with_debug = true;
  }
  ~Options() {
    cvars::zero_page_scalar_first_chance = enabled;
    cvars::protect_zero = protect;
    cvars::full_optimization_even_with_debug = optimize;
  }
};

class ScalarModule final : public Module {
 public:
  explicit ScalarModule(Processor* processor) : Module(processor) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  bool ContainsAddress(uint32_t address) override {
    return address >= kCode && address < kCode + 16;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(
        processor_->backend()->CreateGuestFunction(this, address));
  }

 private:
  const std::string name_ = "synthetic-scalar";
};

struct Fixture {
  std::unique_ptr<Memory> memory = std::make_unique<Memory>();
  std::unique_ptr<Processor> processor;
  std::unique_ptr<ThreadState> thread;
  ThreadState* previous = ThreadState::Get();
  uint32_t stack = 0;

  Fixture() = default;
  explicit Fixture(uint32_t word) { REQUIRE(Initialize(word)); }

  // The death-test child must not enter Catch, even during setup.
  bool Initialize(uint32_t word) {
    if (!memory->Initialize()) {
      return false;
    }
    auto* heap = memory->LookupHeap(kCode);
    if (!heap ||
        !heap->AllocFixed(kCode, 0x10000, 0x10000,
                          kMemoryAllocationReserve | kMemoryAllocationCommit,
                          kMemoryProtectRead | kMemoryProtectWrite)) {
      return false;
    }
    const std::array<uint32_t, 4> code = {kLiR10One, word, kLiR10Two, kBlr};
    for (size_t index = 0; index < code.size(); ++index) {
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(kCode + static_cast<uint32_t>(index) * 4),
          code[index]);
    }
    processor = std::make_unique<Processor>(memory.get(), nullptr);
    if (!processor->Setup(std::make_unique<backend::x64::X64Backend>()) ||
        !processor->AddModule(
            std::make_unique<ScalarModule>(processor.get()))) {
      return false;
    }
    processor->backend()->CommitExecutableRange(kCode, kCode + 0x10000);
    stack = memory->SystemHeapAlloc(0x10000);
    if (!stack) {
      return false;
    }
    thread =
        std::make_unique<ThreadState>(processor.get(), 0x100, stack + 0x10000);
    ThreadState::Bind(thread.get());
    thread->context()->lr = 0xBCBCBCBC;
    // RA=0 must not use the nonzero value in guest r0.
    thread->context()->r[0] = 0x123456789ABCDEF0;
    thread->context()->r[10] = 0xCCCC;
    thread->context()->r[11] = 0xDDDD;
    return true;
  }

  ~Fixture() {
    ThreadState::Bind(previous);
    thread.reset();
    if (stack) {
      memory->SystemHeapFree(stack);
    }
  }

  GuestFunction* Resolve() {
    auto* function = processor->ResolveFunction(kCode);
    REQUIRE(function);
    REQUIRE(function->is_guest());
    return static_cast<GuestFunction*>(function);
  }
};

// Ordinary pre-access evidence only. There is deliberately no native exception
// observer, CONTEXT inspection, SEH recovery, or assertion in the child.
// The record plus AV exit does not prove the native fault RIP/address/state,
// tail suppression, or correctness of native unwinding through JIT thunks.
struct CallbackRecord {
  uint64_t version;
  uint64_t nonce;
  uint64_t write;
  uint64_t callbacks;
  uint64_t same_thread;
  uint64_t guest_pc;
  uint64_t instruction;
  uint64_t address;
  uint64_t reported_write;
  uint64_t r10;
  uint64_t r0;
  uint64_t complete;
};
static_assert(std::is_trivially_copyable_v<CallbackRecord>);
static_assert(sizeof(CallbackRecord) == 12 * sizeof(uint64_t));
constexpr uint64_t kRecordComplete = 0x5343414C415231;
constexpr int kChildFailure = 90;
constexpr wchar_t kRecordName[] = L"scalar-fault-record.bin";

class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() {
    if (value_ && value_ != INVALID_HANDLE_VALUE) {
      CloseHandle(value_);
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }

 private:
  HANDLE value_;
};

bool WriteCallbackRecord(const CallbackRecord& record) {
  Handle file(CreateFileW(kRecordName, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  return WriteFile(file.get(), &record, sizeof(record), &written, nullptr) &&
         written == sizeof(record) && FlushFileBuffers(file.get());
}

bool ValidCallbackRecord(const CallbackRecord& record, uint64_t nonce,
                         bool write) {
  return record.version == 1 && record.nonce == nonce &&
         record.write == uint64_t(write) && record.callbacks == 1 &&
         record.same_thread == 1 && record.guest_pc == kCode + 4 &&
         record.instruction == ScalarWord(write) && record.address == 0 &&
         record.reported_write == uint64_t(write) && record.r10 == 1 &&
         record.r0 == 0x123456789ABCDEF0 && record.complete == kRecordComplete;
}

int ScalarFaultChild(bool write, uint64_t nonce) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  Options options;
  Fixture fixture;
  if (!fixture.Initialize(ScalarWord(write))) {
    return kChildFailure;
  }
  auto* resolved = fixture.processor->ResolveFunction(kCode);
  if (!resolved || !resolved->is_guest()) {
    return kChildFailure;
  }
  uint64_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState* thread,
          const Processor::ScalarAccessFirstChance& fault) {
        CallbackRecord record{1,
                              nonce,
                              uint64_t(write),
                              ++callbacks,
                              uint64_t(thread == fixture.thread.get()),
                              fault.guest_pc,
                              fault.raw_instruction,
                              fault.guest_address,
                              uint64_t(fault.is_write),
                              fault.modeled_context->r[10],
                              fault.modeled_context->r[0],
                              kRecordComplete};
        // A repeated callback or a failed/partial record must never leave a
        // record that can qualify an eventual unrelated process failure.
        if (callbacks != 1 || !WriteCallbackRecord(record)) {
          ExitProcess(kChildFailure);
        }
      });
  resolved->Call(fixture.thread.get(), 0xBCBCBCBC);
  // A return is not success, even if the pre-access record was written.
  Handle returned(CreateFileW(L"normal-return", GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  return kChildFailure;
}

struct ChildResult {
  bool setup = false;
  bool timed_out = false;
  bool exited = false;
  bool job_empty = false;
  DWORD exit_code = STILL_ACTIVE;
  bool exact_record = false;
  bool normal_return = false;
  CallbackRecord record{};
};

// No inherited handles, shell, detached child, or process-name cleanup. The
// retained process handle and nested kill-on-close Job own this single child.
ChildResult RunChild(bool write, uint64_t nonce,
                     const std::filesystem::path& directory) {
  ChildResult result;
  wchar_t executable[32768]{};
  const DWORD length =
      GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
  if (!length || length >= std::size(executable)) {
    return result;
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.get() ||
      !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return result;
  }
  std::wstring command =
      L"\"" + std::wstring(executable) + L"\" --xe-scalar-fault-child=" +
      (write ? L"write:" : L"read:") + std::to_wstring(nonce);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                      directory.c_str(), &startup, &process)) {
    return result;
  }
  Handle process_handle(process.hProcess);
  Handle thread_handle(process.hThread);
  if (!AssignProcessToJobObject(job.get(), process.hProcess)) {
    TerminateProcess(process.hProcess, kChildFailure);
    WaitForSingleObject(process.hProcess, 5000);
    return result;
  }
  if (ResumeThread(process.hThread) == DWORD(-1)) {
    TerminateJobObject(job.get(), kChildFailure);
    WaitForSingleObject(process.hProcess, 5000);
    return result;
  }
  result.setup = true;
  const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
  result.exited = wait == WAIT_OBJECT_0;
  result.timed_out = wait == WAIT_TIMEOUT;
  if (!result.exited) {
    TerminateJobObject(job.get(), kChildFailure);
    WaitForSingleObject(process.hProcess, 5000);
    return result;
  }
  GetExitCodeProcess(process.hProcess, &result.exit_code);
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
  result.job_empty =
      QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                                &accounting, sizeof(accounting), nullptr) &&
      accounting.ActiveProcesses == 0;
  Handle file(CreateFileW((directory / kRecordName).c_str(), GENERIC_READ,
                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  LARGE_INTEGER size{};
  DWORD read = 0;
  result.exact_record = file.get() != INVALID_HANDLE_VALUE &&
                        GetFileSizeEx(file.get(), &size) &&
                        size.QuadPart == sizeof(result.record) &&
                        ReadFile(file.get(), &result.record,
                                 sizeof(result.record), &read, nullptr) &&
                        read == sizeof(result.record);
  result.normal_return =
      GetFileAttributesW((directory / L"normal-return").c_str()) !=
      INVALID_FILE_ATTRIBUTES;
  return result;
}

bool ValidChildResult(const ChildResult& result, uint64_t nonce, bool write) {
  return result.setup && !result.timed_out && result.exited &&
         result.job_empty && result.exit_code == EXCEPTION_ACCESS_VIOLATION &&
         result.exact_record && !result.normal_return &&
         ValidCallbackRecord(result.record, nonce, write);
}

TEST_CASE("Scalar pre-access callback precedes child access-violation death",
          "[cpu][scalar-first-chance]") {
  const bool write = GENERATE(false, true);
  // A fixed per-parent prefix and strictly increasing low word cannot cancel
  // one another as independently sampled clock/sequence XOR operands could.
  static std::atomic<uint32_t> sequence{0};
  const uint32_t serial = ++sequence;
  REQUIRE(serial != 0);
  const uint64_t nonce = (uint64_t(GetCurrentProcessId()) << 32) | serial;
  const auto directory_name = L"xenia-scalar-" +
                              std::to_wstring(GetTickCount64()) + L"-" +
                              std::to_wstring(nonce);
  const auto directory =
      std::filesystem::temp_directory_path() / directory_name;
  REQUIRE(CreateDirectoryW(directory.c_str(), nullptr));
  const ChildResult result = RunChild(write, nonce, directory);
  // Retain success and failure evidence; never recursively remove a temp path.
  INFO("Child evidence directory: " << directory.string());
  INFO("Child setup=" << result.setup << " exited=" << result.exited
                      << " timeout=" << result.timed_out << " exit code="
                      << result.exit_code << " Job empty=" << result.job_empty
                      << " exact record=" << result.exact_record);
  REQUIRE(ValidChildResult(result, nonce, write));
}

TEST_CASE("Scalar child evidence rejects incomplete and nonfatal outcomes",
          "[cpu][scalar-first-chance]") {
  ChildResult result;
  result.setup = result.exited = result.job_empty = result.exact_record = true;
  result.exit_code = EXCEPTION_ACCESS_VIOLATION;
  result.record = {1,
                   17,
                   0,
                   1,
                   1,
                   kCode + 4,
                   ScalarWord(false),
                   0,
                   0,
                   1,
                   0x123456789ABCDEF0,
                   kRecordComplete};
  REQUIRE(ValidChildResult(result, 17, false));
  auto bad = result;
  bad.record.version = 2;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.complete = 0;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.callbacks = 2;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.instruction = ScalarWord(true);
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.nonce = 18;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.write = 1;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.guest_pc += 4;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.address = 4;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.reported_write = 1;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.same_thread = 0;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.r10 = 2;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.record.r0 = 0;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.exit_code = 0;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.exit_code = kChildFailure;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.normal_return = true;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.exact_record = false;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.timed_out = true;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.setup = false;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.job_empty = false;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
  bad = result;
  bad.exited = false;
  CHECK_FALSE(ValidChildResult(bad, 17, false));
}

TEST_CASE("Scalar direct site restores context and nested offers on return",
          "[cpu][scalar-first-chance]") {
  Options options;
  Fixture fixture(ScalarWord(true));
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      fixture.Resolve(), kCode + 4, ScalarWord(true));
  REQUIRE(site);
  fixture.thread->context()->r[10] = 1;
  ppc::PPCContext saved;
  std::memcpy(&saved, fixture.thread->context(), sizeof(saved));
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState* thread,
          const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(thread == fixture.thread.get());
        CHECK(fault.modeled_context->r[10] == 1);
        REQUIRE(site->Call(thread, 0xBCBCBCBC));
        CHECK(callbacks == 1);
        {
          ThreadState nested(fixture.processor.get(), 0x101);
          REQUIRE(site->Call(&nested, 0xBCBCBCBC));
          CHECK(callbacks == 1);
          CHECK(ThreadState::Get() == thread);
        }
        fixture.processor->set_scalar_access_first_chance_handler({});
        thread->context()->r[10] = 0xBAD;
        thread->context()->r[0] = 0;
        thread->context()->ctr = 0xBAD;
        thread->context()->f[7] = -1.0;
      });
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 1);
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance&) {
        ++callbacks;
      });
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 2);
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
}

TEST_CASE(
    "Scalar first chance leaves ordinary nonzero native accesses unchanged",
    "[cpu][scalar-first-chance]") {
  const bool write = GENERATE(false, true);
  Options options;
  Fixture fixture(ScalarWord(write, 4));
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance&) {
        ++callbacks;
      });
  const uint32_t data = fixture.memory->SystemHeapAlloc(4, 4);
  REQUIRE(data);
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(data),
                               0x12345678);
  fixture.thread->context()->r[4] = 0xDEAD000000000000ULL | data;
  REQUIRE(fixture.Resolve()->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
  CHECK(fixture.thread->context()->r[10] == 2);
  if (write) {
    CHECK(xe::load_and_swap<uint32_t>(fixture.memory->TranslateVirtual(data)) ==
          1);
  } else {
    CHECK(fixture.thread->context()->r[11] == 0x12345678);
  }
  fixture.memory->SystemHeapFree(data);
}

TEST_CASE("Scalar first chance rejects stale instruction and module identities",
          "[cpu][scalar-first-chance]") {
  Options options;
  Fixture fixture(ScalarWord(true));
  auto* function = fixture.Resolve();
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true));
  REQUIRE(site);
  cvars::zero_page_scalar_first_chance = false;
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true)));
  cvars::zero_page_scalar_first_chance = true;
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 5, ScalarWord(true)));
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, 37u << 26));  // Updating STWU is unsupported.
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true, 4)));  // Dynamic RA unsupported.
  CHECK_FALSE(fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true) | 0xFFFF));  // Negative literal.
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance&) {
        ++callbacks;
      });
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(kCode + 4),
                               kBlr);
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
  xe::store_and_swap<uint32_t>(fixture.memory->TranslateVirtual(kCode + 4),
                               ScalarWord(true));
  fixture.processor->RemoveModule("synthetic-scalar");
  REQUIRE(fixture.processor->AddModule(
      std::make_unique<ScalarModule>(fixture.processor.get())));
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 0);
}

void ChangeOperand(ppc::PPCContext* context, void* count, void*) {
  ++*static_cast<size_t*>(count);
  CHECK(context->r[10] == 1);
  context->r[10] = 7;
}

TEST_CASE(
    "Scalar first chance restores context and nesting after host exception",
    "[cpu][scalar-first-chance]") {
  Options options;
  Fixture fixture(ScalarWord(true));
  auto* function = fixture.Resolve();
  auto* site = fixture.processor->RegisterScalarAccessFirstChance(
      function, kCode + 4, ScalarWord(true));
  REQUIRE(site);
  fixture.thread->context()->r[10] = 1;
  ppc::PPCContext saved;
  std::memcpy(&saved, fixture.thread->context(), sizeof(saved));
  struct CallbackFailure {};
  size_t callbacks = 0;
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState* thread,
          const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(fault.modeled_context->r[10] == 1);
        thread->context()->r[10] = 0xBAD;
        thread->context()->f[7] = -1.0;
        throw CallbackFailure{};
      });
  // Direct builtin invocation tests host-exception cleanup, without making a
  // claim that the guest instruction ran or requiring unwinding through JIT.
  REQUIRE_THROWS_AS(site->Call(fixture.thread.get(), 0xBCBCBCBC),
                    CallbackFailure);
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
  fixture.processor->set_scalar_access_first_chance_handler(
      [&](ThreadState*, const Processor::ScalarAccessFirstChance& fault) {
        ++callbacks;
        CHECK(fault.modeled_context->r[10] == 1);
      });
  REQUIRE(site->Call(fixture.thread.get(), 0xBCBCBCBC));
  CHECK(callbacks == 2);  // The first callback's TLS nesting guard was reset.
  CHECK(std::memcmp(fixture.thread->context(), &saved, sizeof(saved)) == 0);
}

TEST_CASE("Scalar boundary control reloads native operands after CALL_EXTERN",
          "[cpu][scalar-first-chance]") {
  Options options;
  size_t calls = 0;
  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  REQUIRE(processor->Setup(std::make_unique<backend::x64::X64Backend>()));
  auto* callback = processor->DefineBuiltin("ChangeScalarOperand",
                                            ChangeOperand, &calls, nullptr);
  auto module = std::make_unique<TestModule>(
      processor.get(), "scalar-boundary-control",
      [](uint32_t address) { return address == kCode; },
      [callback](hir::HIRBuilder& builder) {
        StoreGPR(builder, 10, builder.LoadConstantUint64(1));
        // Give context promotion a cached value before the call.
        StoreGPR(builder, 12, LoadGPR(builder, 10));
        builder.CallExtern(callback);
        builder.Store(LoadGPR(builder, 4),
                      builder.Truncate(LoadGPR(builder, 10), hir::INT32_TYPE));
        StoreGPR(builder, 10, builder.LoadConstantUint64(2));
        builder.Return();
        return true;
      });
  REQUIRE(processor->AddModule(std::move(module)));
  processor->backend()->CommitExecutableRange(kCode, kCode + 0x10000);
  auto* function = processor->ResolveFunction(kCode);
  REQUIRE(function);
  const uint32_t allocation = memory->SystemHeapAlloc(0x10000);
  REQUIRE(allocation);
  auto thread = std::make_unique<ThreadState>(processor.get(), 0x100,
                                              allocation + 0x10000);
  thread->context()->lr = 0xBCBCBCBC;
  thread->context()->r[4] = allocation;
  REQUIRE(function->Call(thread.get(), 0xBCBCBCBC));
  CHECK(calls == 1);
  uint32_t stored = 0;
  std::memcpy(&stored, memory->TranslateVirtual(allocation), sizeof(stored));
  CHECK(stored == 7);  // Real native STORE consumes the post-call operand.
  CHECK(thread->context()->r[10] == 2);
  CHECK(thread->context()->r[12] == 1);
  thread.reset();
  memory->SystemHeapFree(allocation);
}

}  // namespace

namespace {
constexpr char kChildSwitch[] = "--xe-scalar-fault-child=";

bool HasChildSwitch(const std::vector<std::string>& args) {
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].starts_with(kChildSwitch)) {
      return true;
    }
  }
  return false;
}
}  // namespace

// Called only by the CPU executable before ordinary logging initialization.
// Transparent console arguments otherwise do not select a separate log file.
void PrepareScalarFaultChild(const std::vector<std::string>& args) {
  if (HasChildSwitch(args)) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    cvars::log_file = std::filesystem::current_path() / L"child.log";
  }
}

bool RunScalarFaultChild(const std::vector<std::string>& args, int* result) {
  if (!HasChildSwitch(args)) {
    return false;
  }
  *result = kChildFailure;
  if (args.size() != 2 || !args[1].starts_with(kChildSwitch)) {
    return true;
  }
  auto value = std::string_view(args[1]).substr(sizeof(kChildSwitch) - 1);
  bool write = false;
  if (value.starts_with("write:")) {
    write = true;
    value.remove_prefix(6);
  } else if (value.starts_with("read:")) {
    value.remove_prefix(5);
  } else {
    return true;
  }
  uint64_t nonce = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), nonce);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
      !nonce) {
    return true;
  }
  try {
    *result = ScalarFaultChild(write, nonce);
  } catch (...) {
    // Ordinary setup errors cannot qualify as the requested AV termination.
    *result = kChildFailure;
  }
  return true;
}

}  // namespace xe::cpu::testing

#endif  // XE_PLATFORM_WIN32 && XE_ARCH_AMD64
