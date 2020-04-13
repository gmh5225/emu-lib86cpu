/*
 * lib86cpu types
 *
 * ergo720                Copyright (c) 2019
 * the libcpu developers  Copyright (c) 2009-2010
 */

#pragma once

#include "config.h"
#include "platform.h"
#include <stdint.h>
#include <forward_list>
#include "types.h"
#include "interval_tree.h"
#include <unordered_set>


namespace llvm {
	class LLVMContext;
	class BasicBlock;
	class Function;
	class Module;
	class PointerType;
	class StructType;
	class Value;
	class DataLayout;
	class GlobalVariable;
	namespace orc {
		class LLJIT;
	}
}

using namespace llvm;

// lib86cpu error flags
enum lib86cpu_status {
	LIB86CPU_NO_MEMORY = -4,
	LIB86CPU_INVALID_PARAMETER,
	LIB86CPU_LLVM_ERROR,
	LIB86CPU_OP_NOT_IMPLEMENTED,
	LIB86CPU_SUCCESS,
};

#define LIB86CPU_CHECK_SUCCESS(status) (static_cast<lib86cpu_status>(status) == 0)

#define CPU_FLAG_SWAPMEM        (1 << 0)
#define CPU_INTEL_SYNTAX        (1 << 1)
#define CPU_FLAG_FP80           (1 << 2)
#define CPU_CODEGEN_OPTIMIZE    (1 << 3)
#define CPU_PRINT_IR            (1 << 4)
#define CPU_PRINT_IR_OPTIMIZED  (1 << 5)

#define CPU_INTEL_SYNTAX_SHIFT  1

#define CPU_NUM_REGS 33

#define CODE_CACHE_MAX_SIZE (1 << 15)
#define TLB_MAX_SIZE (1 << 20)

#ifdef DEBUG_LOG
#define LOG(...) do { printf(__VA_ARGS__); } while(0)
#else
#define LOG(...)
#endif

#define LIB86CPU_ABORT() \
do {\
    printf("%s:%d: lib86cpu fatal error in function %s\n", __FILE__, __LINE__, __func__);\
    exit(1);\
} while (0)

#define LIB86CPU_ABORT_msg(...) \
do {\
    printf(__VA_ARGS__);\
    exit(1);\
} while (0)

// used to generate the parity table
// borrowed from Bit Twiddling Hacks by Sean Eron Anderson (public domain)
// http://graphics.stanford.edu/~seander/bithacks.html#ParityLookupTable
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
#define GEN_TABLE P6(0), P6(1), P6(1), P6(0)

// mmio/pmio access handlers
typedef uint32_t  (*fp_read)(addr_t addr, size_t size, void *opaque);
typedef void      (*fp_write)(addr_t addr, size_t size, uint32_t value, void *opaque);

// memory region type
enum mem_type_t {
	MEM_UNMAPPED,
	MEM_RAM,
	MEM_MMIO,
	MEM_PMIO,
	MEM_ALIAS,
};

template<typename T>
struct memory_region_t {
	T start;
	T end;
	int type;
	int priority;
	fp_read read_handler;
	fp_write write_handler;
	void *opaque;
	addr_t alias_offset;
	memory_region_t<T> *aliased_region;
	memory_region_t() : start(0), end(0), alias_offset(0), type(MEM_UNMAPPED), priority(0), read_handler(nullptr), write_handler(nullptr),
		opaque(nullptr), aliased_region(nullptr) {};
};

template<typename T>
struct sort_by_priority
{
	bool operator() (const std::reference_wrapper<std::unique_ptr<memory_region_t<T>>> &lhs, const std::reference_wrapper<std::unique_ptr<memory_region_t<T>>> &rhs) const
	{
		return lhs.get()->priority > rhs.get()->priority;
	}
};

struct exp_data_t {
	uint32_t fault_addr;    // only used during page faults
	uint16_t code;          // error code used by the exception (if any)
	uint16_t idx;           // index number of the exception
	uint32_t eip;           // eip of the instr that generated the exception
};

struct exp_info_t {
	exp_data_t exp_data;
	uint8_t exp_in_flight;  // one when servicing an exception, zero otherwise
};

// forward declare
struct translated_code_t;
struct cpu_ctx_t;
using entry_t = translated_code_t *(*)(cpu_ctx_t *cpu_ctx);
using raise_exp_t = translated_code_t *(*)(cpu_ctx_t *cpu_ctx, exp_data_t *exp_data);

struct translated_code_ctx_t {
	addr_t cs_base;
	addr_t pc;
	uint32_t cpu_flags;
	entry_t ptr_code;
	entry_t jmp_offset[3];
	uint32_t flags;
	uint32_t size;
};

struct translated_code_t {
	std::forward_list<translated_code_t *> linked_tc;
	translated_code_ctx_t tc_ctx;
};

struct disas_ctx_t {
	uint8_t flags;
	addr_t virt_pc, start_pc, pc;
	addr_t instr_page_addr;
#if DEBUG_LOG
	uint8_t instr_bytes[15];
	uint8_t byte_idx;
#endif
};

struct regs_layout_t {
	const unsigned bits_size;
	const unsigned idx;
	const char *name;
};

struct lazy_eflags_t {
	uint32_t result;
	uint32_t auxbits;
	// returns 1 when parity is odd, 0 if even
	uint8_t parity[256] = { GEN_TABLE };
};

// forward declare
struct cpu_t;
// this struct should contain all cpu variables which need to be visible from the llvm generated code
struct cpu_ctx_t {
	cpu_t *cpu;
	regs_t regs;
	lazy_eflags_t lazy_eflags;
	uint32_t hflags;
	uint32_t tlb[TLB_MAX_SIZE];
	uint8_t *ram;
	raise_exp_t exp_fn;
	exp_info_t *ptr_exp_info;
};

struct cpu_t {
	uint32_t cpu_flags;
	const char *cpu_name;
	const regs_layout_t *regs_layout;
	cpu_ctx_t cpu_ctx;
	memory_region_t<addr_t> *pt_mr;
	translated_code_t *tc; // tc for which we are currently generating code
	std::unique_ptr<interval_tree<addr_t, std::unique_ptr<memory_region_t<addr_t>>>> memory_space_tree;
	std::unique_ptr<interval_tree<port_t, std::unique_ptr<memory_region_t<port_t>>>> io_space_tree;
	std::set<std::reference_wrapper<std::unique_ptr<memory_region_t<addr_t>>>, sort_by_priority<addr_t>> memory_out;
	std::set<std::reference_wrapper<std::unique_ptr<memory_region_t<port_t>>>, sort_by_priority<port_t>> io_out;
	std::forward_list<std::unique_ptr<translated_code_t>> code_cache[CODE_CACHE_MAX_SIZE];
	std::unordered_map<uint32_t, std::unordered_set<translated_code_t *>> tc_page_map;
	uint16_t num_tc;
	exp_info_t exp_info;

	// llvm specific variables
	std::unique_ptr<orc::LLJIT> jit;
	DataLayout *dl;
	LLVMContext *ctx;
	Module *mod;
	Value *ptr_cpu_ctx;
	Value *ptr_regs;
	Value *ptr_eflags;
	Value *ptr_hflags;
	Value *ptr_tlb;
	Value *ptr_ram;
	Value *ptr_exp_fn;
	Value *ptr_invtc_fn;
	Value *instr_eip;
	BasicBlock *bb; // bb to which we are currently adding llvm instructions
	Function *ptr_mem_ldfn[7];
	Function *ptr_mem_stfn[7];
	GlobalVariable *exp_data;
};

// cpu api
API_FUNC lib86cpu_status cpu_new(size_t ramsize, cpu_t *&out);
API_FUNC void cpu_free(cpu_t *cpu);
API_FUNC lib86cpu_status cpu_run(cpu_t *cpu);

// memory api
API_FUNC lib86cpu_status memory_init_region_ram(cpu_t *cpu, addr_t start, size_t size, int priority);
API_FUNC lib86cpu_status memory_init_region_io(cpu_t *cpu, addr_t start, size_t size, bool io_space, fp_read read_func, fp_write write_func, void *opaque, int priority);
API_FUNC lib86cpu_status memory_init_region_alias(cpu_t *cpu, addr_t alias_start, addr_t ori_start, size_t ori_size, int priority);
API_FUNC lib86cpu_status memory_destroy_region(cpu_t *cpu, addr_t start, size_t size, bool io_space);
