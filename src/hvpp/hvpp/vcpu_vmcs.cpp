#include "vcpu.h"
#include "lib/assert.h"

namespace hvpp {

//
// control state
//

auto vcpu::pin_based_controls() const noexcept -> msr::vmx_pinbased_ctls
{
  msr::vmx_pinbased_ctls result;
  vmx::vmread(vmx::vmcs::field::ctrl_pin_based_vm_execution_controls, result);
  return result;
}

void vcpu::pin_based_controls(msr::vmx_pinbased_ctls controls) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_pin_based_vm_execution_controls, vmx::adjust(controls));
}

auto vcpu::processor_based_controls() const noexcept -> msr::vmx_procbased_ctls
{
  msr::vmx_procbased_ctls result;
  vmx::vmread(vmx::vmcs::field::ctrl_processor_based_vm_execution_controls, result);
  return result;
}

void vcpu::processor_based_controls(msr::vmx_procbased_ctls controls) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_processor_based_vm_execution_controls, vmx::adjust(controls));
}

auto vcpu::processor_based_controls2() const noexcept -> msr::vmx_procbased_ctls2
{
  msr::vmx_procbased_ctls2 result;
  vmx::vmread(vmx::vmcs::field::ctrl_secondary_processor_based_vm_execution_controls, result);
  return result;
}

void vcpu::processor_based_controls2(msr::vmx_procbased_ctls2 controls) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_secondary_processor_based_vm_execution_controls, controls);
}

auto vcpu::vm_entry_controls() const noexcept -> msr::vmx_entry_ctls
{
  msr::vmx_entry_ctls result;
  vmx::vmread(vmx::vmcs::field::ctrl_vmentry_controls, result);
  return result;
}

void vcpu::vm_entry_controls(msr::vmx_entry_ctls controls) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_vmentry_controls, vmx::adjust(controls));
}

auto vcpu::vm_exit_controls() const noexcept -> msr::vmx_exit_ctls
{
  msr::vmx_exit_ctls result;
  vmx::vmread(vmx::vmcs::field::ctrl_vmexit_controls, result);
  return result;
}

void vcpu::vm_exit_controls(msr::vmx_exit_ctls controls) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_vmexit_controls, vmx::adjust(controls));
}

auto vcpu::exception_bitmap() const noexcept -> vmx::exception_bitmap
{
  vmx::exception_bitmap result;
  vmx::vmread(vmx::vmcs::field::ctrl_exception_bitmap, result);
  return result;
}

void vcpu::exception_bitmap(vmx::exception_bitmap exception_bitmap) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_exception_bitmap, exception_bitmap);
}

auto vcpu::msr_bitmap() const noexcept -> const vmx::msr_bitmap&
{
  return msr_bitmap_;
}

void vcpu::msr_bitmap(const vmx::msr_bitmap& msr_bitmap) noexcept
{
  msr_bitmap_ = msr_bitmap;
  vmx::vmwrite(vmx::vmcs::field::ctrl_msr_bitmap_address, pa_t::from_va(&msr_bitmap_));
}

auto vcpu::io_bitmap() const noexcept -> const vmx::io_bitmap&
{
  return io_bitmap_;
}

void vcpu::io_bitmap(const vmx::io_bitmap& io_bitmap) noexcept
{
  io_bitmap_ = io_bitmap;

  auto procbased_ctls = processor_based_controls();
  procbased_ctls.use_io_bitmaps = true;
  processor_based_controls(procbased_ctls);

  vmx::vmwrite(vmx::vmcs::field::ctrl_io_bitmap_a_address, pa_t::from_va(&io_bitmap_.a));
  vmx::vmwrite(vmx::vmcs::field::ctrl_io_bitmap_b_address, pa_t::from_va(&io_bitmap_.b));
}

auto vcpu::pagefault_error_code_mask() const noexcept -> pagefault_error_code
{
  pagefault_error_code result;
  vmx::vmread(vmx::vmcs::field::ctrl_pagefault_error_code_mask, result);
  return result;
}

void vcpu::pagefault_error_code_mask(pagefault_error_code mask) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_pagefault_error_code_mask, mask);
}

auto vcpu::pagefault_error_code_match() const noexcept -> pagefault_error_code
{
  pagefault_error_code result;
  vmx::vmread(vmx::vmcs::field::ctrl_pagefault_error_code_match, result);
  return result;
}

void vcpu::pagefault_error_code_match(pagefault_error_code match) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_pagefault_error_code_match, match);
}

//
// control entry state
//

void vcpu::inject(interrupt_info interrupt) noexcept
{
  entry_interruption_info(interrupt.info_);

  if (interrupt.valid())
  {
    //
    // These hardware exceptions must provide an error code:
    //  - #DF (8)  - always 0
    //  - #TS (10)
    //  - #NP (11)
    //  - #SS (12)
    //  - #GP (13)
    //  - #PF (14)
    //  - #AC (17) - always 0
    //
    // (ref: Vol3A[6.3.1(External Interrupts)])
    //

    if (interrupt.type() == vmx::interrupt_type::hardware_exception)
    {
      switch (interrupt.vector())
      {
        case exception_vector::invalid_tss:
        case exception_vector::segment_not_present:
        case exception_vector::stack_segment_fault:
        case exception_vector::general_protection:
        case exception_vector::page_fault:
          hvpp_assert(interrupt.error_code_valid());
          entry_interruption_error_code(interrupt.error_code());
          break;

        case exception_vector::double_fault:
        case exception_vector::alignment_check:
          hvpp_assert(interrupt.error_code_valid() && interrupt.error_code().flags == 0);
          entry_interruption_error_code(interrupt.error_code());
          break;

        default:
          break;
      }
    }

    //
    // The instruction pointer that is pushed on the stack depends on the type of event and whether nested
    // exceptions occur during its delivery.The term current guest RIP refers to the value to be loaded from the
    // guest - state area.The value pushed is determined as follows:
    //  - If VM entry successfully injects(with no nested exception) an event with interruption type external
    //    interrupt, NMI, or hardware exception, the current guest RIP is pushed on the stack.
    //
    //  - If VM entry successfully injects(with no nested exception) an event with interruption type software
    //    interrupt, privileged software exception, or software exception, the current guest RIP is incremented by the
    //    VM - entry instruction length before being pushed on the stack.
    //
    //  - If VM entry encounters an exception while injecting an event and that exception does not cause a VM exit,
    //    the current guest RIP is pushed on the stack regardless of event type or VM - entry instruction length.If the
    //    encountered exception does cause a VM exit that saves RIP, the saved RIP is current guest RIP.
    //
    // (ref: Vol3C[26.5.1.1(Details of Vectored-Event Injection)])
    //

    switch (interrupt.type())
    {
      case vmx::interrupt_type::external:
      case vmx::interrupt_type::nmi:
      case vmx::interrupt_type::hardware_exception:
      case vmx::interrupt_type::other_event:
      default:
        break;

      case vmx::interrupt_type::software:
      case vmx::interrupt_type::privileged_exception:
      case vmx::interrupt_type::software_exception:
        if (interrupt.rip_adjust_ == -1)
        {
          interrupt.rip_adjust_ = static_cast<int>(exit_instruction_length());
        }

        if (interrupt.rip_adjust_ > 0)
        {
          entry_instruction_length(interrupt.rip_adjust_);
        }
        break;
    }
  }
}

void vcpu::suppress_rip_adjust() noexcept
{
  suppress_rip_adjust_ = true;
}

auto vcpu::entry_instruction_length() const noexcept -> uint32_t
{
  uint32_t result;
  vmx::vmread(vmx::vmcs::field::ctrl_vmentry_instruction_length, result);
  return result;
}

void vcpu::entry_instruction_length(uint32_t instruction_length) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_vmentry_instruction_length, instruction_length);
}

auto vcpu::entry_interruption_info() const noexcept -> vmx::interrupt_info
{
  vmx::interrupt_info result;
  vmx::vmread(vmx::vmcs::field::ctrl_vmentry_interruption_info, result);
  return result;
}

void vcpu::entry_interruption_info(vmx::interrupt_info info) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_vmentry_interruption_info, info);
}

auto vcpu::entry_interruption_error_code() const noexcept -> exception_error_code
{
  exception_error_code result;
  vmx::vmread(vmx::vmcs::field::ctrl_vmentry_exception_error_code, result);
  return result;
}

void vcpu::entry_interruption_error_code(exception_error_code error_code) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_vmentry_exception_error_code, error_code);
}

//
// exit state
//

auto vcpu::exit_interrupt_info() const noexcept -> interrupt_info
{
  interrupt_info result;
  result.info_ = exit_interruption_info();

  if (result.info_.valid)
  {
    if (result.info_.error_code_valid)
    {
      result.error_code_ = exit_interruption_error_code();
    }

    result.rip_adjust_ = exit_instruction_length();
  }

  return result;
}

auto vcpu::exit_instruction_error() const noexcept -> vmx::instruction_error
{
  vmx::instruction_error result;
  vmx::vmread(vmx::vmcs::field::vmexit_instruction_error, result);
  return result;
}

auto vcpu::exit_instruction_info() const noexcept -> uint32_t
{
  uint32_t result;
  vmx::vmread(vmx::vmcs::field::vmexit_instruction_info, result);
  return result;
}

auto vcpu::exit_instruction_length() const noexcept -> uint32_t
{
  uint32_t result;
  vmx::vmread(vmx::vmcs::field::vmexit_instruction_length, result);
  return result;
}

auto vcpu::exit_interruption_info() const noexcept -> vmx::interrupt_info
{
  vmx::interrupt_info result;
  vmx::vmread(vmx::vmcs::field::vmexit_interruption_info, result);
  return result;
}

auto vcpu::exit_interruption_error_code() const noexcept -> exception_error_code
{
  exception_error_code result;
  vmx::vmread(vmx::vmcs::field::vmexit_interruption_error_code, result);
  return result;
}

auto vcpu::exit_reason() const noexcept -> vmx::exit_reason
{
  vmx::exit_reason result;
  vmx::vmread(vmx::vmcs::field::vmexit_reason, result);
  return result;
}

auto vcpu::exit_qualification() const noexcept -> vmx::exit_qualification
{
  vmx::exit_qualification result;
  vmx::vmread(vmx::vmcs::field::vmexit_qualification, result);
  return result;
}

auto vcpu::exit_guest_physical_address() const noexcept -> pa_t
{
  pa_t result;
  vmx::vmread(vmx::vmcs::field::vmexit_guest_physical_address, result);
  return result;
}

auto vcpu::exit_guest_linear_address() const noexcept -> la_t
{
  la_t result;
  vmx::vmread(vmx::vmcs::field::vmexit_guest_linear_address, result);
  return result;
}

context& vcpu::exit_context() noexcept
{
  return exit_context_;
}

//
// guest state
//

auto vcpu::guest_cr0_shadow() const noexcept -> cr0_t
{
  cr0_t cr0;
  vmx::vmread(vmx::vmcs::field::ctrl_cr0_read_shadow, cr0);
  return cr0;
}

void vcpu::guest_cr0_shadow(cr0_t cr0) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_cr0_read_shadow, cr0);
}

auto vcpu::guest_cr0() const noexcept -> cr0_t
{
  cr0_t cr0;
  vmx::vmread(vmx::vmcs::field::guest_cr0, cr0);
  return cr0;
}

void vcpu::guest_cr0(cr0_t cr0) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_cr0, cr0);
}

auto vcpu::guest_cr3() const noexcept -> cr3_t
{
  cr3_t cr3;
  vmx::vmread(vmx::vmcs::field::guest_cr3, cr3);
  return cr3;
}

void vcpu::guest_cr3(cr3_t cr3) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_cr3, cr3);
}

auto vcpu::guest_cr4_shadow() const noexcept -> cr4_t
{
  cr4_t cr4;
  vmx::vmread(vmx::vmcs::field::ctrl_cr4_read_shadow, cr4);
  return cr4;
}

void vcpu::guest_cr4_shadow(cr4_t cr4) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::ctrl_cr4_read_shadow, cr4);
}

auto vcpu::guest_cr4() const noexcept -> cr4_t
{
  cr4_t cr4;
  vmx::vmread(vmx::vmcs::field::guest_cr4, cr4);
  return cr4;
}

void vcpu::guest_cr4(cr4_t cr4) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_cr4, cr4);
}

auto vcpu::guest_dr7() const noexcept -> dr7_t
{
  dr7_t dr7;
  vmx::vmread(vmx::vmcs::field::guest_dr7, dr7);

  return dr7;
}

void vcpu::guest_dr7(dr7_t dr7) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_dr7, dr7);
}

auto vcpu::guest_debugctl() const noexcept -> msr::debugctl
{
  msr::debugctl debugctl;
  vmx::vmread(vmx::vmcs::field::guest_debugctl, debugctl);
  return debugctl;
}

void vcpu::guest_debugctl(msr::debugctl debugctl) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_debugctl, debugctl);
}

auto vcpu::guest_rsp() const noexcept -> uint64_t
{
  uint64_t rsp;
  vmx::vmread(vmx::vmcs::field::guest_rsp, rsp);
  return rsp;
}

void vcpu::guest_rsp(uint64_t rsp) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_rsp, rsp);
}

auto vcpu::guest_rip() const noexcept -> uint64_t
{
  uint64_t rip;
  vmx::vmread(vmx::vmcs::field::guest_rip, rip);
  return rip;
}

void vcpu::guest_rip(uint64_t rip) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_rip, rip);
}

auto vcpu::guest_rflags() const noexcept -> rflags_t
{
  rflags_t rflags;
  vmx::vmread(vmx::vmcs::field::guest_rflags, rflags);
  return rflags;
}

void vcpu::guest_rflags(rflags_t rflags) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_rflags, rflags);
}

auto vcpu::guest_gdtr() const noexcept -> gdtr_t
{
  gdtr_t gdtr;
  vmx::vmread(vmx::vmcs::field::guest_gdtr_base, gdtr.base_address);
  vmx::vmread(vmx::vmcs::field::guest_gdtr_limit, gdtr.limit);
  return gdtr;
}

void vcpu::guest_gdtr(gdtr_t gdtr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_gdtr_base, gdtr.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_gdtr_limit, gdtr.limit);
}

auto vcpu::guest_idtr() const noexcept -> idtr_t
{
  idtr_t idtr;
  vmx::vmread(vmx::vmcs::field::guest_idtr_base, idtr.base_address);
  vmx::vmread(vmx::vmcs::field::guest_idtr_limit, idtr.limit);
  return idtr;
}

void vcpu::guest_idtr(idtr_t idtr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_idtr_base, idtr.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_idtr_limit, idtr.limit);
}

auto vcpu::guest_cs() const noexcept -> seg_t<cs_t>
{
  seg_t<cs_t> cs;
  vmx::vmread(vmx::vmcs::field::guest_cs_base, cs.base_address);
  vmx::vmread(vmx::vmcs::field::guest_cs_limit, cs.limit);
  vmx::vmread(vmx::vmcs::field::guest_cs_access_rights, cs.access);
  vmx::vmread(vmx::vmcs::field::guest_cs_selector, cs.selector);
  return cs;
}

void vcpu::guest_cs(seg_t<cs_t> cs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_cs_base, cs.base_address /* 0 */);
  vmx::vmwrite(vmx::vmcs::field::guest_cs_limit, cs.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_cs_access_rights, cs.access);
  vmx::vmwrite(vmx::vmcs::field::guest_cs_selector, cs.selector);
}

auto vcpu::guest_ds() const noexcept -> seg_t<ds_t>
{
  seg_t<ds_t> ds;
  vmx::vmread(vmx::vmcs::field::guest_ds_base, ds.base_address);
  vmx::vmread(vmx::vmcs::field::guest_ds_limit, ds.limit);
  vmx::vmread(vmx::vmcs::field::guest_ds_access_rights, ds.access);
  vmx::vmread(vmx::vmcs::field::guest_ds_selector, ds.selector);

  return ds;
}

void vcpu::guest_ds(seg_t<ds_t> ds) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_ds_base, ds.base_address /* 0 */);
  vmx::vmwrite(vmx::vmcs::field::guest_ds_limit, ds.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_ds_access_rights, ds.access);
  vmx::vmwrite(vmx::vmcs::field::guest_ds_selector, ds.selector);
}

auto vcpu::guest_es() const noexcept -> seg_t<es_t>
{
  seg_t<es_t> es;
  vmx::vmread(vmx::vmcs::field::guest_es_base, es.base_address);
  vmx::vmread(vmx::vmcs::field::guest_es_limit, es.limit);
  vmx::vmread(vmx::vmcs::field::guest_es_access_rights, es.access);
  vmx::vmread(vmx::vmcs::field::guest_es_selector, es.selector);

  return es;
}

void vcpu::guest_es(seg_t<es_t> es) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_es_base, es.base_address /* 0 */);
  vmx::vmwrite(vmx::vmcs::field::guest_es_limit, es.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_es_access_rights, es.access);
  vmx::vmwrite(vmx::vmcs::field::guest_es_selector, es.selector);
}

auto vcpu::guest_fs() const noexcept -> seg_t<fs_t>
{
  seg_t<fs_t> fs;
  vmx::vmread(vmx::vmcs::field::guest_fs_base, fs.base_address);
  vmx::vmread(vmx::vmcs::field::guest_fs_limit, fs.limit);
  vmx::vmread(vmx::vmcs::field::guest_fs_access_rights, fs.access);
  vmx::vmread(vmx::vmcs::field::guest_fs_selector, fs.selector);

  return fs;
}

void vcpu::guest_fs(seg_t<fs_t> fs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_fs_base, fs.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_fs_limit, fs.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_fs_access_rights, fs.access);
  vmx::vmwrite(vmx::vmcs::field::guest_fs_selector, fs.selector);
}

auto vcpu::guest_gs() const noexcept -> seg_t<gs_t>
{
  seg_t<gs_t> gs;
  vmx::vmread(vmx::vmcs::field::guest_gs_base, gs.base_address);
  vmx::vmread(vmx::vmcs::field::guest_gs_limit, gs.limit);
  vmx::vmread(vmx::vmcs::field::guest_gs_access_rights, gs.access);
  vmx::vmread(vmx::vmcs::field::guest_gs_selector, gs.selector);

  return gs;
}

void vcpu::guest_gs(seg_t<gs_t> gs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_gs_base, gs.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_gs_limit, gs.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_gs_access_rights, gs.access);
  vmx::vmwrite(vmx::vmcs::field::guest_gs_selector, gs.selector);
}

auto vcpu::guest_ss() const noexcept -> seg_t<ss_t>
{
  seg_t<ss_t> ss;
  vmx::vmread(vmx::vmcs::field::guest_ss_base, ss.base_address);
  vmx::vmread(vmx::vmcs::field::guest_ss_limit, ss.limit);
  vmx::vmread(vmx::vmcs::field::guest_ss_access_rights, ss.access);
  vmx::vmread(vmx::vmcs::field::guest_ss_selector, ss.selector);

  return ss;
}

void vcpu::guest_ss(seg_t<ss_t> ss) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_ss_base, ss.base_address /* 0 */);
  vmx::vmwrite(vmx::vmcs::field::guest_ss_limit, ss.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_ss_access_rights, ss.access);
  vmx::vmwrite(vmx::vmcs::field::guest_ss_selector, ss.selector);
}

auto vcpu::guest_tr() const noexcept -> seg_t<tr_t>
{
  seg_t<tr_t> tr;
  vmx::vmread(vmx::vmcs::field::guest_tr_base, tr.base_address);
  vmx::vmread(vmx::vmcs::field::guest_tr_limit, tr.limit);
  vmx::vmread(vmx::vmcs::field::guest_tr_access_rights, tr.access);
  vmx::vmread(vmx::vmcs::field::guest_tr_selector, tr.selector);

  return tr;
}

void vcpu::guest_tr(seg_t<tr_t> tr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_tr_base, tr.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_tr_limit, tr.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_tr_access_rights, tr.access);
  vmx::vmwrite(vmx::vmcs::field::guest_tr_selector, tr.selector);
}

auto vcpu::guest_ldtr() const noexcept -> seg_t<ldtr_t>
{
  seg_t<ldtr_t> ldtr;
  vmx::vmread(vmx::vmcs::field::guest_ldtr_base, ldtr.base_address);
  vmx::vmread(vmx::vmcs::field::guest_ldtr_limit, ldtr.limit);
  vmx::vmread(vmx::vmcs::field::guest_ldtr_access_rights, ldtr.access);
  vmx::vmread(vmx::vmcs::field::guest_ldtr_selector, ldtr.selector);

  return ldtr;
}

void vcpu::guest_ldtr(seg_t<ldtr_t> ldtr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::guest_ldtr_base, ldtr.base_address);
  vmx::vmwrite(vmx::vmcs::field::guest_ldtr_limit, ldtr.limit);
  vmx::vmwrite(vmx::vmcs::field::guest_ldtr_access_rights, ldtr.access);
  vmx::vmwrite(vmx::vmcs::field::guest_ldtr_selector, ldtr.selector);
}

//
// host state
//

auto vcpu::host_cr0() const noexcept -> cr0_t
{
  cr0_t cr0;
  vmx::vmread(vmx::vmcs::field::host_cr0, cr0);
  return cr0;
}

void vcpu::host_cr0(cr0_t cr0) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_cr0, cr0);
}

auto vcpu::host_cr3() const noexcept -> cr3_t
{
  cr3_t cr3;
  vmx::vmread(vmx::vmcs::field::host_cr3, cr3);
  return cr3;
}

void vcpu::host_cr3(cr3_t cr3) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_cr3, cr3);
}

auto vcpu::host_cr4() const noexcept -> cr4_t
{
  cr4_t cr4;
  vmx::vmread(vmx::vmcs::field::host_cr4, cr4);
  return cr4;
}

void vcpu::host_cr4(cr4_t cr4) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_cr4, cr4);
}

auto vcpu::host_rsp() const noexcept -> uint64_t
{
  uint64_t rsp;
  vmx::vmread(vmx::vmcs::field::host_rsp, rsp);
  return rsp;
}

void vcpu::host_rsp(uint64_t rsp) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_rsp, rsp);
}

auto vcpu::host_rip() const noexcept -> uint64_t
{
  uint64_t rip;
  vmx::vmread(vmx::vmcs::field::host_rip, rip);
  return rip;
}

void vcpu::host_rip(uint64_t rip) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_rip, rip);
}

auto vcpu::host_gdtr() const noexcept -> gdtr_t
{
  gdtr_t gdtr;
  vmx::vmread(vmx::vmcs::field::host_gdtr_base, gdtr.base_address);
  return gdtr;
}

void vcpu::host_gdtr(gdtr_t gdtr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_gdtr_base, gdtr.base_address);
}

auto vcpu::host_idtr() const noexcept -> idtr_t
{
  idtr_t idtr;
  vmx::vmread(vmx::vmcs::field::host_idtr_base, idtr.base_address);
  return idtr;
}

void vcpu::host_idtr(idtr_t idtr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_idtr_base, idtr.base_address);
}

auto vcpu::host_cs() const noexcept -> seg_t<cs_t>
{
  seg_t<cs_t> cs;
  vmx::vmread(vmx::vmcs::field::host_cs_selector, cs.selector);
  return cs;
}

void vcpu::host_cs(seg_t<cs_t> cs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_cs_selector, cs.selector.index * 8);
}

auto vcpu::host_ds() const noexcept -> seg_t<ds_t>
{
  seg_t<ds_t> ds;
  vmx::vmread(vmx::vmcs::field::host_ds_selector, ds.selector);
  return ds;
}

void vcpu::host_ds(seg_t<ds_t> ds) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_ds_selector, ds.selector.index * 8);
}

auto vcpu::host_es() const noexcept -> seg_t<es_t>
{
  seg_t<es_t> es;
  vmx::vmread(vmx::vmcs::field::host_es_selector, es.selector);
  return es;
}

void vcpu::host_es(seg_t<es_t> es) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_es_selector, es.selector.index * 8);
}

auto vcpu::host_fs() const noexcept -> seg_t<fs_t>
{
  seg_t<fs_t> fs;
  vmx::vmread(vmx::vmcs::field::host_fs_selector, fs.selector);
  vmx::vmread(vmx::vmcs::field::host_fs_base, fs.base_address);
  return fs;
}

void vcpu::host_fs(seg_t<fs_t> fs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_fs_selector, fs.selector.index * 8);
  vmx::vmwrite(vmx::vmcs::field::host_fs_base, fs.base_address);
}

auto vcpu::host_gs() const noexcept -> seg_t<gs_t>
{
  seg_t<gs_t> gs;
  vmx::vmread(vmx::vmcs::field::host_gs_selector, gs.selector);
  vmx::vmread(vmx::vmcs::field::host_gs_base, gs.base_address);
  return gs;
}

void vcpu::host_gs(seg_t<gs_t> gs) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_gs_selector, gs.selector.index * 8);
  vmx::vmwrite(vmx::vmcs::field::host_gs_base, gs.base_address);
}

auto vcpu::host_ss() const noexcept -> seg_t<ss_t>
{
  seg_t<ss_t> ss;
  vmx::vmread(vmx::vmcs::field::host_ss_selector, ss.selector);
  return ss;
}

void vcpu::host_ss(seg_t<ss_t> ss) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_ss_selector, ss.selector.index * 8);
}

auto vcpu::host_tr() const noexcept -> seg_t<tr_t>
{
  seg_t<tr_t> tr;
  vmx::vmread(vmx::vmcs::field::host_tr_selector, tr.selector);
  vmx::vmread(vmx::vmcs::field::host_tr_base, tr.base_address);
  return tr;
}

void vcpu::host_tr(seg_t<tr_t> tr) noexcept
{
  vmx::vmwrite(vmx::vmcs::field::host_tr_selector, tr.selector.index * 8);
  vmx::vmwrite(vmx::vmcs::field::host_tr_base, tr.base_address);
}

}