/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "calling_convention_x86.h"

#include <android-base/logging.h>

#include "arch/instruction_set.h"
#include "arch/x86/jni_frame_x86.h"
#include "utils/x86/managed_register_x86.h"

namespace art HIDDEN {
namespace x86 {

static constexpr ManagedRegister kManagedCoreArgumentRegisters[] = {
    X86ManagedRegister::FromCpuRegister(EAX),
    X86ManagedRegister::FromCpuRegister(ECX),
    X86ManagedRegister::FromCpuRegister(EDX),
    X86ManagedRegister::FromCpuRegister(EBX),
};
static constexpr size_t kManagedCoreArgumentRegistersCount =
    arraysize(kManagedCoreArgumentRegisters);
static constexpr size_t kManagedFpArgumentRegistersCount = 4u;

static constexpr ManagedRegister kCalleeSaveRegisters[] = {
    // Core registers.
    X86ManagedRegister::FromCpuRegister(EBP),
    X86ManagedRegister::FromCpuRegister(ESI),
    X86ManagedRegister::FromCpuRegister(EDI),
    // No hard float callee saves.
};

template <size_t size>
static constexpr uint32_t CalculateCoreCalleeSpillMask(
    const ManagedRegister (&callee_saves)[size]) {
  // The spilled PC gets a special marker.
  uint32_t result = 1 << kNumberOfCpuRegisters;
  for (auto&& r : callee_saves) {
    if (r.AsX86().IsCpuRegister()) {
      result |= (1 << r.AsX86().AsCpuRegister());
    }
  }
  return result;
}

static constexpr uint32_t kCoreCalleeSpillMask = CalculateCoreCalleeSpillMask(kCalleeSaveRegisters);
static constexpr uint32_t kFpCalleeSpillMask = 0u;

static constexpr ManagedRegister kNativeCalleeSaveRegisters[] = {
    // Core registers.
    X86ManagedRegister::FromCpuRegister(EBX),
    X86ManagedRegister::FromCpuRegister(EBP),
    X86ManagedRegister::FromCpuRegister(ESI),
    X86ManagedRegister::FromCpuRegister(EDI),
    // No hard float callee saves.
};

static constexpr uint32_t kNativeCoreCalleeSpillMask =
    CalculateCoreCalleeSpillMask(kNativeCalleeSaveRegisters);
static constexpr uint32_t kNativeFpCalleeSpillMask = 0u;

// Calling convention

ArrayRef<const ManagedRegister> X86JniCallingConvention::CalleeSaveScratchRegisters() const {
  DCHECK(!IsCriticalNative());
  // All managed callee-save registers are available.
  static_assert((kCoreCalleeSpillMask & ~kNativeCoreCalleeSpillMask) == 0u);
  static_assert(kFpCalleeSpillMask == 0u);
  return ArrayRef<const ManagedRegister>(kCalleeSaveRegisters);
}

ArrayRef<const ManagedRegister> X86JniCallingConvention::ArgumentScratchRegisters() const {
  DCHECK(!IsCriticalNative());
  // Exclude return registers (EAX/EDX) even if unused. Using the same scratch registers helps
  // making more JNI stubs identical for better reuse, such as deduplicating them in oat files.
  // Due to the odd ordering of argument registers, use a separate register array.
  static constexpr ManagedRegister kArgumentScratchRegisters[] = {
      X86ManagedRegister::FromCpuRegister(ECX),
      X86ManagedRegister::FromCpuRegister(EBX),
  };
  static_assert(kManagedCoreArgumentRegisters[1].Equals(kArgumentScratchRegisters[0]));
  static_assert(kManagedCoreArgumentRegisters[3].Equals(kArgumentScratchRegisters[1]));
  ArrayRef<const ManagedRegister> scratch_regs(kArgumentScratchRegisters);
  DCHECK(std::none_of(scratch_regs.begin(),
                      scratch_regs.end(),
                      [return_reg = ReturnRegister().AsX86()](ManagedRegister reg) {
                        return return_reg.Overlaps(reg.AsX86());
                      }));
  return scratch_regs;
}

static ManagedRegister ReturnRegisterForShorty(std::string_view shorty, bool jni) {
  if (shorty[0] == 'F' || shorty[0] == 'D') {
    if (jni) {
      return X86ManagedRegister::FromX87Register(ST0);
    } else {
      return X86ManagedRegister::FromXmmRegister(XMM0);
    }
  } else if (shorty[0] == 'J') {
    return X86ManagedRegister::FromRegisterPair(EAX_EDX);
  } else if (shorty[0] == 'V') {
    return ManagedRegister::NoRegister();
  } else {
    return X86ManagedRegister::FromCpuRegister(EAX);
  }
}

ManagedRegister X86ManagedRuntimeCallingConvention::ReturnRegister() const {
  return ReturnRegisterForShorty(GetShorty(), false);
}

ManagedRegister X86JniCallingConvention::ReturnRegister() const {
  return ReturnRegisterForShorty(GetShorty(), true);
}

ManagedRegister X86JniCallingConvention::IntReturnRegister() const {
  return X86ManagedRegister::FromCpuRegister(EAX);
}

// Managed runtime calling convention

ManagedRegister X86ManagedRuntimeCallingConvention::MethodRegister() {
  return X86ManagedRegister::FromCpuRegister(EAX);
}

ManagedRegister X86ManagedRuntimeCallingConvention::ArgumentRegisterForMethodExitHook() {
  return X86ManagedRegister::FromCpuRegister(EBX);
}

void X86ManagedRuntimeCallingConvention::ResetIterator(FrameOffset displacement) {
  ManagedRuntimeCallingConvention::ResetIterator(displacement);
  gpr_arg_count_ = 1u;  // Skip EAX for ArtMethod*
}

void X86ManagedRuntimeCallingConvention::Next() {
  if (!IsCurrentParamAFloatOrDouble()) {
    gpr_arg_count_ += IsCurrentParamALong() ? 2u : 1u;
  }
  ManagedRuntimeCallingConvention::Next();
}

bool X86ManagedRuntimeCallingConvention::IsCurrentParamInRegister() {
  if (IsCurrentParamAFloatOrDouble()) {
    return itr_float_and_doubles_ < kManagedFpArgumentRegistersCount;
  } else {
    // Don't split a long between the last register and the stack.
    size_t extra_regs = IsCurrentParamALong() ? 1u : 0u;
    return gpr_arg_count_ + extra_regs < kManagedCoreArgumentRegistersCount;
  }
}

bool X86ManagedRuntimeCallingConvention::IsCurrentParamOnStack() {
  return !IsCurrentParamInRegister();
}

ManagedRegister X86ManagedRuntimeCallingConvention::CurrentParamRegister() {
  DCHECK(IsCurrentParamInRegister());
  if (IsCurrentParamAFloatOrDouble()) {
    // First four float parameters are passed via XMM0..XMM3
    XmmRegister reg = static_cast<XmmRegister>(XMM0 + itr_float_and_doubles_);
    return X86ManagedRegister::FromXmmRegister(reg);
  } else {
    if (IsCurrentParamALong()) {
      switch (gpr_arg_count_) {
        case 1:
          static_assert(kManagedCoreArgumentRegisters[1].AsX86().AsCpuRegister() == ECX);
          static_assert(kManagedCoreArgumentRegisters[2].AsX86().AsCpuRegister() == EDX);
          return X86ManagedRegister::FromRegisterPair(ECX_EDX);
        case 2:
          static_assert(kManagedCoreArgumentRegisters[2].AsX86().AsCpuRegister() == EDX);
          static_assert(kManagedCoreArgumentRegisters[3].AsX86().AsCpuRegister() == EBX);
          return X86ManagedRegister::FromRegisterPair(EDX_EBX);
        default:
          LOG(FATAL) << "UNREACHABLE";
          UNREACHABLE();
      }
    } else {
      return kManagedCoreArgumentRegisters[gpr_arg_count_];
    }
  }
}

FrameOffset X86ManagedRuntimeCallingConvention::CurrentParamStackOffset() {
  return FrameOffset(displacement_.Int32Value() +   // displacement
                     kFramePointerSize +                 // Method*
                     (itr_slots_ * kFramePointerSize));  // offset into in args
}

// JNI calling convention

X86JniCallingConvention::X86JniCallingConvention(bool is_static,
                                                 bool is_synchronized,
                                                 bool is_fast_native,
                                                 bool is_critical_native,
                                                 std::string_view shorty)
    : JniCallingConvention(is_static,
                           is_synchronized,
                           is_fast_native,
                           is_critical_native,
                           shorty,
                           kX86PointerSize) {
}

uint32_t X86JniCallingConvention::CoreSpillMask() const {
  return is_critical_native_ ? 0u : kCoreCalleeSpillMask;
}

uint32_t X86JniCallingConvention::FpSpillMask() const {
  return is_critical_native_ ? 0u : kFpCalleeSpillMask;
}

size_t X86JniCallingConvention::FrameSize() const {
  if (is_critical_native_) {
    CHECK(!SpillsMethod());
    CHECK(!HasLocalReferenceSegmentState());
    return 0u;  // There is no managed frame for @CriticalNative.
  }

  // Method*, PC return address and callee save area size, local reference segment state
  DCHECK(SpillsMethod());
  const size_t method_ptr_size = static_cast<size_t>(kX86PointerSize);
  const size_t pc_return_addr_size = kFramePointerSize;
  const size_t callee_save_area_size = CalleeSaveRegisters().size() * kFramePointerSize;
  size_t total_size = method_ptr_size + pc_return_addr_size + callee_save_area_size;

  DCHECK(HasLocalReferenceSegmentState());
  // Cookie is saved in one of the spilled registers.

  return RoundUp(total_size, kStackAlignment);
}

size_t X86JniCallingConvention::OutFrameSize() const {
  // The size of outgoing arguments.
  size_t size = GetNativeOutArgsSize(/*num_args=*/ NumberOfExtraArgumentsForJni() + NumArgs(),
                                     NumLongOrDoubleArgs());

  // @CriticalNative can use tail call as all managed callee saves are preserved by AAPCS.
  static_assert((kCoreCalleeSpillMask & ~kNativeCoreCalleeSpillMask) == 0u);
  static_assert((kFpCalleeSpillMask & ~kNativeFpCalleeSpillMask) == 0u);

  if (UNLIKELY(IsCriticalNative())) {
    // Add return address size for @CriticalNative.
    // For normal native the return PC is part of the managed stack frame instead of out args.
    size += kFramePointerSize;
    // For @CriticalNative, we can make a tail call if there are no stack args
    // and the return type is not FP type (needs moving from ST0 to MMX0) and
    // we do not need to extend the result.
    bool return_type_ok = GetShorty()[0] == 'I' || GetShorty()[0] == 'J' || GetShorty()[0] == 'V';
    DCHECK_EQ(
        return_type_ok,
        GetShorty()[0] != 'F' && GetShorty()[0] != 'D' && !RequiresSmallResultTypeExtension());
    if (return_type_ok && size == kFramePointerSize) {
      // Note: This is not aligned to kNativeStackAlignment but that's OK for tail call.
      static_assert(kFramePointerSize < kNativeStackAlignment);
      // The stub frame size is considered 0 in the callee where the return PC is a part of
      // the callee frame but it is kPointerSize in the compiled stub before the tail call.
      DCHECK_EQ(0u, GetCriticalNativeStubFrameSize(GetShorty()));
      return kFramePointerSize;
    }
  }

  size_t out_args_size = RoundUp(size, kNativeStackAlignment);
  if (UNLIKELY(IsCriticalNative())) {
    DCHECK_EQ(out_args_size, GetCriticalNativeStubFrameSize(GetShorty()));
  }
  return out_args_size;
}

ArrayRef<const ManagedRegister> X86JniCallingConvention::CalleeSaveRegisters() const {
  if (UNLIKELY(IsCriticalNative())) {
    // Do not spill anything, whether tail call or not (return PC is already on the stack).
    return ArrayRef<const ManagedRegister>();
  } else {
    return ArrayRef<const ManagedRegister>(kCalleeSaveRegisters);
  }
}

bool X86JniCallingConvention::IsCurrentParamInRegister() {
  return false;  // Everything is passed by stack.
}

bool X86JniCallingConvention::IsCurrentParamOnStack() {
  return true;  // Everything is passed by stack.
}

ManagedRegister X86JniCallingConvention::CurrentParamRegister() {
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

FrameOffset X86JniCallingConvention::CurrentParamStackOffset() {
  return
      FrameOffset(displacement_.Int32Value() - OutFrameSize() + (itr_slots_ * kFramePointerSize));
}

ManagedRegister X86JniCallingConvention::LockingArgumentRegister() const {
  DCHECK(!IsFastNative());
  DCHECK(!IsCriticalNative());
  DCHECK(IsSynchronized());
  // The callee-save register is EBP is suitable as a locking argument.
  static_assert(kCalleeSaveRegisters[0].Equals(X86ManagedRegister::FromCpuRegister(EBP)));
  return X86ManagedRegister::FromCpuRegister(EBP);
}

ManagedRegister X86JniCallingConvention::HiddenArgumentRegister() const {
  CHECK(IsCriticalNative());
  // EAX is neither managed callee-save, nor argument register, nor scratch register.
  DCHECK(std::none_of(kCalleeSaveRegisters,
                      kCalleeSaveRegisters + std::size(kCalleeSaveRegisters),
                      [](ManagedRegister callee_save) constexpr {
                        return callee_save.Equals(X86ManagedRegister::FromCpuRegister(EAX));
                      }));
  return X86ManagedRegister::FromCpuRegister(EAX);
}

bool X86JniCallingConvention::UseTailCall() const {
  CHECK(IsCriticalNative());
  return OutFrameSize() == kFramePointerSize;
}

}  // namespace x86
}  // namespace art
