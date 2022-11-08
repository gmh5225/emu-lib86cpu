/*
 * x86 translation code
 *
 * ergo720                Copyright (c) 2019
 */

#include "internal.h"
#include "frontend.h"
#include "memory.h"
#include "main_wnd.h"
#include "debugger.h"
#include "helpers.h"

#ifdef LIB86CPU_X64_EMITTER
#include "x64/jit.h"
#endif

#define BAD LIB86CPU_ABORT_msg("Encountered unimplemented instruction %s", log_instr(disas_ctx->virt_pc - cpu->instr_bytes, &instr).c_str())


static void
check_dbl_exp(cpu_ctx_t *cpu_ctx)
{
	uint16_t idx = cpu_ctx->exp_info.exp_data.idx;
	bool old_contributory = cpu_ctx->exp_info.old_exp == 0 || (cpu_ctx->exp_info.old_exp >= 10 && cpu_ctx->exp_info.old_exp <= 13);
	bool curr_contributory = idx == 0 || (idx >= 10 && idx <= 13);

	LOG(log_level::info, "%s old: %u new %u", __func__, cpu_ctx->exp_info.old_exp, idx);

	if (cpu_ctx->exp_info.old_exp == EXP_DF) {
		throw lc86_exp_abort("The guest has triple faulted, cannot continue", lc86_status::success);
	}

	if ((old_contributory && curr_contributory) || (cpu_ctx->exp_info.old_exp == EXP_PF && (curr_contributory || (idx == EXP_PF)))) {
		cpu_ctx->exp_info.exp_data.code = 0;
		cpu_ctx->exp_info.exp_data.eip = 0;
		idx = EXP_DF;
	}

	if (curr_contributory || (idx == EXP_PF) || (idx == EXP_DF)) {
		cpu_ctx->exp_info.old_exp = idx;
	}

	cpu_ctx->exp_info.exp_data.idx = idx;
}

template<bool is_int>
translated_code_t *cpu_raise_exception(cpu_ctx_t *cpu_ctx)
{
	check_dbl_exp(cpu_ctx);

	cpu_t *cpu = cpu_ctx->cpu;
	uint32_t fault_addr = cpu_ctx->exp_info.exp_data.fault_addr;
	uint16_t code = cpu_ctx->exp_info.exp_data.code;
	uint16_t idx = cpu_ctx->exp_info.exp_data.idx;
	uint32_t eip = cpu_ctx->exp_info.exp_data.eip;
	uint32_t old_eflags = read_eflags(cpu);

	if (cpu_ctx->hflags & HFLG_PE_MODE) {
		// protected mode

		if (idx * 8 + 7 > cpu_ctx->regs.idtr_hidden.limit) {
			cpu_ctx->exp_info.exp_data.code = idx * 8 + 2;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		uint64_t desc = mem_read<uint64_t>(cpu, cpu_ctx->regs.idtr_hidden.base + idx * 8, eip, 2);
		uint16_t type = (desc >> 40) & 0x1F;
		uint32_t new_eip, eflags;
		switch (type)
		{
		case 5:  // task gate
			// we don't support task gates yet, so just abort
			LIB86CPU_ABORT_msg("Task gates are not supported yet while delivering an exception");

		case 6:  // interrupt gate, 16 bit
		case 14: // interrupt gate, 32 bit
			eflags = cpu_ctx->regs.eflags & ~IF_MASK;
			new_eip = ((desc & 0xFFFF000000000000) >> 32) | (desc & 0xFFFF);
			break;

		case 7:  // trap gate, 16 bit
		case 15: // trap gate, 32 bit
			eflags = cpu_ctx->regs.eflags;
			new_eip = ((desc & 0xFFFF000000000000) >> 32) | (desc & 0xFFFF);
			break;

		default:
			cpu_ctx->exp_info.exp_data.code = idx * 8 + 2;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		uint32_t dpl = (desc & SEG_DESC_DPL) >> 45;
		uint32_t cpl = cpu_ctx->hflags & HFLG_CPL;
		if (is_int && (dpl < cpl)) {
			cpu_ctx->exp_info.exp_data.code = idx * 8 + 2;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		if ((desc & SEG_DESC_P) == 0) {
			cpu_ctx->exp_info.exp_data.code = idx * 8 + 2;
			cpu_ctx->exp_info.exp_data.idx = EXP_NP;
			return cpu_raise_exception(cpu_ctx);
		}

		uint16_t sel = (desc & 0xFFFF0000) >> 16;
		if ((sel >> 2) == 0) {
			cpu_ctx->exp_info.exp_data.code = 0;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		addr_t code_desc_addr;
		uint64_t code_desc;
		if (read_seg_desc_helper(cpu, sel, code_desc_addr, code_desc, eip)) {
			return cpu_raise_exception(cpu_ctx);
		}

		dpl = (code_desc & SEG_DESC_DPL) >> 45;
		if (dpl > cpl) {
			cpu_ctx->exp_info.exp_data.code = sel & 0xFFFC;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		if ((code_desc & SEG_DESC_P) == 0) {
			cpu_ctx->exp_info.exp_data.code = sel & 0xFFFC;
			cpu_ctx->exp_info.exp_data.idx = EXP_NP;
			return cpu_raise_exception(cpu_ctx);
		}

		if (code_desc & SEG_DESC_C) {
			dpl = cpl;
		}

		set_access_flg_seg_desc_helper(cpu, code_desc, code_desc_addr, eip);

		uint32_t seg_base = read_seg_desc_base_helper(cpu, code_desc);
		uint32_t seg_limit = read_seg_desc_limit_helper(cpu, code_desc);
		uint32_t seg_flags = read_seg_desc_flags_helper(cpu, code_desc);
		uint32_t stack_switch, stack_mask, stack_base, esp;
		uint32_t new_esp;
		uint16_t new_ss;
		addr_t ss_desc_addr;
		uint64_t ss_desc;

		if (dpl < cpl) {
			// more privileged

			if (read_stack_ptr_from_tss_helper(cpu, dpl, new_esp, new_ss, eip)) {
				return cpu_raise_exception(cpu_ctx);
			}

			if ((new_ss >> 2) == 0) {
				cpu_ctx->exp_info.exp_data.code = new_ss & 0xFFFC;
				cpu_ctx->exp_info.exp_data.idx = EXP_TS;
				return cpu_raise_exception(cpu_ctx);
			}

			if (read_seg_desc_helper(cpu, new_ss, ss_desc_addr, ss_desc, eip)) {
				// code already written by read_seg_desc_helper
				cpu_ctx->exp_info.exp_data.idx = EXP_TS;
				return cpu_raise_exception(cpu_ctx);
			}

			uint32_t p = (ss_desc & SEG_DESC_P) >> 40;
			uint32_t s = (ss_desc & SEG_DESC_S) >> 44;
			uint32_t d = (ss_desc & SEG_DESC_DC) >> 42;
			uint32_t w = (ss_desc & SEG_DESC_W) >> 39;
			uint32_t ss_dpl = (ss_desc & SEG_DESC_DPL) >> 42;
			uint32_t ss_rpl = (new_ss & 3) << 5;
			if ((s | d | w | ss_dpl | ss_rpl | p) ^ ((0x85 | (dpl << 3)) | (dpl << 5))) {
				cpu_ctx->exp_info.exp_data.code = new_ss & 0xFFFC;
				cpu_ctx->exp_info.exp_data.idx = EXP_TS;
				return cpu_raise_exception(cpu_ctx);
			}

			set_access_flg_seg_desc_helper(cpu, ss_desc, ss_desc_addr, eip);

			stack_switch = 1;
			stack_mask = ss_desc & SEG_DESC_DB ? 0xFFFFFFFF : 0xFFFF;
			stack_base = read_seg_desc_base_helper(cpu, ss_desc);
			esp = new_esp;
		}
		else { // same privilege
			stack_switch = 0;
			stack_mask = cpu_ctx->hflags & HFLG_SS32 ? 0xFFFFFFFF : 0xFFFF;
			stack_base = cpu_ctx->regs.ss_hidden.base;
			esp = cpu_ctx->regs.esp;
		}

		uint8_t has_code;
		switch (idx)
		{
		case EXP_DF:
		case EXP_TS:
		case EXP_NP:
		case EXP_SS:
		case EXP_GP:
		case EXP_PF:
		case EXP_AC:
			has_code = 1;
			break;

		default:
			has_code = 0;
		}

		type >>= 3;
		if (stack_switch) {
			if (type) { // push 32, priv
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.ss, eip, 2);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.esp, eip, 2);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), old_eflags, eip, 2);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.cs, eip, 2);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), eip, eip, 2);
				if (has_code) {
					esp -= 4;
					mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), code, eip, 2);
				}
			}
			else { // push 16, priv
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.ss, eip, 2);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.esp, eip, 2);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), old_eflags, eip, 2);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.cs, eip, 2);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), eip, eip, 2);
				if (has_code) {
					esp -= 2;
					mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), code, eip, 2);
				}
			}

			uint32_t ss_flags = read_seg_desc_flags_helper(cpu, ss_desc);
			cpu_ctx->regs.ss = (new_ss & ~3) | dpl;
			cpu_ctx->regs.ss_hidden.base = stack_base;
			cpu_ctx->regs.ss_hidden.limit = read_seg_desc_limit_helper(cpu, ss_desc);
			cpu_ctx->regs.ss_hidden.flags = ss_flags;
			cpu_ctx->hflags = ((ss_flags & SEG_HIDDEN_DB) >> 19) | (cpu_ctx->hflags & ~HFLG_SS32);
		}
		else {
			if (type) { // push 32, not priv
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), old_eflags, eip, 0);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.cs, eip, 0);
				esp -= 4;
				mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), eip, eip, 0);
				if (has_code) {
					esp -= 4;
					mem_write<uint32_t>(cpu, stack_base + (esp & stack_mask), code, eip, 0);
				}
			}
			else { // push 16, not priv
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), old_eflags, eip, 0);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.cs, eip, 0);
				esp -= 2;
				mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), eip, eip, 0);
				if (has_code) {
					esp -= 2;
					mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), code, eip, 0);
				}
			}
		}

		cpu_ctx->regs.eflags = (eflags & ~(VM_MASK | RF_MASK | NT_MASK | TF_MASK));
		cpu_ctx->regs.esp = (cpu_ctx->regs.esp & ~stack_mask) | (esp & stack_mask);
		cpu_ctx->regs.cs = (sel & ~3) | dpl;
		cpu_ctx->regs.cs_hidden.base = seg_base;
		cpu_ctx->regs.cs_hidden.limit = seg_limit;
		cpu_ctx->regs.cs_hidden.flags = seg_flags;
		cpu_ctx->hflags = (((seg_flags & SEG_HIDDEN_DB) >> 20) | dpl) | (cpu_ctx->hflags & ~(HFLG_CS32 | HFLG_CPL));
		cpu_ctx->regs.eip = new_eip;
		// always clear HFLG_DBG_TRAP
		cpu_ctx->hflags &= ~HFLG_DBG_TRAP;
		if (idx == EXP_PF) {
			cpu_ctx->regs.cr2 = fault_addr;
		}
		if (idx == EXP_DB) {
			cpu_ctx->regs.dr7 &= ~DR7_GD_MASK;
		}
		cpu_ctx->exp_info.old_exp = EXP_INVALID;
	}
	else {
		// real mode

		if (idx * 4 + 3 > cpu_ctx->regs.idtr_hidden.limit) {
			cpu_ctx->exp_info.exp_data.code = idx * 8 + 2;
			cpu_ctx->exp_info.exp_data.idx = EXP_GP;
			return cpu_raise_exception(cpu_ctx);
		}

		uint32_t vec_entry = mem_read<uint32_t>(cpu, cpu_ctx->regs.idtr_hidden.base + idx * 4, eip, 0);
		uint32_t stack_mask = 0xFFFF;
		uint32_t stack_base = cpu_ctx->regs.ss_hidden.base;
		uint32_t esp = cpu_ctx->regs.esp;
		esp -= 2;
		mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), old_eflags, eip, 0);
		esp -= 2;
		mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), cpu_ctx->regs.cs, eip, 0);
		esp -= 2;
		mem_write<uint16_t>(cpu, stack_base + (esp & stack_mask), eip, eip, 0);

		cpu_ctx->regs.eflags &= ~(AC_MASK | RF_MASK | IF_MASK | TF_MASK);
		cpu_ctx->regs.esp = (cpu_ctx->regs.esp & ~stack_mask) | (esp & stack_mask);
		cpu_ctx->regs.cs = vec_entry >> 16;
		cpu_ctx->regs.cs_hidden.base = cpu_ctx->regs.cs << 4;
		cpu_ctx->regs.eip = vec_entry & 0xFFFF;
		// always clear HFLG_DBG_TRAP
		cpu_ctx->hflags &= ~HFLG_DBG_TRAP;
		if (idx == EXP_DB) {
			cpu_ctx->regs.dr7 &= ~DR7_GD_MASK;
		}
		cpu_ctx->exp_info.old_exp = EXP_INVALID;
	}

	return nullptr;
}

addr_t
get_pc(cpu_ctx_t *cpu_ctx)
{
	return cpu_ctx->regs.cs_hidden.base + cpu_ctx->regs.eip;
}

translated_code_t::translated_code_t() noexcept
{
	size = 0;
	flags = 0;
	ptr_code = nullptr;
}

static inline uint32_t
tc_hash(addr_t pc)
{
	return pc & (CODE_CACHE_MAX_SIZE - 1);
}

template<bool remove_hook>
void tc_invalidate(cpu_ctx_t *cpu_ctx, addr_t addr, [[maybe_unused]] uint8_t size, [[maybe_unused]] uint32_t eip)
{
	bool halt_tc = false;
	addr_t phys_addr;
	uint8_t is_code;

	if constexpr (remove_hook) {
		phys_addr = get_write_addr(cpu_ctx->cpu, addr, 2, cpu_ctx->regs.eip, &is_code);
	}
	else {
		if (cpu_ctx->cpu->cpu_flags & CPU_ALLOW_CODE_WRITE) {
			return;
		}

		try {
			phys_addr = get_write_addr(cpu_ctx->cpu, addr, 2, eip, &is_code);
		}
		catch (host_exp_t type) {
			// because all callers of this function translate the address already, this should never happen
			LIB86CPU_ABORT_msg("Unexpected page fault in %s", __func__);
		}
	}

	// find all tc's in the page addr belongs to
	auto it_map = cpu_ctx->cpu->tc_page_map.find(phys_addr >> PAGE_SHIFT);
	if (it_map != cpu_ctx->cpu->tc_page_map.end()) {
		auto it_set = it_map->second.begin();
		uint32_t flags = (cpu_ctx->hflags & HFLG_CONST) | (cpu_ctx->regs.eflags & EFLAGS_CONST);
		std::vector<std::unordered_set<translated_code_t *>::iterator> tc_to_delete;
		// iterate over all tc's found in the page
		while (it_set != it_map->second.end()) {
			translated_code_t *tc_in_page = *it_set;
			// only invalidate the tc if phys_addr is included in the translated address range of the tc
			// hook tc's have a zero guest code size, so they are unaffected by guest writes and do not need to be considered by tc_invalidate
			bool remove_tc;
			if constexpr (remove_hook) {
				remove_tc = !tc_in_page->size && (tc_in_page->pc == phys_addr);
			}
			else {
				remove_tc = tc_in_page->size && !(std::min(phys_addr + size - 1, tc_in_page->pc + tc_in_page->size - 1) < std::max(phys_addr, tc_in_page->pc));
			}

			if (remove_tc) {
				auto it_list = tc_in_page->linked_tc.begin();
				// now unlink all other tc's which jump to this tc
				while (it_list != tc_in_page->linked_tc.end()) {
					if ((*it_list)->jmp_offset[0] == tc_in_page->ptr_code) {
						(*it_list)->jmp_offset[0] = (*it_list)->jmp_offset[2];
					}
					if ((*it_list)->jmp_offset[1] == tc_in_page->ptr_code) {
						(*it_list)->jmp_offset[1] = (*it_list)->jmp_offset[2];
					}
					it_list++;
				}

				// delete the found tc from the code cache
				uint32_t idx = tc_hash(tc_in_page->pc);
				auto it = cpu_ctx->cpu->code_cache[idx].begin();
				while (it != cpu_ctx->cpu->code_cache[idx].end()) {
					if (it->get() == tc_in_page) {
						try {
							if (it->get()->cs_base == cpu_ctx->regs.cs_hidden.base &&
								it->get()->pc == get_code_addr(cpu_ctx->cpu, get_pc(cpu_ctx), cpu_ctx->regs.eip) &&
								it->get()->cpu_flags == flags) {
								// worst case: the write overlaps with the tc we are currently executing
								halt_tc = true;
								if constexpr (!remove_hook) {
									cpu_ctx->cpu->cpu_flags |= (CPU_DISAS_ONE | CPU_ALLOW_CODE_WRITE);
								}
							}
						}
						catch (host_exp_t type) {
							// the current tc cannot fault
						}
						cpu_ctx->cpu->code_cache[idx].erase(it);
						cpu_ctx->cpu->num_tc--;
						break;
					}
					it++;
				}

				// we can't delete the tc in tc_page_map right now because it would invalidate its iterator, which is still needed below
				tc_to_delete.push_back(it_set);

				if constexpr (remove_hook) {
					break;
				}
			}
			it_set++;
		}

		// delete the found tc's from tc_page_map and ibtc
		for (auto &it : tc_to_delete) {
			auto it_ibtc = cpu_ctx->cpu->ibtc.find((*it)->virt_pc);
			if (it_ibtc != cpu_ctx->cpu->ibtc.end()) {
				cpu_ctx->cpu->ibtc.erase(it_ibtc);
			}
			it_map->second.erase(it);
		}

		// if the tc_page_map for addr is now empty, also clear TLB_CODE and its key in the map
		if (it_map->second.empty()) {
			cpu_ctx->tlb[addr >> PAGE_SHIFT] &= ~TLB_CODE;
			cpu_ctx->cpu->tc_page_map.erase(it_map);
		}
	}

	if (halt_tc) {
		// in this case the tc we were executing has been destroyed and thus we must return to the translator with an exception
		if constexpr (!remove_hook) {
			cpu_ctx->regs.eip = eip;
		}
		throw host_exp_t::halt_tc;
	}
}

template void tc_invalidate<true>(cpu_ctx_t *cpu_ctx, addr_t addr, [[maybe_unused]] uint8_t size, [[maybe_unused]] uint32_t eip);
template void tc_invalidate<false>(cpu_ctx_t *cpu_ctx, addr_t addr, [[maybe_unused]] uint8_t size, [[maybe_unused]] uint32_t eip);

static translated_code_t *
tc_cache_search(cpu_t *cpu, addr_t pc)
{
	uint32_t flags = (cpu->cpu_ctx.hflags & HFLG_CONST) | (cpu->cpu_ctx.regs.eflags & EFLAGS_CONST);
	uint32_t idx = tc_hash(pc);
	auto it = cpu->code_cache[idx].begin();
	while (it != cpu->code_cache[idx].end()) {
		translated_code_t *tc = it->get();
		if (tc->cs_base == cpu->cpu_ctx.regs.cs_hidden.base &&
			tc->pc == pc &&
			tc->cpu_flags == flags) {
			return tc;
		}
		it++;
	}

	return nullptr;
}

static void
tc_cache_insert(cpu_t *cpu, addr_t pc, std::unique_ptr<translated_code_t> &&tc)
{
	cpu->num_tc++;
	cpu->tc_page_map[pc >> PAGE_SHIFT].insert(tc.get());
	cpu->code_cache[tc_hash(pc)].push_front(std::move(tc));
}

void
tc_cache_clear(cpu_t *cpu)
{
	// Use this when you want to destroy all tc's but without affecting the actual code allocated. E.g: on x86-64, you'll want to keep the .pdata sections
	// when this is called from a function called from the JITed code, and the current function can potentially throw an exception
	cpu->num_tc = 0;
	cpu->tc_page_map.clear();
	cpu->ibtc.clear();
	for (auto &bucket : cpu->code_cache) {
		bucket.clear();
	}
}

void
tc_cache_purge(cpu_t *cpu)
{
	// This is like tc_cache_clear, but it also frees all code allocated. E.g: on x86-64, the jit also emits .pdata sections that hold the exception tables
	// necessary to unwind the stack of the JITed functions
	tc_cache_clear(cpu);
	cpu->jit->destroy_all_code();
}

static void
tc_link_direct(translated_code_t *prev_tc, translated_code_t *ptr_tc)
{
	uint32_t num_jmp = prev_tc->flags & TC_FLG_NUM_JMP;

	switch (num_jmp)
	{
	case 0:
		break;

	case 1:
	case 2:
		switch ((prev_tc->flags & TC_FLG_JMP_TAKEN) >> 4)
		{
		case TC_JMP_DST_PC:
			prev_tc->jmp_offset[0] = ptr_tc->ptr_code;
			ptr_tc->linked_tc.push_front(prev_tc);
			break;

		case TC_JMP_NEXT_PC:
			prev_tc->jmp_offset[1] = ptr_tc->ptr_code;
			ptr_tc->linked_tc.push_front(prev_tc);
			break;

		case TC_JMP_RET:
			if (num_jmp == 1) {
				break;
			}
			[[fallthrough]];

		default:
			LIB86CPU_ABORT();
		}
		break;

	default:
		LIB86CPU_ABORT();
	}
}

void
tc_link_dst_only(translated_code_t *prev_tc, translated_code_t *ptr_tc)
{
	switch (prev_tc->flags & TC_FLG_NUM_JMP)
	{
	case 0:
		break;

	case 1:
		prev_tc->jmp_offset[0] = ptr_tc->ptr_code;
		ptr_tc->linked_tc.push_front(prev_tc);
		break;

	default:
		LIB86CPU_ABORT();
	}
}

entry_t
link_indirect_handler(cpu_ctx_t *cpu_ctx, translated_code_t *tc)
{
	const auto it = cpu_ctx->cpu->ibtc.find(get_pc(cpu_ctx));

	if (it != cpu_ctx->cpu->ibtc.end()) {
		if (it->second->cs_base == cpu_ctx->regs.cs_hidden.base &&
			it->second->cpu_flags == ((cpu_ctx->hflags & HFLG_CONST) | (cpu_ctx->regs.eflags & EFLAGS_CONST)) &&
			((it->second->virt_pc & ~PAGE_MASK) == (tc->virt_pc & ~PAGE_MASK))) {
			return it->second->ptr_code;
		}
	}

	return tc->jmp_offset[2];
}

static void
cpu_translate(cpu_t *cpu, disas_ctx_t *disas_ctx)
{
	cpu->translate_next = 1;
	cpu->virt_pc = disas_ctx->virt_pc;

	ZydisDecodedInstruction instr;
	ZydisDecoder decoder;
	ZyanStatus status;

	init_instr_decoder(disas_ctx, &decoder);

	do {
		cpu->instr_eip = cpu->virt_pc - cpu->cpu_ctx.regs.cs_hidden.base;

		try {
			status = decode_instr(cpu, disas_ctx, &decoder, &instr);
		}
		catch (host_exp_t type) {
			// this happens on instr breakpoints (not int3)
			assert(type == host_exp_t::de_exp);
			cpu->jit->raise_exp_inline_emit(0, 0, EXP_DB, cpu->instr_eip);
			disas_ctx->flags |= DISAS_FLG_DBG_FAULT;
			return;
		}

		if (ZYAN_SUCCESS(status)) {
			// successfully decoded

			cpu->instr_bytes = instr.length;
			disas_ctx->flags |= ((disas_ctx->virt_pc & ~PAGE_MASK) != ((disas_ctx->virt_pc + cpu->instr_bytes - 1) & ~PAGE_MASK)) << 2;
			disas_ctx->pc += cpu->instr_bytes;
			disas_ctx->virt_pc += cpu->instr_bytes;

			// att syntax uses percentage symbols to designate the operands, which will cause an error/crash if we (or the client)
			// attempts to interpret them as conversion specifiers, so we pass the formatted instruction as an argument
			LOG(log_level::debug, "0x%08X  %s", disas_ctx->virt_pc - cpu->instr_bytes, instr_logfn(disas_ctx->virt_pc - cpu->instr_bytes, &instr).c_str());
		}
		else {
			// NOTE: if rf is set, then it means we are translating the instr that caused a breakpoint. However, the exp handler always clears rf on itw own,
			// which means we do not need to do it again here in the case the original instr raises another kind of exp
			switch (status)
			{
			case ZYDIS_STATUS_BAD_REGISTER:
			case ZYDIS_STATUS_ILLEGAL_LOCK:
			case ZYDIS_STATUS_DECODING_ERROR:
				// illegal and/or undefined instruction, or lock prefix used on an instruction which does not accept it or used as source operand,
				// or the instruction encodes a register that cannot be used (e.g. mov cs, edx)
				cpu->jit->raise_exp_inline_emit(0, 0, EXP_UD, cpu->instr_eip);
				return;

			case ZYDIS_STATUS_NO_MORE_DATA:
				// buffer < 15 bytes
				cpu->cpu_flags &= ~(CPU_DISAS_ONE | CPU_ALLOW_CODE_WRITE);
				if (disas_ctx->exp_data.idx == EXP_PF) {
					// buffer size reduced because of page fault on second page
					disas_ctx->flags |= DISAS_FLG_FETCH_FAULT;
					cpu->jit->raise_exp_inline_emit(disas_ctx->exp_data.fault_addr, disas_ctx->exp_data.code, disas_ctx->exp_data.idx, disas_ctx->exp_data.eip);
					return;
				}
				else {
					// buffer size reduced because ram/rom region ended
					LIB86CPU_ABORT_msg("Attempted to execute code outside of ram/rom!");
				}

			case ZYDIS_STATUS_INSTRUCTION_TOO_LONG: {
				// instruction length > 15 bytes
				cpu->cpu_flags &= ~(CPU_DISAS_ONE | CPU_ALLOW_CODE_WRITE);
				volatile addr_t addr = get_code_addr(cpu, disas_ctx->virt_pc + X86_MAX_INSTR_LENGTH, disas_ctx->virt_pc - cpu->cpu_ctx.regs.cs_hidden.base, TLB_CODE, disas_ctx);
				if (disas_ctx->exp_data.idx == EXP_PF) {
					disas_ctx->flags |= DISAS_FLG_FETCH_FAULT;
					cpu->jit->raise_exp_inline_emit(disas_ctx->exp_data.fault_addr, disas_ctx->exp_data.code, disas_ctx->exp_data.idx, disas_ctx->exp_data.eip);
				}
				else {
					cpu->jit->raise_exp_inline_emit(0, 0, EXP_GP, disas_ctx->virt_pc - cpu->cpu_ctx.regs.cs_hidden.base);
				}
				return;
			}

			default:
				LIB86CPU_ABORT_msg("Unhandled zydis decode return status");
			}
		}


		if ((disas_ctx->flags & DISAS_FLG_CS32) ^ ((instr.attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) >> 34)) {
			cpu->size_mode = SIZE32;
		}
		else {
			cpu->size_mode = SIZE16;
		}

		if ((disas_ctx->flags & DISAS_FLG_CS32) ^ ((instr.attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) >> 35)) {
			cpu->addr_mode = ADDR32;
		}
		else {
			cpu->addr_mode = ADDR16;
		}

		switch (instr.mnemonic)
		{
		case ZYDIS_MNEMONIC_AAA: BAD;
		case ZYDIS_MNEMONIC_AAD: BAD;
		case ZYDIS_MNEMONIC_AAM: BAD;
		case ZYDIS_MNEMONIC_AAS: BAD;
		case ZYDIS_MNEMONIC_ADC: BAD;
		case ZYDIS_MNEMONIC_ADD:
			cpu->jit->add(&instr);
			break;

		case ZYDIS_MNEMONIC_AND:
			cpu->jit->and_(&instr);
			break;

		case ZYDIS_MNEMONIC_ARPL: BAD;
		case ZYDIS_MNEMONIC_BOUND: BAD;
		case ZYDIS_MNEMONIC_BSF:
			cpu->jit->bsf(&instr);
			break;

		case ZYDIS_MNEMONIC_BSR:
			cpu->jit->bsr(&instr);
			break;

		case ZYDIS_MNEMONIC_BSWAP: BAD;
		case ZYDIS_MNEMONIC_BT:BAD;
		case ZYDIS_MNEMONIC_BTC:BAD;
		case ZYDIS_MNEMONIC_BTR:BAD;
		case ZYDIS_MNEMONIC_BTS: BAD;
		case ZYDIS_MNEMONIC_CALL:
			cpu->jit->call(&instr);
			break;

		case ZYDIS_MNEMONIC_CBW: BAD;
		case ZYDIS_MNEMONIC_CDQ: BAD;
		case ZYDIS_MNEMONIC_CLC:
			cpu->jit->clc(&instr);
			break;

		case ZYDIS_MNEMONIC_CLD:
			cpu->jit->cld(&instr);
			break;

		case ZYDIS_MNEMONIC_CLI:
			cpu->jit->cli(&instr);
			break;

		case ZYDIS_MNEMONIC_CLTS:        BAD;
		case ZYDIS_MNEMONIC_CMC:         BAD;
		case ZYDIS_MNEMONIC_CMOVB:	 BAD;
		case ZYDIS_MNEMONIC_CMOVBE:	 BAD;
		case ZYDIS_MNEMONIC_CMOVL:	 BAD;
		case ZYDIS_MNEMONIC_CMOVLE:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNB:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNBE: BAD;
		case ZYDIS_MNEMONIC_CMOVNL:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNLE: BAD;
		case ZYDIS_MNEMONIC_CMOVNO:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNP:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNS:	 BAD;
		case ZYDIS_MNEMONIC_CMOVNZ:	 BAD;
		case ZYDIS_MNEMONIC_CMOVO:	 BAD;
		case ZYDIS_MNEMONIC_CMOVP:	 BAD;
		case ZYDIS_MNEMONIC_CMOVS:	 BAD;
		case ZYDIS_MNEMONIC_CMOVZ: BAD;
		case ZYDIS_MNEMONIC_CMP:
			cpu->jit->cmp(&instr);
			break;

		case ZYDIS_MNEMONIC_CMPSB:
		case ZYDIS_MNEMONIC_CMPSW:
		case ZYDIS_MNEMONIC_CMPSD:
			cpu->jit->cmps(&instr);
			break;

		case ZYDIS_MNEMONIC_CMPXCHG8B:   BAD;
		case ZYDIS_MNEMONIC_CMPXCHG:     BAD;
		case ZYDIS_MNEMONIC_CPUID:       BAD;
		case ZYDIS_MNEMONIC_CWD: BAD;
		case ZYDIS_MNEMONIC_CWDE: BAD;
		case ZYDIS_MNEMONIC_DAA: BAD;
		case ZYDIS_MNEMONIC_DAS: BAD;
		case ZYDIS_MNEMONIC_DEC:
			cpu->jit->dec(&instr);
			break;

		case ZYDIS_MNEMONIC_DIV:
			cpu->jit->div(&instr);
			break;

		case ZYDIS_MNEMONIC_ENTER: BAD;
		case ZYDIS_MNEMONIC_HLT:
			cpu->jit->hlt(&instr);
			break;

		case ZYDIS_MNEMONIC_IDIV: BAD;
		case ZYDIS_MNEMONIC_IMUL:
			cpu->jit->imul(&instr);
			break;

		case ZYDIS_MNEMONIC_IN: BAD;
		case ZYDIS_MNEMONIC_INC:
			cpu->jit->inc(&instr);
			break;

		case ZYDIS_MNEMONIC_INSB:BAD;
		case ZYDIS_MNEMONIC_INSD:BAD;
		case ZYDIS_MNEMONIC_INSW: BAD;
		case ZYDIS_MNEMONIC_INT3: BAD;
		case ZYDIS_MNEMONIC_INT:         BAD;
		case ZYDIS_MNEMONIC_INTO:        BAD;
		case ZYDIS_MNEMONIC_INVD:        BAD;
		case ZYDIS_MNEMONIC_INVLPG:      BAD;
		case ZYDIS_MNEMONIC_IRET:
		case ZYDIS_MNEMONIC_IRETD:
			cpu->jit->iret(&instr);
			break;

		case ZYDIS_MNEMONIC_JCXZ:
		case ZYDIS_MNEMONIC_JECXZ:
		case ZYDIS_MNEMONIC_JO:
		case ZYDIS_MNEMONIC_JNO:
		case ZYDIS_MNEMONIC_JB:
		case ZYDIS_MNEMONIC_JNB:
		case ZYDIS_MNEMONIC_JZ:
		case ZYDIS_MNEMONIC_JNZ:
		case ZYDIS_MNEMONIC_JBE:
		case ZYDIS_MNEMONIC_JNBE:
		case ZYDIS_MNEMONIC_JS:
		case ZYDIS_MNEMONIC_JNS:
		case ZYDIS_MNEMONIC_JP:
		case ZYDIS_MNEMONIC_JNP:
		case ZYDIS_MNEMONIC_JL:
		case ZYDIS_MNEMONIC_JNL:
		case ZYDIS_MNEMONIC_JLE:
		case ZYDIS_MNEMONIC_JNLE:
			cpu->jit->jcc(&instr);
			break;

		case ZYDIS_MNEMONIC_JMP:
			cpu->jit->jmp(&instr);
			break;

		case ZYDIS_MNEMONIC_LAHF:
			cpu->jit->lahf(&instr);
			break;

		case ZYDIS_MNEMONIC_LAR:         BAD;
		case ZYDIS_MNEMONIC_LEA:
			cpu->jit->lea(&instr);
			break;

		case ZYDIS_MNEMONIC_LEAVE:
			cpu->jit->leave(&instr);
			break;

		case ZYDIS_MNEMONIC_LGDT:
			cpu->jit->lgdt(&instr);
			break;

		case ZYDIS_MNEMONIC_LIDT:
			cpu->jit->lidt(&instr);
			break;

		case ZYDIS_MNEMONIC_LLDT:
			cpu->jit->lldt(&instr);
			break;

		case ZYDIS_MNEMONIC_LMSW:        BAD;
		case ZYDIS_MNEMONIC_LODSB:
		case ZYDIS_MNEMONIC_LODSD:
		case ZYDIS_MNEMONIC_LODSW:
			cpu->jit->lods(&instr);
			break;

		case ZYDIS_MNEMONIC_LOOP:
		case ZYDIS_MNEMONIC_LOOPE:
		case ZYDIS_MNEMONIC_LOOPNE:
			cpu->jit->loop(&instr);
			break;

		case ZYDIS_MNEMONIC_LSL:         BAD;
		case ZYDIS_MNEMONIC_LDS:
			cpu->jit->lds(&instr);
			break;

		case ZYDIS_MNEMONIC_LES:
			cpu->jit->les(&instr);
			break;

		case ZYDIS_MNEMONIC_LFS:
			cpu->jit->lfs(&instr);
			break;

		case ZYDIS_MNEMONIC_LGS:
			cpu->jit->lgs(&instr);
			break;

		case ZYDIS_MNEMONIC_LSS:
			cpu->jit->lss(&instr);
			break;

		case ZYDIS_MNEMONIC_LTR:
			cpu->jit->ltr(&instr);
			break;

		case ZYDIS_MNEMONIC_MOV:
			cpu->jit->mov(&instr);
			break;

		case ZYDIS_MNEMONIC_MOVD:BAD;
		case ZYDIS_MNEMONIC_MOVSB:
		case ZYDIS_MNEMONIC_MOVSD:
		case ZYDIS_MNEMONIC_MOVSW:
			cpu->jit->movs(&instr);
			break;

		case ZYDIS_MNEMONIC_MOVSX:
			cpu->jit->movsx(&instr);
			break;

		case ZYDIS_MNEMONIC_MOVZX:
			cpu->jit->movzx(&instr);
			break;

		case ZYDIS_MNEMONIC_MUL:
			cpu->jit->mul(&instr);
			break;

		case ZYDIS_MNEMONIC_NEG:
			cpu->jit->neg(&instr);
			break;

		case ZYDIS_MNEMONIC_NOP:
			// nothing to do
			break;

		case ZYDIS_MNEMONIC_NOT:
			cpu->jit->not_(&instr);
			break;

		case ZYDIS_MNEMONIC_OR:
			cpu->jit->or_(&instr);
			break;

		case ZYDIS_MNEMONIC_OUT:
			cpu->jit->out(&instr);
			break;

		case ZYDIS_MNEMONIC_OUTSB:BAD;
		case ZYDIS_MNEMONIC_OUTSD:BAD;
		case ZYDIS_MNEMONIC_OUTSW:BAD;
		case ZYDIS_MNEMONIC_POP:
			cpu->jit->pop(&instr);
			break;

		case ZYDIS_MNEMONIC_POPA:
		case ZYDIS_MNEMONIC_POPAD:
			cpu->jit->popa(&instr);
			break;

		case ZYDIS_MNEMONIC_POPF:
		case ZYDIS_MNEMONIC_POPFD:
			cpu->jit->popf(&instr);
			break;

		case ZYDIS_MNEMONIC_PUSH:
			cpu->jit->push(&instr);
			break;

		case ZYDIS_MNEMONIC_PUSHA:
		case ZYDIS_MNEMONIC_PUSHAD:
			cpu->jit->pusha(&instr);
			break;

		case ZYDIS_MNEMONIC_PUSHF:
		case ZYDIS_MNEMONIC_PUSHFD:
			cpu->jit->pushf(&instr);
			break;

		case ZYDIS_MNEMONIC_RCL: BAD;
		case ZYDIS_MNEMONIC_RCR: BAD;
		case ZYDIS_MNEMONIC_RDMSR: BAD;
		case ZYDIS_MNEMONIC_RDPMC:       BAD;
		case ZYDIS_MNEMONIC_RDTSC: BAD;
		case ZYDIS_MNEMONIC_RET:
			cpu->jit->ret(&instr);
			break;

		case ZYDIS_MNEMONIC_ROL: BAD;
		case ZYDIS_MNEMONIC_ROR: BAD;
		case ZYDIS_MNEMONIC_RSM:         BAD;
		case ZYDIS_MNEMONIC_SAHF:
			cpu->jit->sahf(&instr);
			break;

		case ZYDIS_MNEMONIC_SAR:
			cpu->jit->sar(&instr);
			break;

		case ZYDIS_MNEMONIC_SBB:
			cpu->jit->sbb(&instr);
			break;

		case ZYDIS_MNEMONIC_SCASB:
		case ZYDIS_MNEMONIC_SCASD:
		case ZYDIS_MNEMONIC_SCASW:
			cpu->jit->scas(&instr);
			break;

		case ZYDIS_MNEMONIC_SETB:BAD;
		case ZYDIS_MNEMONIC_SETBE:BAD;
		case ZYDIS_MNEMONIC_SETL:BAD;
		case ZYDIS_MNEMONIC_SETLE:BAD;
		case ZYDIS_MNEMONIC_SETNB:BAD;
		case ZYDIS_MNEMONIC_SETNBE:BAD;
		case ZYDIS_MNEMONIC_SETNL:BAD;
		case ZYDIS_MNEMONIC_SETNLE:BAD;
		case ZYDIS_MNEMONIC_SETNO:BAD;
		case ZYDIS_MNEMONIC_SETNP:BAD;
		case ZYDIS_MNEMONIC_SETNS:BAD;
		case ZYDIS_MNEMONIC_SETNZ:BAD;
		case ZYDIS_MNEMONIC_SETO:BAD;
		case ZYDIS_MNEMONIC_SETP:BAD;
		case ZYDIS_MNEMONIC_SETS:BAD;
		case ZYDIS_MNEMONIC_SETZ: BAD;
		case ZYDIS_MNEMONIC_SGDT:        BAD;
		case ZYDIS_MNEMONIC_SHL:
			cpu->jit->shl(&instr);
			break;

		case ZYDIS_MNEMONIC_SHLD: BAD;
		case ZYDIS_MNEMONIC_SHR:
			cpu->jit->shr(&instr);
			break;

		case ZYDIS_MNEMONIC_SHRD: BAD;
		case ZYDIS_MNEMONIC_SIDT:        BAD;
		case ZYDIS_MNEMONIC_SLDT:        BAD;
		case ZYDIS_MNEMONIC_SMSW:        BAD;
		case ZYDIS_MNEMONIC_STC:
			cpu->jit->stc(&instr);
			break;

		case ZYDIS_MNEMONIC_STD:
			cpu->jit->std(&instr);
			break;

		case ZYDIS_MNEMONIC_STI:
			cpu->jit->sti(&instr);
			break;

		case ZYDIS_MNEMONIC_STOSB:
		case ZYDIS_MNEMONIC_STOSD:
		case ZYDIS_MNEMONIC_STOSW:
			cpu->jit->stos(&instr);
			break;

		case ZYDIS_MNEMONIC_STR:         BAD;
		case ZYDIS_MNEMONIC_SUB:
			cpu->jit->sub(&instr);
			break;

		case ZYDIS_MNEMONIC_SYSENTER:    BAD;
		case ZYDIS_MNEMONIC_SYSEXIT:     BAD;
		case ZYDIS_MNEMONIC_TEST:
			cpu->jit->test(&instr);
			break;

		case ZYDIS_MNEMONIC_UD1:         BAD;
		case ZYDIS_MNEMONIC_UD2:         BAD;
		case ZYDIS_MNEMONIC_VERR:
			cpu->jit->verr(&instr);
			break;

		case ZYDIS_MNEMONIC_VERW:
			cpu->jit->verw(&instr);
			break;

		case ZYDIS_MNEMONIC_WBINVD:      BAD;
		case ZYDIS_MNEMONIC_WRMSR:       BAD;
		case ZYDIS_MNEMONIC_XADD:        BAD;
		case ZYDIS_MNEMONIC_XCHG:
			cpu->jit->xchg(&instr);
			break;

		case ZYDIS_MNEMONIC_XLAT:        BAD;
		case ZYDIS_MNEMONIC_XOR:
			cpu->jit->xor_(&instr);
			break;

#if 0
		case ZYDIS_MNEMONIC_AAA: {
			std::vector<BasicBlock *> vec_bb = getBBs(3);
			BR_COND(vec_bb[0], vec_bb[1], OR(ICMP_UGT(AND(LD_R8L(EAX_idx), CONST8(0xF)), CONST8(9)), ICMP_NE(LD_AF(), CONST32(0))));
			cpu->bb = vec_bb[0];
			ST_REG_idx(ADD(LD_R16(EAX_idx), CONST16(0x106)), EAX_idx);
			ST_FLG_AUX(CONST32(0x80000008));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST_FLG_AUX(CONST32(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
			ST_REG_idx(AND(LD_R8L(EAX_idx), CONST8(0xF)), EAX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_AAD: {
			Value *al = LD_R8L(EAX_idx);
			Value *ah = LD_R8H(EAX_idx);
			ST_REG_idx(ADD(al, MUL(ah, CONST8(instr.operands[OPNUM_SINGLE].imm.value.u))), EAX_idx);
			ST_R8H(CONST8(0), EAX_idx);
			ST_FLG_RES_ext(LD_R8L(EAX_idx));
			ST_FLG_AUX(CONST32(0));
		}
		break;

		case ZYDIS_MNEMONIC_AAM: {
			if (instr.operands[OPNUM_SINGLE].imm.value.u == 0) {
				RAISEin0(EXP_DE);
				translate_next = 0;
			}
			else {
				Value *al = LD_R8L(EAX_idx);
				ST_R8H(UDIV(al, CONST8(instr.operands[OPNUM_SINGLE].imm.value.u)), EAX_idx);
				ST_REG_idx(UREM(al, CONST8(instr.operands[OPNUM_SINGLE].imm.value.u)), EAX_idx);
				ST_FLG_RES_ext(LD_R8L(EAX_idx));
				ST_FLG_AUX(CONST32(0));
			}
		}
		break;

		case ZYDIS_MNEMONIC_AAS: {
			std::vector<BasicBlock *> vec_bb = getBBs(3);
			BR_COND(vec_bb[0], vec_bb[1], OR(ICMP_UGT(AND(LD_R8L(EAX_idx), CONST8(0xF)), CONST8(9)), ICMP_NE(LD_AF(), CONST32(0))));
			cpu->bb = vec_bb[0];
			ST_REG_idx(SUB(LD_R16(EAX_idx), CONST16(6)), EAX_idx);
			ST_R8H(SUB(LD_R8H(EAX_idx), CONST8(1)), EAX_idx);
			ST_FLG_AUX(CONST32(0x80000008));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST_FLG_AUX(CONST32(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
			ST_REG_idx(AND(LD_R8L(EAX_idx), CONST8(0xF)), EAX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_ADC: {
			Value *src, *sum1, *sum2, *dst, *rm, *cf, *sum_cout;
			switch (instr.opcode)
			{
			case 0x14:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x15: {
				switch (size_mode)
				{
				case SIZE8:
					src = CONST8(instr.operands[OPNUM_SRC].imm.value.u);
					rm = GEP_REG_idx(EAX_idx);
					dst = LD(rm, getIntegerType(8));
					break;

				case SIZE16:
					src = CONST16(instr.operands[OPNUM_SRC].imm.value.u);
					rm = GEP_REG_idx(EAX_idx);
					dst = LD(rm, getIntegerType(16));
					break;

				case SIZE32:
					src = CONST32(instr.operands[OPNUM_SRC].imm.value.u);
					rm = GEP_REG_idx(EAX_idx);
					dst = LD(rm, getIntegerType(32));
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
			break;

			case 0x80:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x81:
			case 0x83: {
				assert(instr.raw.modrm.reg == 2);

				if (instr.opcode == 0x83) {
					src = (size_mode == SIZE16) ? SEXT16(CONST8(instr.operands[OPNUM_SRC].imm.value.u)) :
						SEXT32(CONST8(instr.operands[OPNUM_SRC].imm.value.u));
				}
				else {
					src = GET_IMM();
				}

				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
			}
			break;

			case 0x10:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x11: {
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
			}
			break;

			case 0x12:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x13: {
				GET_RM(OPNUM_SRC, src = LD_REG_val(rm);, src = LD_MEM(fn_idx[size_mode], rm););
				rm = GET_REG(OPNUM_DST);
				dst = LD_REG_val(rm);
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			switch (size_mode)
			{
			case SIZE8:
				cf = TRUNC8(SHR(LD_CF(), CONST32(31)));
				sum1 = ADD(dst, src);
				sum2 = ADD(sum1, cf);
				sum_cout = GEN_SUM_VEC8(dst, src, sum2);
				break;

			case SIZE16:
				cf = TRUNC16(SHR(LD_CF(), CONST32(31)));
				sum1 = ADD(dst, src);
				sum2 = ADD(sum1, cf);
				sum_cout = GEN_SUM_VEC16(dst, src, sum2);
				break;

			case SIZE32:
				cf = SHR(LD_CF(), CONST32(31));
				sum1 = ADD(dst, src);
				sum2 = ADD(sum1, cf);
				sum_cout = GEN_SUM_VEC32(dst, src, sum2);
				break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(sum2, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, sum2);
			}

			SET_FLG(sum2, sum_cout);
		}
		break;

		case ZYDIS_MNEMONIC_ARPL: {
			assert((instr.operands[OPNUM_DST].size == 16) && (instr.operands[OPNUM_SRC].size == 16));

			Value *rm, *rpl_dst, *rpl_src = LD_REG_val(GET_REG(OPNUM_SRC));
			GET_RM(OPNUM_DST, rpl_dst = LD_REG_val(rm);, rpl_dst = LD_MEM(MEM_LD16_idx, rm););
			std::vector<BasicBlock *> vec_bb = getBBs(3);
			BR_COND(vec_bb[0], vec_bb[1], ICMP_ULT(AND(rpl_dst, CONST16(3)), AND(rpl_src, CONST16(3))));
			cpu->bb = vec_bb[0];
			Value *new_rpl = OR(AND(rpl_dst, CONST16(0xFFFC)), AND(rpl_src, CONST16(3)));
			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(new_rpl, rm);
			}
			else {
				ST_MEM(MEM_LD16_idx, rm, new_rpl);
			}
			Value *new_sfd = XOR(LD_SF(), CONST32(0));
			Value *new_pdb = SHL(XOR(AND(XOR(LD_FLG_RES(), SHR(LD_FLG_AUX(), CONST32(8))), CONST32(0xFF)), CONST32(0)), CONST32(8));
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0xFFFF00FE)), OR(new_sfd, new_pdb)));
			ST_FLG_RES(CONST32(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST_FLG_RES(OR(LD_FLG_RES(), CONST32(0x100)));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
		}
		break;

		case ZYDIS_MNEMONIC_BOUND: {
			Value *idx = LD_REG_val(GET_REG(OPNUM_DST));
			Value *idx_addr = GET_OP(OPNUM_SRC);
			Value *lower_idx = LD_MEM(fn_idx[size_mode], idx_addr);
			Value *upper_idx = LD_MEM(fn_idx[size_mode], ADD(idx_addr, (size_mode == SIZE16) ? CONST32(2) : CONST32(4)));
			std::vector<BasicBlock *> vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], OR(ICMP_SLT(idx, lower_idx), ICMP_SGT(idx, upper_idx)));
			cpu->bb = vec_bb[0];
			RAISEin0(EXP_BR);
			UNREACH();
			cpu->bb = vec_bb[1];
		}
		break;

		case ZYDIS_MNEMONIC_BSWAP: {
			int reg_idx = GET_REG_idx(instr.operands[OPNUM_SINGLE].reg.value);
			Value *temp = LD_R32(reg_idx);
			temp = INTRINSIC_ty(bswap, getIntegerType(32), temp);
			ST_REG_idx(temp, reg_idx);
		}
		break;

		case ZYDIS_MNEMONIC_BT:
		case ZYDIS_MNEMONIC_BTC:
		case ZYDIS_MNEMONIC_BTR:
		case ZYDIS_MNEMONIC_BTS: {
			Value *rm, *base, *offset, *idx, *cf, *cf2;
			size_t op_size = instr.operands[OPNUM_DST].size;
			if (instr.opcode != 0xBA) {
				offset = LD_REG_val(GET_REG(OPNUM_SRC));
			}
			else {
				offset = ZEXTs(op_size, GET_IMM8());
			}

			// NOTE: we can't use llvm's SDIV when the base is a memory operand because that rounds towards zero, while the instruction rounds the
			// offset towards negative infinity, that is, it does a floored division
			GET_RM(OPNUM_DST, base = LD_REG_val(rm); offset = UREM(offset, CONSTs(op_size, op_size));,
				offset = UREM(offset, CONSTs(op_size, 8)); idx = FLOOR_DIV(offset, CONSTs(op_size, 8), op_size);
				idx = (op_size == 16) ? ZEXT32(idx) : idx; base = LD_MEM(fn_idx[size_mode], ADD(rm, idx)););
			if (op_size == 16) {
				cf = AND(SHR(base, offset), CONST16(1));
				cf2 = ZEXT32(cf);
			}
			else {
				cf = AND(SHR(base, offset), CONST32(1));
				cf2 = cf;
			}

			switch (instr.operands[OPNUM_DST].type)
			{
			case ZYDIS_OPERAND_TYPE_REGISTER:
				switch (instr.mnemonic)
				{
				case ZYDIS_MNEMONIC_BTC:
					ST_REG_val(OR(AND(base, NOT(SHL(CONSTs(op_size, 1), offset))), SHL(AND(NOT(cf), CONSTs(op_size, 1)), offset)), rm);
					break;

				case ZYDIS_MNEMONIC_BTR:
					ST_REG_val(AND(base, NOT(SHL(CONSTs(op_size, 1), offset))), rm);
					break;

				case ZYDIS_MNEMONIC_BTS:
					ST_REG_val(OR(AND(base, NOT(SHL(CONSTs(op_size, 1), offset))), SHL(CONSTs(op_size, 1), offset)), rm);
					break;
				}
				break;

			case ZYDIS_OPERAND_TYPE_MEMORY:
				switch (instr.mnemonic)
				{
				case ZYDIS_MNEMONIC_BTC:
					ST_MEM(fn_idx[size_mode], ADD(rm, idx), OR(AND(base, NOT(SHL(CONSTs(op_size, 1), offset))), SHL(AND(NOT(cf), CONSTs(op_size, 1)), offset)));
					break;

				case ZYDIS_MNEMONIC_BTR:
					ST_MEM(fn_idx[size_mode], ADD(rm, idx), AND(base, NOT(SHL(CONSTs(op_size, 1), offset))));
					break;

				case ZYDIS_MNEMONIC_BTS:
					ST_MEM(fn_idx[size_mode], ADD(rm, idx), OR(AND(base, NOT(SHL(CONSTs(op_size, 1), offset))), SHL(CONSTs(op_size, 1), offset)));
					break;
				}
				break;

			default:
				LIB86CPU_ABORT_msg("Invalid operand type used in GET_RM macro!");
			}


			ST_FLG_AUX(SHL(cf2, CONST32(31)));
		}
		break;

		case ZYDIS_MNEMONIC_CBW: {
			ST_REG_idx(SEXT16(LD_R8L(EAX_idx)), EAX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_CDQ: {
			ST_REG_idx(TRUNC32(SHR(SEXT64(LD_R32(EAX_idx)), CONST64(32))), EDX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_CLTS:        BAD;
		case ZYDIS_MNEMONIC_CMC:         BAD;
		case ZYDIS_MNEMONIC_CMOVB:
		case ZYDIS_MNEMONIC_CMOVBE:
		case ZYDIS_MNEMONIC_CMOVL:
		case ZYDIS_MNEMONIC_CMOVLE:
		case ZYDIS_MNEMONIC_CMOVNB:
		case ZYDIS_MNEMONIC_CMOVNBE:
		case ZYDIS_MNEMONIC_CMOVNL:
		case ZYDIS_MNEMONIC_CMOVNLE:
		case ZYDIS_MNEMONIC_CMOVNO:
		case ZYDIS_MNEMONIC_CMOVNP:
		case ZYDIS_MNEMONIC_CMOVNS:
		case ZYDIS_MNEMONIC_CMOVNZ:
		case ZYDIS_MNEMONIC_CMOVO:
		case ZYDIS_MNEMONIC_CMOVP:
		case ZYDIS_MNEMONIC_CMOVS:
		case ZYDIS_MNEMONIC_CMOVZ: {
			Value *val;
			switch (instr.opcode)
			{
			case 0x40:
				val = ICMP_NE(LD_OF(), CONST32(0)); // OF != 0
				break;

			case 0x41:
				val = ICMP_EQ(LD_OF(), CONST32(0)); // OF == 0
				break;

			case 0x42:
				val = ICMP_NE(LD_CF(), CONST32(0)); // CF != 0
				break;

			case 0x43:
				val = ICMP_EQ(LD_CF(), CONST32(0)); // CF == 0
				break;

			case 0x44:
				val = ICMP_EQ(LD_ZF(), CONST32(0)); // ZF != 0
				break;

			case 0x45:
				val = ICMP_NE(LD_ZF(), CONST32(0)); // ZF == 0
				break;

			case 0x46:
				val = OR(ICMP_NE(LD_CF(), CONST32(0)), ICMP_EQ(LD_ZF(), CONST32(0))); // CF != 0 OR ZF != 0
				break;

			case 0x47:
				val = AND(ICMP_EQ(LD_CF(), CONST32(0)), ICMP_NE(LD_ZF(), CONST32(0))); // CF == 0 AND ZF == 0
				break;

			case 0x48:
				val = ICMP_NE(LD_SF(), CONST32(0)); // SF != 0
				break;

			case 0x49:
				val = ICMP_EQ(LD_SF(), CONST32(0)); // SF == 0
				break;

			case 0x4A:
				val = ICMP_EQ(LD_PF(), CONST8(0)); // PF != 0
				break;

			case 0x4B:
				val = ICMP_NE(LD_PF(), CONST8(0)); // PF == 0
				break;

			case 0x4C:
				val = ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF != OF
				break;

			case 0x4D:
				val = ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF == OF
				break;

			case 0x4E:
				val = OR(ICMP_EQ(LD_ZF(), CONST32(0)), ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF != 0 OR SF != OF
				break;

			case 0x4F:
				val = AND(ICMP_NE(LD_ZF(), CONST32(0)), ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF == 0 AND SF == OF
				break;

			default:
				LIB86CPU_ABORT();
			}

			Value *rm, *src;
			std::vector<BasicBlock *>vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], val);
			cpu->bb = vec_bb[0];
			GET_RM(OPNUM_SRC, src = LD_REG_val(rm);, src = LD_MEM(fn_idx[size_mode], rm););
			ST_REG_val(src, GET_REG(OPNUM_DST));
			BR_UNCOND(vec_bb[1]);
			cpu->bb = vec_bb[1];
		}
		break;

		case ZYDIS_MNEMONIC_CMPXCHG8B:   BAD;
		case ZYDIS_MNEMONIC_CMPXCHG:     BAD;
		case ZYDIS_MNEMONIC_CPUID:       BAD;
		case ZYDIS_MNEMONIC_CWD: {
			ST_REG_idx(TRUNC16(SHR(SEXT32(LD_R16(EAX_idx)), CONST32(16))), EDX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_CWDE: {
			ST_REG_idx(SEXT32(LD_R16(EAX_idx)), EAX_idx);
		}
		break;

		case ZYDIS_MNEMONIC_DAA: {
			Value *old_al = LD_R8L(EAX_idx);
			Value *old_cf = LD_CF();
			ST_FLG_AUX(AND(LD_FLG_AUX(), CONST32(8)));
			std::vector<BasicBlock *> vec_bb = getBBs(6);
			BR_COND(vec_bb[0], vec_bb[1], OR(ICMP_UGT(AND(old_al, CONST8(0xF)), CONST8(9)), ICMP_NE(LD_AF(), CONST32(0))));
			cpu->bb = vec_bb[0];
			Value *sum = ADD(old_al, CONST8(6));
			ST_REG_idx(sum, EAX_idx);
			ST_FLG_AUX(OR(OR(AND(GEN_SUM_VEC8(old_al, CONST8(6), sum), CONST32(0x80000000)), old_cf), CONST32(8)));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST_FLG_AUX(CONST32(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
			BR_COND(vec_bb[3], vec_bb[4], OR(ICMP_UGT(old_al, CONST8(0x99)), ICMP_NE(old_cf, CONST32(0))));
			cpu->bb = vec_bb[3];
			ST_REG_idx(ADD(LD_R8L(EAX_idx), CONST8(0x60)), EAX_idx);
			ST_FLG_AUX(OR(LD_FLG_AUX(), CONST32(0x80000000)));
			BR_UNCOND(vec_bb[5]);
			cpu->bb = vec_bb[4];
			ST_FLG_AUX(AND(LD_FLG_AUX(), CONST32(0x7FFFFFFF)));
			BR_UNCOND(vec_bb[5]);
			cpu->bb = vec_bb[5];
			ST_FLG_RES_ext(LD_R8L(EAX_idx));
		}
		break;

		case ZYDIS_MNEMONIC_DAS: {
			Value *old_al = LD_R8L(EAX_idx);
			Value *old_cf = LD_CF();
			ST_FLG_AUX(AND(LD_FLG_AUX(), CONST32(8)));
			std::vector<BasicBlock *> vec_bb = getBBs(5);
			BR_COND(vec_bb[0], vec_bb[1], OR(ICMP_UGT(AND(old_al, CONST8(0xF)), CONST8(9)), ICMP_NE(LD_AF(), CONST32(0))));
			cpu->bb = vec_bb[0];
			Value *sub = SUB(old_al, CONST8(6));
			ST_REG_idx(sub, EAX_idx);
			ST_FLG_AUX(OR(OR(AND(GEN_SUB_VEC8(old_al, CONST8(6), sub), CONST32(0x80000000)), old_cf), CONST32(8)));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST_FLG_AUX(CONST32(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
			BR_COND(vec_bb[3], vec_bb[4], OR(ICMP_UGT(old_al, CONST8(0x99)), ICMP_NE(old_cf, CONST32(0))));
			cpu->bb = vec_bb[3];
			ST_REG_idx(SUB(LD_R8L(EAX_idx), CONST8(0x60)), EAX_idx);
			ST_FLG_AUX(OR(LD_FLG_AUX(), CONST32(0x80000000)));
			BR_UNCOND(vec_bb[4]);
			cpu->bb = vec_bb[4];
			ST_FLG_RES_ext(LD_R8L(EAX_idx));
		}
		break;

		case ZYDIS_MNEMONIC_ENTER: {
			uint32_t nesting_lv = instr.operands[OPNUM_SRC].imm.value.u % 32;
			uint32_t stack_sub, push_tot_size = 0;
			Value *frame_esp, *ebp_addr, *esp_ptr, *ebp_ptr;
			std::vector<Value *> args;

			switch ((size_mode << 1) | ((cpu->cpu_ctx.hflags & HFLG_SS32) >> SS32_SHIFT))
			{
			case 0: { // sp, push 32
				stack_sub = 4;
				esp_ptr = GEP_REG_idx(ESP_idx);
				ebp_ptr = GEP_REG_idx(EBP_idx);
				ebp_addr = ALLOC32();
				ST(ebp_addr, ZEXT32(LD_R16(EBP_idx)));
				frame_esp = OR(ZEXT32(SUB(LD_R16(ESP_idx), CONST16(4))), AND(LD_R32(ESP_idx), CONST32(0xFFFF0000)));
				args.push_back(LD_R32(EBP_idx));
			}
			break;

			case 1: { // esp, push 32
				stack_sub = 4;
				esp_ptr = GEP_REG_idx(ESP_idx);
				ebp_ptr = GEP_REG_idx(EBP_idx);
				ebp_addr = ALLOC32();
				ST(ebp_addr, LD_R32(EBP_idx));
				frame_esp = SUB(LD_R32(ESP_idx), CONST32(4));
				args.push_back(LD_R32(EBP_idx));
			}
			break;

			case 2: { // sp, push 16
				stack_sub = 2;
				esp_ptr = GEP_REG_idx(ESP_idx);
				ebp_ptr = GEP_REG_idx(EBP_idx);
				ebp_addr = ALLOC32();
				ST(ebp_addr, ZEXT32(LD_R16(EBP_idx)));
				frame_esp = SUB(LD_R16(ESP_idx), CONST16(2));
				args.push_back(LD_R16(EBP_idx));
			}
			break;

			case 3: { // esp, push 16
				stack_sub = 2;
				esp_ptr = GEP_REG_idx(ESP_idx);
				ebp_ptr = GEP_REG_idx(EBP_idx);
				ebp_addr = ALLOC32();
				ST(ebp_addr, LD_R32(EBP_idx));
				frame_esp = TRUNC16(SUB(LD_R32(ESP_idx), CONST32(2)));
				args.push_back(LD_R16(EBP_idx));
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (nesting_lv > 0) {
				for (uint32_t i = 1; i < nesting_lv; ++i) {
					ST(ebp_addr, SUB(LD(ebp_addr, getIntegerType(32)), CONST32(stack_sub)));
					Value *new_ebp = LD_MEM(fn_idx[size_mode], ADD(LD(ebp_addr, getIntegerType(32)), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)));
					args.push_back(new_ebp);
					push_tot_size += stack_sub;
				}
				args.push_back(frame_esp);
				push_tot_size += stack_sub;
			}
			MEM_PUSH(args);

			ST(ebp_ptr, frame_esp);
			ST(esp_ptr, SUB(SUB(frame_esp, CONSTs(stack_sub << 3, push_tot_size)), CONSTs(stack_sub << 3, instr.operands[OPNUM_DST].imm.value.u)));
		}
		break;

		case ZYDIS_MNEMONIC_IDIV: {
			switch (instr.opcode)
			{
			case 0xF6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xF7: {
				assert(instr.raw.modrm.reg == 7);

				Value *val, *reg, *rm, *div;
				std::vector<BasicBlock *> vec_bb = getBBs(3);
				switch (size_mode)
				{
				case SIZE8:
					reg = LD_R16(EAX_idx);
					GET_RM(OPNUM_SINGLE, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(val, CONST8(0)));
					cpu->bb = vec_bb[0];
					RAISEin0(EXP_DE);
					UNREACH();
					cpu->bb = vec_bb[1];
					div = SDIV(reg, SEXT16(val));
					BR_COND(vec_bb[0], vec_bb[2], ICMP_NE(div, SEXT16(TRUNC8(div))));
					cpu->bb = vec_bb[2];
					ST_REG_val(TRUNC8(div), GEP_REG_idx(EAX_idx));
					ST_REG_val(TRUNC8(SREM(reg, SEXT16(val))), GEP_R8H(EAX_idx));
					break;

				case SIZE16:
					reg = OR(SHL(ZEXT32(LD_R16(EDX_idx)), CONST32(16)), ZEXT32(LD_R16(EAX_idx)));
					GET_RM(OPNUM_SINGLE, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(val, CONST16(0)));
					cpu->bb = vec_bb[0];
					RAISEin0(EXP_DE);
					UNREACH();
					cpu->bb = vec_bb[1];
					div = SDIV(reg, SEXT32(val));
					BR_COND(vec_bb[0], vec_bb[2], ICMP_NE(div, SEXT32(TRUNC16(div))));
					cpu->bb = vec_bb[2];
					ST_REG_val(TRUNC16(div), GEP_REG_idx(EAX_idx));
					ST_REG_val(TRUNC16(SREM(reg, SEXT32(val))), GEP_REG_idx(EDX_idx));
					break;

				case SIZE32:
					reg = OR(SHL(ZEXT64(LD_R32(EDX_idx)), CONST64(32)), ZEXT64(LD_R32(EAX_idx)));
					GET_RM(OPNUM_SINGLE, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(val, CONST32(0)));
					cpu->bb = vec_bb[0];
					RAISEin0(EXP_DE);
					UNREACH();
					cpu->bb = vec_bb[1];
					div = SDIV(reg, SEXT64(val));
					BR_COND(vec_bb[0], vec_bb[2], ICMP_NE(div, SEXT64(TRUNC32(div))));
					cpu->bb = vec_bb[2];
					ST_REG_val(TRUNC32(div), GEP_REG_idx(EAX_idx));
					ST_REG_val(TRUNC32(SREM(reg, SEXT64(val))), GEP_REG_idx(EDX_idx));
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case ZYDIS_MNEMONIC_IN: {
			switch (instr.opcode)
			{
			case 0xE4:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xE5: {
				Value *port = GET_IMM8();
				check_io_priv_emit(cpu, ZEXT32(port), size_mode);
				Value *val = LD_IO(ZEXT16(port));
				size_mode == SIZE16 ? ST_REG_idx(val, EAX_idx) : size_mode == SIZE32 ? ST_REG_idx(val, EAX_idx) : ST_REG_idx(val, EAX_idx);
			}
			break;

			case 0xEC:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xED: {
				Value *port = LD_R16(EDX_idx);
				check_io_priv_emit(cpu, ZEXT32(port), size_mode);
				Value *val = LD_IO(port);
				size_mode == SIZE16 ? ST_REG_idx(val, EAX_idx) : size_mode == SIZE32 ? ST_REG_idx(val, EAX_idx) : ST_REG_idx(val, EAX_idx);
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case ZYDIS_MNEMONIC_INSB:
		case ZYDIS_MNEMONIC_INSD:
		case ZYDIS_MNEMONIC_INSW: {
			switch (instr.opcode)
			{
			case 0x6C:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x6D: {
				Value *val, *df, *addr, *src, *edi, *io_val, *port = LD_R16(EDX_idx);
				check_io_priv_emit(cpu, ZEXT32(port), size_mode);
				std::vector<BasicBlock *> vec_bb = getBBs(3);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					edi = ZEXT32(LD_R16(EDI_idx));
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				case ADDR32:
					edi = LD_R32(EDI_idx);
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					io_val = LD_IO(port);
					ST_MEM(MEM_LD8_idx, addr, io_val);
					break;

				case SIZE16:
					val = CONST32(2);
					io_val = LD_IO(port);
					ST_MEM(MEM_LD16_idx, addr, io_val);
					break;

				case SIZE32:
					val = CONST32(4);
					io_val = LD_IO(port);
					ST_MEM(MEM_LD32_idx, addr, io_val);
					break;

				default:
					LIB86CPU_ABORT();
				}

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(df, CONST32(0)));

				cpu->bb = vec_bb[0];
				Value *edi_sum = ADD(edi, val);
				addr_mode == ADDR16 ? ST_REG_idx(TRUNC16(edi_sum), EDI_idx) : ST_REG_idx(edi_sum, EDI_idx);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP();
				}
				else {
					BR_UNCOND(vec_bb[2]);
				}

				cpu->bb = vec_bb[1];
				Value *edi_sub = SUB(edi, val);
				addr_mode == ADDR16 ? ST_REG_idx(TRUNC16(edi_sub), EDI_idx) : ST_REG_idx(edi_sub, EDI_idx);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP();
				}
				else {
					BR_UNCOND(vec_bb[2]);
				}

				cpu->bb = vec_bb[2];
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case ZYDIS_MNEMONIC_INT3: {
			// NOTE1: we don't support virtual 8086 mode, so we don't need to check for it
			// NOTE2: we can't just use RAISEin0 because the eip should point to the instr following int3
			RAISEisInt(0, 0, EXP_BP, (pc + bytes) - cpu_ctx->regs.cs_hidden.base);
			translate_next = 0;
		}
		break;

		case ZYDIS_MNEMONIC_INT:         BAD;
		case ZYDIS_MNEMONIC_INTO:        BAD;
		case ZYDIS_MNEMONIC_INVD:        BAD;
		case ZYDIS_MNEMONIC_INVLPG:      BAD;
		case ZYDIS_MNEMONIC_LAR:         BAD;
		case ZYDIS_MNEMONIC_LMSW:        BAD;
		case ZYDIS_MNEMONIC_LSL:         BAD;

		case ZYDIS_MNEMONIC_MOVD: {
			if (cpu_ctx->hflags & HFLG_CR0_EM) {
				RAISEin0(EXP_UD);
				translate_next = 0;
			}
			else {
				switch (instr.opcode)
				{
				case 0x6E: {
					Value *src, *rm;
					std::vector<BasicBlock *> vec_bb = getBBs(2);

					BR_COND(vec_bb[0], vec_bb[1], ICMP_NE(AND(LD_R16(ST_idx), CONST16(ST_ES_MASK)), CONST16(0)));
					cpu->bb = vec_bb[0];
					RAISEin0(EXP_MF);
					UNREACH();
					cpu->bb = vec_bb[1];
					GET_RM(OPNUM_SRC, src = LD_R32(GET_REG_idx(instr.operands[OPNUM_SRC].reg.value));, src = LD_MEM(MEM_LD32_idx, rm););
					int mm_idx = GET_REG_idx(instr.operands[OPNUM_DST].reg.value);
					ST_MM64(ZEXT64(src), mm_idx);
					UPDATE_FPU_AFTER_MMX_w(CONST16(0), mm_idx);
				}
				break;

				case 0x7E: {
					Value *rm;
					std::vector<BasicBlock *> vec_bb = getBBs(2);

					BR_COND(vec_bb[0], vec_bb[1], ICMP_NE(AND(LD_R16(ST_idx), CONST16(ST_ES_MASK)), CONST16(0)));
					cpu->bb = vec_bb[0];
					RAISEin0(EXP_MF);
					UNREACH();
					cpu->bb = vec_bb[1];
					int mm_idx = GET_REG_idx(instr.operands[OPNUM_SRC].reg.value);
					GET_RM(OPNUM_DST, ST_REG_idx(LD_MM32(mm_idx), GET_REG_idx(instr.operands[OPNUM_DST].reg.value));, ST_MEM(MEM_LD32_idx, rm, LD_MM32(mm_idx)););
					UPDATE_FPU_AFTER_MMX_r(CONST16(0), mm_idx);
				}
				break;

				default:
					BAD;
				}

			}
		}
		break;

		case ZYDIS_MNEMONIC_OUTSB:
		case ZYDIS_MNEMONIC_OUTSD:
		case ZYDIS_MNEMONIC_OUTSW:
			switch (instr.opcode)
			{
			case 0x6E:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x6F: {
				Value *val, *df, *addr, *src, *esi, *io_val, *port = LD_R16(EDX_idx);
				check_io_priv_emit(cpu, ZEXT32(port), size_mode);
				std::vector<BasicBlock *> vec_bb = getBBs(3);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					esi = ZEXT32(LD_R16(ESI_idx));
					addr = ADD(LD_SEG_HIDDEN(GET_REG_idx(instr.operands[OPNUM_SRC].mem.segment), SEG_BASE_idx), esi);
					break;

				case ADDR32:
					esi = LD_R32(ESI_idx);
					addr = ADD(LD_SEG_HIDDEN(GET_REG_idx(instr.operands[OPNUM_SRC].mem.segment), SEG_BASE_idx), esi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					io_val = LD_MEM(MEM_LD8_idx, addr);
					ST_IO(port, io_val);
					break;

				case SIZE16:
					val = CONST32(2);
					io_val = LD_MEM(MEM_LD16_idx, addr);
					ST_IO(port, io_val);
					break;

				case SIZE32:
					val = CONST32(4);
					io_val = LD_MEM(MEM_LD32_idx, addr);
					ST_IO(port, io_val);
					break;

				default:
					LIB86CPU_ABORT();
				}

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(df, CONST32(0)));

				cpu->bb = vec_bb[0];
				Value *esi_sum = ADD(esi, val);
				addr_mode == ADDR16 ? ST_REG_idx(TRUNC16(esi_sum), ESI_idx) : ST_REG_idx(esi_sum, ESI_idx);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP();
				}
				else {
					BR_UNCOND(vec_bb[2]);
				}

				cpu->bb = vec_bb[1];
				Value *esi_sub = SUB(esi, val);
				addr_mode == ADDR16 ? ST_REG_idx(TRUNC16(esi_sub), ESI_idx) : ST_REG_idx(esi_sub, ESI_idx);
				if (instr.attributes & ZYDIS_ATTRIB_HAS_REP) {
					REP();
				}
				else {
					BR_UNCOND(vec_bb[2]);
				}

				cpu->bb = vec_bb[2];
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
			break;

		case ZYDIS_MNEMONIC_RCL: {
			assert(instr.raw.modrm.reg == 2);

			Value *count;
			switch (instr.opcode)
			{
			case 0xD0:
				count = CONST32(1);
				size_mode = SIZE8;
				break;

			case 0xD2:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				size_mode = SIZE8;
				break;

			case 0xC0:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				size_mode = SIZE8;
				break;

			case 0xD1:
				count = CONST32(1);
				break;

			case 0xD3:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xC1:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];
			Value *val, *rm, *flg, *res;
			switch (size_mode)
			{
			case SIZE8: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i9 = OR(ZEXTs(9, val), TRUNCs(9, SHR(LD_CF(), CONST32(23))));
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(9), (std::vector<Value *> { i9, i9, TRUNCs(9, count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(CONST8(8), TRUNC8(count))), CONST8(1)));
				Value *of = ZEXT32(AND(rotl, CONSTs(9, 1 << 7)));
				flg = OR(SHL(cf, CONST32(31)), SHL(of, CONST32(23)));
				res = TRUNC8(rotl);
			}
			break;

			case SIZE16: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i17 = OR(ZEXTs(17, val), TRUNCs(17, SHR(LD_CF(), CONST32(15))));
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(17), (std::vector<Value *> { i17, i17, TRUNCs(17, count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(CONST16(16), TRUNC16(count))), CONST16(1)));
				Value *of = ZEXT32(AND(rotl, CONSTs(17, 1 << 15)));
				flg = OR(SHL(cf, CONST32(31)), SHL(of, CONST32(15)));
				res = TRUNC16(rotl);
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i33 = OR(ZEXTs(33, val), SHL(ZEXTs(33, LD_CF()), CONSTs(33, 1)));
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(33), (std::vector<Value *> { i33, i33, ZEXTs(33, count) }));
				Value *cf = AND(SHR(val, SUB(CONST32(32), count)), CONST32(1));
				Value *of = TRUNC32(SHR(AND(rotl, CONSTs(33, 1ULL << 31)), CONSTs(33, 1)));
				flg = OR(SHL(cf, CONST32(31)), of);
				res = TRUNC32(rotl);
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(res, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, res);
			}
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), flg));
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_RCR: {
			assert(instr.raw.modrm.reg == 3);

			Value *count;
			switch (instr.opcode)
			{
			case 0xD0:
				count = CONST32(1);
				size_mode = SIZE8;
				break;

			case 0xD2:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				size_mode = SIZE8;
				break;

			case 0xC0:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				size_mode = SIZE8;
				break;

			case 0xD1:
				count = CONST32(1);
				break;

			case 0xD3:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xC1:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];
			Value *val, *rm, *flg, *res;
			switch (size_mode)
			{
			case SIZE8: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i9 = OR(SHL(ZEXTs(9, val), CONSTs(9, 1)), TRUNCs(9, SHR(LD_CF(), CONST32(31))));
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(9), (std::vector<Value *> { val, val, TRUNCs(9, count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(TRUNC8(count), CONST8(1))), CONST8(1)));
				Value *of = ZEXT32(XOR(AND(rotr, CONSTs(9, 1 << 8)), SHL(AND(rotr, CONSTs(9, 1 << 7)), CONSTs(9, 1))));
				flg = OR(SHL(cf, CONST32(31)), SHL(XOR(of, SHL(cf, CONST32(8))), CONST32(22)));
				res = TRUNC8(SHR(val, CONSTs(9, 1)));
			}
			break;

			case SIZE16: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i17 = OR(SHL(ZEXTs(17, val), CONSTs(17, 1)), TRUNCs(17, SHR(LD_CF(), CONST32(31))));
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(17), (std::vector<Value *> { val, val, TRUNCs(17, count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(TRUNC16(count), CONST16(1))), CONST16(1)));
				Value *of = ZEXT32(XOR(AND(rotr, CONSTs(17, 1 << 16)), SHL(AND(rotr, CONSTs(17, 1 << 15)), CONSTs(17, 1))));
				flg = OR(SHL(cf, CONST32(31)), SHL(XOR(of, SHL(cf, CONST32(16))), CONST32(14)));
				res = TRUNC16(SHR(val, CONSTs(17, 1)));
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *i33 = OR(SHL(ZEXTs(33, val), CONSTs(33, 1)), SHL(ZEXTs(33, LD_CF()), CONSTs(33, 31)));
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(33), (std::vector<Value *> { val, val, ZEXTs(33, count) }));
				Value *cf = AND(SHR(val, SUB(count, CONST32(1))), CONST32(1));
				Value *of = TRUNC32(XOR(SHR(AND(rotr, CONSTs(33, 1ULL << 32)), CONSTs(33, 1)), AND(rotr, CONSTs(33, 1 << 31))));
				flg = OR(SHL(cf, CONST32(31)), SHR(XOR(of, SHL(cf, CONST32(31))), CONST32(1)));
				res = TRUNC32(SHR(val, CONSTs(33, 1)));
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(res, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, res);
			}
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), flg));
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_RDMSR: {
			if (cpu_ctx->hflags & HFLG_CPL) {
				RAISEin0(EXP_GP);
				translate_next = 0;
			}
			else {
				Function *msr_r_fn = cast<Function>(cpu->mod->getOrInsertFunction("msr_read_helper", getVoidType(), cpu->ptr_cpu_ctx->getType()).getCallee());
				CallInst::Create(msr_r_fn, cpu->ptr_cpu_ctx, "", cpu->bb);
			}
		}
		break;

		case ZYDIS_MNEMONIC_RDPMC:       BAD;
		case ZYDIS_MNEMONIC_RDTSC: {
			if (cpu_ctx->hflags & HFLG_CPL) {
				std::vector<BasicBlock *>vec_bb = getBBs(2);
				BR_COND(vec_bb[0], vec_bb[1], ICMP_NE(AND(LD_R32(CR4_idx), CONST32(CR4_TSD_MASK)), CONST32(0)));
				cpu->bb = vec_bb[0];
				RAISEin0(EXP_GP);
				UNREACH();
				cpu->bb = vec_bb[1];
			}

			Function *rdtsc_fn = cast<Function>(cpu->mod->getOrInsertFunction("cpu_rdtsc_handler", getVoidType(), cpu->ptr_cpu_ctx->getType()).getCallee());
			CallInst::Create(rdtsc_fn, cpu->ptr_cpu_ctx, "", cpu->bb);
		}
		break;

		case ZYDIS_MNEMONIC_ROL: {
			assert(instr.raw.modrm.reg == 0);

			Value *count;
			switch (instr.opcode)
			{
			case 0xD0:
				count = CONST32(1);
				size_mode = SIZE8;
				break;

			case 0xD2:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				size_mode = SIZE8;
				break;

			case 0xC0:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				size_mode = SIZE8;
				break;

			case 0xD1:
				count = CONST32(1);
				break;

			case 0xD3:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xC1:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];
			Value *val, *rm, *flg, *res;
			switch (size_mode)
			{
			case SIZE8: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(8), (std::vector<Value *> { val, val, TRUNC8(count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(CONST8(8), TRUNC8(count))), CONST8(1)));
				Value *of = ZEXT32(AND(rotl, CONST8(1 << 7)));
				flg = OR(SHL(cf, CONST32(31)), SHL(of, CONST32(23)));
				res = rotl;
			}
			break;

			case SIZE16: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(16), (std::vector<Value *> { val, val, TRUNC16(count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(CONST16(16), TRUNC16(count))), CONST16(1)));
				Value *of = ZEXT32(AND(rotl, CONST16(1 << 15)));
				flg = OR(SHL(cf, CONST32(31)), SHL(of, CONST32(15)));
				res = rotl;
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotl = INTRINSIC_ty(fshl, getIntegerType(32), (std::vector<Value *> { val, val, count }));
				Value *cf = AND(SHR(val, SUB(CONST32(32), count)), CONST32(1));
				Value *of = AND(rotl, CONST32(1 << 31));
				flg = OR(SHL(cf, CONST32(31)), SHR(of, CONST32(1)));
				res = rotl;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(res, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, res);
			}
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), flg));
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_ROR: {
			assert(instr.raw.modrm.reg == 1);

			Value *count;
			switch (instr.opcode)
			{
			case 0xD0:
				count = CONST32(1);
				size_mode = SIZE8;
				break;

			case 0xD2:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				size_mode = SIZE8;
				break;

			case 0xC0:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				size_mode = SIZE8;
				break;

			case 0xD1:
				count = CONST32(1);
				break;

			case 0xD3:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xC1:
				count = CONST32(instr.operands[OPNUM_SRC].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];
			Value *val, *rm, *flg, *res;
			switch (size_mode)
			{
			case SIZE8: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(8), (std::vector<Value *> { val, val, TRUNC8(count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(TRUNC8(count), CONST8(1))), CONST8(1)));
				Value *of = ZEXT32(XOR(AND(rotr, CONST8(1 << 7)), SHL(AND(rotr, CONST8(1 << 6)), CONST8(1))));
				flg = OR(SHL(cf, CONST32(31)), SHL(XOR(of, SHL(cf, CONST32(7))), CONST32(23)));
				res = rotr;
			}
			break;

			case SIZE16: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(16), (std::vector<Value *> { val, val, TRUNC16(count) }));
				Value *cf = ZEXT32(AND(SHR(val, SUB(TRUNC16(count), CONST16(1))), CONST16(1)));
				Value *of = ZEXT32(XOR(AND(rotr, CONST16(1 << 15)), SHL(AND(rotr, CONST16(1 << 14)), CONST16(1))));
				flg = OR(SHL(cf, CONST32(31)), SHL(XOR(of, SHL(cf, CONST32(15))), CONST32(15)));
				res = rotr;
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				Value *rotr = INTRINSIC_ty(fshr, getIntegerType(32), (std::vector<Value *> { val, val, count }));
				Value *cf = AND(SHR(val, SUB(count, CONST32(1))), CONST32(1));
				Value *of = XOR(SHR(AND(rotr, CONST32(1 << 31)), CONST32(1)), AND(rotr, CONST32(1 << 30)));
				flg = OR(SHL(cf, CONST32(31)), XOR(of, SHL(cf, CONST32(30))));
				res = rotr;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(res, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, res);
			}
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), flg));
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_RSM:         BAD;
		case ZYDIS_MNEMONIC_SETB:
		case ZYDIS_MNEMONIC_SETBE:
		case ZYDIS_MNEMONIC_SETL:
		case ZYDIS_MNEMONIC_SETLE:
		case ZYDIS_MNEMONIC_SETNB:
		case ZYDIS_MNEMONIC_SETNBE:
		case ZYDIS_MNEMONIC_SETNL:
		case ZYDIS_MNEMONIC_SETNLE:
		case ZYDIS_MNEMONIC_SETNO:
		case ZYDIS_MNEMONIC_SETNP:
		case ZYDIS_MNEMONIC_SETNS:
		case ZYDIS_MNEMONIC_SETNZ:
		case ZYDIS_MNEMONIC_SETO:
		case ZYDIS_MNEMONIC_SETP:
		case ZYDIS_MNEMONIC_SETS:
		case ZYDIS_MNEMONIC_SETZ: {
			Value *val;
			switch (instr.opcode)
			{
			case 0x90:
				val = ICMP_NE(LD_OF(), CONST32(0)); // OF != 0
				break;

			case 0x91:
				val = ICMP_EQ(LD_OF(), CONST32(0)); // OF == 0
				break;

			case 0x92:
				val = ICMP_NE(LD_CF(), CONST32(0)); // CF != 0
				break;

			case 0x93:
				val = ICMP_EQ(LD_CF(), CONST32(0)); // CF == 0
				break;

			case 0x94:
				val = ICMP_EQ(LD_ZF(), CONST32(0)); // ZF != 0
				break;

			case 0x95:
				val = ICMP_NE(LD_ZF(), CONST32(0)); // ZF == 0
				break;

			case 0x96:
				val = OR(ICMP_NE(LD_CF(), CONST32(0)), ICMP_EQ(LD_ZF(), CONST32(0))); // CF != 0 OR ZF != 0
				break;

			case 0x97:
				val = AND(ICMP_EQ(LD_CF(), CONST32(0)), ICMP_NE(LD_ZF(), CONST32(0))); // CF == 0 AND ZF == 0
				break;

			case 0x98:
				val = ICMP_NE(LD_SF(), CONST32(0)); // SF != 0
				break;

			case 0x99:
				val = ICMP_EQ(LD_SF(), CONST32(0)); // SF == 0
				break;

			case 0x9A:
				val = ICMP_EQ(LD_PF(), CONST8(0)); // PF != 0
				break;

			case 0x9B:
				val = ICMP_NE(LD_PF(), CONST8(0)); // PF == 0
				break;

			case 0x9C:
				val = ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF != OF
				break;

			case 0x9D:
				val = ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF == OF
				break;

			case 0x9E:
				val = OR(ICMP_EQ(LD_ZF(), CONST32(0)), ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF != 0 OR SF != OF
				break;

			case 0x9F:
				val = AND(ICMP_NE(LD_ZF(), CONST32(0)), ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF == 0 AND SF == OF
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(3);
			Value *rm, *byte = ALLOC8();
			BR_COND(vec_bb[0], vec_bb[1], val);
			cpu->bb = vec_bb[0];
			ST(byte, CONST8(1));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[1];
			ST(byte, CONST8(0));
			BR_UNCOND(vec_bb[2]);
			cpu->bb = vec_bb[2];
			GET_RM(OPNUM_SINGLE, ST_REG_val(LD(byte, getIntegerType(8)), rm);, ST_MEM(MEM_LD8_idx, rm, LD(byte, getIntegerType(8))););
		}
		break;

		case ZYDIS_MNEMONIC_SGDT:        BAD;

		case ZYDIS_MNEMONIC_SHLD: {
			Value *count;
			switch (instr.opcode)
			{
			case 0xA5:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xA4:
				count = CONST32(instr.operands[OPNUM_THIRD].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			Value *dst, *src, *rm, *flg, *val;
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];

			switch (size_mode)
			{
			case SIZE16: {
				BasicBlock *bb = BasicBlock::Create(CTX(), "", cpu->bb->getParent(), 0);
				BR_COND(vec_bb[0], bb, ICMP_UGT(count, CONST32(16)));
				cpu->bb = bb;
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				val = TRUNC16(SHR(SHL(OR(SHL(ZEXT32(dst), CONST32(16)), ZEXT32(src)), count), CONST32(16)));
				Value *cf = SHL(AND(ZEXT32(dst), SHL(CONST32(1), SUB(CONST32(16), count))), ADD(CONST32(15), count));
				Value *of = SHL(ZEXT32(XOR(AND(dst, CONST16(1 << 15)), AND(val, CONST16(1 << 15)))), CONST32(15));
				flg = OR(cf, XOR(SHR(cf, CONST32(1)), of));
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				val = TRUNC32(SHR(SHL(OR(SHL(ZEXT64(dst), CONST64(32)), ZEXT64(src)), ZEXT64(count)), CONST64(32)));
				Value *cf = SHL(AND(dst, SHL(CONST32(1), SUB(CONST32(32), count))), SUB(count, CONST32(1)));
				Value *of = SHR(XOR(AND(dst, CONST32(1 << 31)), AND(val, CONST32(1 << 31))), CONST32(1));
				flg = OR(cf, XOR(SHR(cf, CONST32(1)), of));
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(val, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, val);
			}
			SET_FLG(val, flg);
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_SHRD: {
			Value *count;
			switch (instr.opcode)
			{
			case 0xAD:
				count = ZEXT32(AND(LD_R8L(ECX_idx), CONST8(0x1F)));
				break;

			case 0xAC:
				count = CONST32(instr.operands[OPNUM_THIRD].imm.value.u & 0x1F);
				break;

			default:
				LIB86CPU_ABORT();
			}

			std::vector<BasicBlock *>vec_bb = getBBs(2);
			Value *dst, *src, *rm, *flg, *val;
			BR_COND(vec_bb[0], vec_bb[1], ICMP_EQ(count, CONST32(0)));
			cpu->bb = vec_bb[1];

			switch (size_mode)
			{
			case SIZE16: {
				BasicBlock *bb = BasicBlock::Create(CTX(), "", cpu->bb->getParent(), 0);
				BR_COND(vec_bb[0], bb, ICMP_UGT(count, CONST32(16)));
				cpu->bb = bb;
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				val = TRUNC16(SHR(OR(SHL(ZEXT32(src), CONST32(16)), ZEXT32(dst)), count));
				Value *cf = SHL(AND(ZEXT32(dst), SHL(CONST32(1), SUB(count, CONST32(1)))), SUB(CONST32(32), count));
				Value *of = SHL(ZEXT32(XOR(AND(dst, CONST16(1 << 15)), AND(val, CONST16(1 << 15)))), CONST32(15));
				flg = OR(cf, XOR(SHR(cf, CONST32(1)), of));
			}
			break;

			case SIZE32: {
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm);, dst = LD_MEM(fn_idx[size_mode], rm););
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				val = TRUNC32(SHR(OR(SHL(ZEXT64(src), CONST64(32)), ZEXT64(dst)), ZEXT64(count)));
				Value *cf = SHL(AND(dst, SHL(CONST32(1), SUB(count, CONST32(1)))), SUB(CONST32(32), count));
				Value *of = SHR(XOR(AND(dst, CONST32(1 << 31)), AND(val, CONST32(1 << 31))), CONST32(1));
				flg = OR(cf, XOR(SHR(cf, CONST32(1)), of));
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			if (instr.operands[OPNUM_DST].type == ZYDIS_OPERAND_TYPE_REGISTER) {
				ST_REG_val(val, rm);
			}
			else {
				ST_MEM(fn_idx[size_mode], rm, val);
			}
			SET_FLG(val, flg);
			BR_UNCOND(vec_bb[0]);
			cpu->bb = vec_bb[0];
		}
		break;

		case ZYDIS_MNEMONIC_SIDT:        BAD;
		case ZYDIS_MNEMONIC_SLDT:        BAD;
		case ZYDIS_MNEMONIC_SMSW:        BAD;
		case ZYDIS_MNEMONIC_STR:         BAD;
		case ZYDIS_MNEMONIC_SYSENTER:    BAD;
		case ZYDIS_MNEMONIC_SYSEXIT:     BAD;
		case ZYDIS_MNEMONIC_UD1:         BAD;
		case ZYDIS_MNEMONIC_UD2:         BAD;
		case ZYDIS_MNEMONIC_WBINVD:      BAD;
		case ZYDIS_MNEMONIC_WRMSR:       BAD;
		case ZYDIS_MNEMONIC_XADD:        BAD;
		case ZYDIS_MNEMONIC_XLAT:        BAD;

#endif
		default:
			LIB86CPU_ABORT();
		}

		cpu->virt_pc += cpu->instr_bytes;
		cpu->tc->size += cpu->instr_bytes;

	} while ((cpu->translate_next | (disas_ctx->flags & (DISAS_FLG_PAGE_CROSS | DISAS_FLG_ONE_INSTR))) == 1);
}

static translated_code_t *
cpu_dbg_int(cpu_ctx_t *cpu_ctx)
{
	// this is called when the user closes the debugger window
	throw lc86_exp_abort("The debugger was closed", lc86_status::success);
}

static translated_code_t *
cpu_do_int(cpu_ctx_t *cpu_ctx)
{
	// hw interrupts not implemented yet
	throw lc86_exp_abort("Hardware interrupts are not implemented yet", lc86_status::internal_error);
}

// forward declare for cpu_main_loop
translated_code_t *tc_run_code(cpu_ctx_t *cpu_ctx, translated_code_t *tc);

template<bool is_tramp>
void cpu_suppress_trampolines(cpu_t *cpu)
{
	if constexpr (is_tramp) {
		// we need to remove the HFLG_TRAMP after we have searched the tc cache, but before executing the guest code, so that successive tc's
		// can still call hooks, if the trampolined function happens to make calls to other hooked functions internally
		cpu->cpu_ctx.hflags &= ~HFLG_TRAMP;
	}
}

template<bool is_tramp, bool is_trap, typename T>
void cpu_main_loop(cpu_t *cpu, T &&lambda)
{
	translated_code_t *prev_tc = nullptr, *ptr_tc = nullptr;
	addr_t virt_pc, pc;

	// main cpu loop
	while (lambda()) {

		retry:
		try {
			virt_pc = get_pc(&cpu->cpu_ctx);
			cpu_check_data_watchpoints(cpu, virt_pc, 1, DR7_TYPE_INSTR, cpu->cpu_ctx.regs.eip);
			pc = get_code_addr(cpu, virt_pc, cpu->cpu_ctx.regs.eip);
		}
		catch (host_exp_t type) {
			assert((type == host_exp_t::pf_exp) || (type == host_exp_t::de_exp));
			cpu_suppress_trampolines<is_tramp>(cpu);

			// this is either a page fault or a debug exception. In both cases, we have to call the exception handler
			retry_exp:
			try {
				// the exception handler always returns nullptr
				prev_tc = cpu_raise_exception(&cpu->cpu_ctx);
			}
			catch (host_exp_t type) {
				assert((type == host_exp_t::pf_exp) || (type == host_exp_t::de_exp));

				// page fault or debug exception while delivering another exception
				goto retry_exp;
			}

			goto retry;
		}

		if constexpr (!is_trap) {
			// if we are executing a trapped instr, we must always emit a new tc to run it and not consider other tc's in the cache. Doing so avoids having to invalidate
			// the tc in the cache that contains the trapped instr
			ptr_tc = tc_cache_search(cpu, pc);
		}

		if (ptr_tc == nullptr) {

			// code block for this pc not present, we need to translate new code
			std::unique_ptr<translated_code_t> tc(new translated_code_t);

			cpu->tc = tc.get();
			cpu->jit->gen_tc_prologue();

			// prepare the disas ctx
			disas_ctx_t disas_ctx{};
			disas_ctx.flags = ((cpu->cpu_ctx.hflags & HFLG_CS32) >> CS32_SHIFT) |
				((cpu->cpu_ctx.hflags & HFLG_PE_MODE) >> (PE_MODE_SHIFT - 1)) |
				(cpu->cpu_flags & CPU_DISAS_ONE) |
				((cpu->cpu_flags & CPU_SINGLE_STEP) >> 3) |
				((cpu->cpu_ctx.regs.eflags & RF_MASK) >> 9) | // if rf is set, we need to clear it after the first instr executed
				((cpu->cpu_ctx.regs.eflags & TF_MASK) >> 1); // if tf is set, we need to raise a DB exp after every instruction
			disas_ctx.virt_pc = virt_pc;
			disas_ctx.pc = pc;

			if constexpr (!is_trap) {
				const auto it = cpu->hook_map.find(disas_ctx.virt_pc);
				bool take_hook;
				if constexpr (is_tramp) {
					take_hook = (it != cpu->hook_map.end()) && !(cpu->cpu_ctx.hflags & HFLG_TRAMP);
				}
				else {
					take_hook = it != cpu->hook_map.end();
				}

				if (take_hook) {
					cpu->instr_eip = disas_ctx.virt_pc - cpu->cpu_ctx.regs.cs_hidden.base;
					//hook_emit(cpu, it->second.get());
				}
				else {
					// start guest code translation
					cpu_translate(cpu, &disas_ctx);
				}
			}
			else {
				// don't take hooks if we are executing a trapped instr. Otherwise, if the trapped instr is also hooked, we will take the hook instead of executing it
				cpu_translate(cpu, &disas_ctx);
				//raise_exp_inline_emit(cpu, CONST32(0), CONST16(0), CONST16(EXP_DB), LD_R32(EIP_idx));
			}

			cpu->jit->gen_tc_epilogue();

			cpu->tc->pc = pc;
			cpu->tc->virt_pc = virt_pc;
			cpu->tc->cs_base = cpu->cpu_ctx.regs.cs_hidden.base;
			cpu->tc->cpu_flags = (cpu->cpu_ctx.hflags & HFLG_CONST) | (cpu->cpu_ctx.regs.eflags & EFLAGS_CONST);
			cpu->jit->gen_code_block();
			cpu->tc->jmp_offset[3] = &cpu_dbg_int;
			cpu->tc->jmp_offset[4] = &cpu_do_int;

			// we are done with code generation for this block, so we null the tc and bb pointers to prevent accidental usage
			ptr_tc = cpu->tc;
			cpu->tc = nullptr;

			if (disas_ctx.flags & (DISAS_FLG_PAGE_CROSS | DISAS_FLG_ONE_INSTR)) {
				if (cpu->cpu_flags & CPU_FORCE_INSERT) {
					if ((cpu->num_tc) == CODE_CACHE_MAX_SIZE) {
						tc_cache_purge(cpu);
						prev_tc = nullptr;
					}
					tc_cache_insert(cpu, pc, std::move(tc));
				}

				cpu_suppress_trampolines<is_tramp>(cpu);
				cpu->cpu_flags &= ~(CPU_DISAS_ONE | CPU_ALLOW_CODE_WRITE | CPU_FORCE_INSERT);
				tc_run_code(&cpu->cpu_ctx, ptr_tc);
				prev_tc = nullptr;
				continue;
			}
			else {
				if ((cpu->num_tc) == CODE_CACHE_MAX_SIZE) {
					tc_cache_purge(cpu);
					prev_tc = nullptr;
				}
				tc_cache_insert(cpu, pc, std::move(tc));
			}
		}

		cpu_suppress_trampolines<is_tramp>(cpu);

		// see if we can link the previous tc with the current one
		if (prev_tc != nullptr) {
			switch (prev_tc->flags & TC_FLG_LINK_MASK)
			{
			case 0:
				break;

			case TC_FLG_DST_ONLY:
			case TC_FLG_COND_DST_ONLY:
				tc_link_dst_only(prev_tc, ptr_tc);
				break;

			case TC_FLG_DIRECT:
				tc_link_direct(prev_tc, ptr_tc);
				break;

			case TC_FLG_RET:
			case TC_FLG_INDIRECT:
				cpu->ibtc.insert_or_assign(virt_pc, ptr_tc);
				break;

			default:
				LIB86CPU_ABORT();
			}
		}

		prev_tc = tc_run_code(&cpu->cpu_ctx, ptr_tc);
	}
}

translated_code_t *
tc_run_code(cpu_ctx_t *cpu_ctx, translated_code_t *tc)
{
	try {
		// run the translated code
		return tc->ptr_code(cpu_ctx);
	}
	catch (host_exp_t type) {
		switch (type)
		{
		case host_exp_t::pf_exp: {
			// page fault while excecuting the translated code
			retry_exp:
			try {
				// the exception handler always returns nullptr
				return cpu_raise_exception(cpu_ctx);
			}
			catch (host_exp_t type) {
				assert((type == host_exp_t::pf_exp) || (type == host_exp_t::de_exp));

				// page fault or debug exception while delivering another exception
				goto retry_exp;
			}
		}
		break;

		case host_exp_t::de_exp: {
			// debug exception trap (mem/io r/w watch) while excecuting the translated code.
			// We set CPU_DBG_TRAP, so that we can execute the trapped instruction without triggering again a de exp,
			// and then jump to the debug handler. Note thate eip points to the trapped instr, so we can execute it.
			assert(cpu_ctx->exp_info.exp_data.idx == EXP_DB);

			cpu_ctx->cpu->cpu_flags |= CPU_DISAS_ONE;
			cpu_ctx->hflags |= HFLG_DBG_TRAP;
			cpu_ctx->regs.eip = cpu_ctx->exp_info.exp_data.eip;
			// run the main loop only once, since we only execute the trapped instr
			int i = 0;
			cpu_main_loop<false, true>(cpu_ctx->cpu, [&i]() { return i++ == 0; });
			return nullptr;
		}

		case host_exp_t::cpu_mode_changed:
			tc_cache_purge(cpu_ctx->cpu);
			[[fallthrough]];

		case host_exp_t::halt_tc:
			return nullptr;

		default:
			LIB86CPU_ABORT_msg("Unknown host exception in %s", __func__);
		}
	}
}

lc86_status
cpu_start(cpu_t *cpu)
{
	if (cpu->cpu_flags & CPU_DBG_PRESENT) {
		std::promise<bool> promise;
		std::future<bool> fut = promise.get_future();
		std::thread(dbg_main_wnd, cpu, std::ref(promise)).detach();
		bool has_err = fut.get();
		if (has_err) {
			return lc86_status::internal_error;
		}
		// wait until the debugger continues execution, so that users have a chance to set breakpoints and/or inspect the guest code
		guest_running.wait(false);
	}

	try {
		cpu_main_loop<false, false>(cpu, []() { return true; });
	}
	catch (lc86_exp_abort &exp) {
		if (cpu->cpu_flags & CPU_DBG_PRESENT) {
			dbg_should_close();
		}

		last_error = exp.what();
		return exp.get_code();
	}

	assert(0);

	return set_last_error(lc86_status::internal_error);
}

void
cpu_exec_trampoline(cpu_t *cpu, const uint32_t ret_eip)
{
	// set the trampoline flag, so that we can call the trampoline tc instead of the hook tc
	cpu->cpu_ctx.hflags |= HFLG_TRAMP;
	cpu_main_loop<true, false>(cpu, [cpu, ret_eip]() { return cpu->cpu_ctx.regs.eip != ret_eip; });
}

template translated_code_t *cpu_raise_exception<true>(cpu_ctx_t *cpu_ctx);
template translated_code_t *cpu_raise_exception<false>(cpu_ctx_t *cpu_ctx);
