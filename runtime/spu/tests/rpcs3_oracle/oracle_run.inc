/* included at the end of the generated oracle TU */
#include "oracle.h"
namespace utils { bool has_avx() { return false; } u64 _get_main_tid() { return 0; } }
static spu_thread g_o_spu;
static uint8_t g_o_ls[0x40000], g_o_ls0[0x40000];
static const spu_decoder<oracle_ops> g_o_dec;
namespace fmt { [[noreturn]] void raw_throw_exception(std::source_location, const char*, const fmt_type_info*, const u64*) { throw std::runtime_error("fmt"); } }

extern "C" void rpcs3_oracle_set_ls(const uint8_t* ls) { memcpy(g_o_ls, ls, sizeof g_o_ls); memcpy(g_o_ls0, ls, sizeof g_o_ls0); }
extern "C" void rpcs3_oracle_poke(uint32_t a, const uint8_t* p, uint32_t n) { memcpy(g_o_ls + a, p, n); memcpy(g_o_ls0 + a, p, n); }

extern "C" int rpcs3_oracle_run(uint32_t insn, uint32_t pc, const uint8_t gpr[128][16], uint32_t srr0,
                                uint32_t ch_in, uint32_t ch_cnt, uint32_t events, oracle_result* out)
{
	spu_thread& s = g_o_spu;
	memset(out, 0, sizeof *out);
	for (int r = 0; r < 128; r++) for (int k = 0; k < 16; k++) reinterpret_cast<u8*>(&s.gpr[r])[k] = gpr[r][15 - k];
	s.fpscr.Reset(); s.pc = pc; s.srr0 = srr0; s.ls = g_o_ls; s.state = 0; s.last_ref = ~0u;
	s.halted = s.stopped = 0; s.stop_code = 0; s.ch_ops = 0; s.ch_no = 0; s.ch_out_val = 0;
	s.ch_in_val = ch_in; s.ch_cnt_val = ch_cnt; s.events = events;
	fesetround(FE_TONEAREST);
	spu_opcode_t op; op.opcode = insn;
	bool adv;
	try { adv = g_o_dec.decode(insn)(s, op); }
	catch (...) { fesetround(FE_TONEAREST); return 0; }
	fesetround(FE_TONEAREST);
	out->ok = 1;
	out->next_pc = adv ? ((pc + 4) & 0x3FFFC) : (s.pc & 0x3FFFC);
	out->halted = s.halted; out->stopped = s.stopped; out->stop_code = s.stop_code;
	out->ch_ops = s.ch_ops; out->ch_no = s.ch_no; out->ch_out = s.ch_out_val;
	for (int r = 0; r < 128; r++) for (int k = 0; k < 16; k++) out->gpr[r][k] = reinterpret_cast<u8*>(&s.gpr[r])[15 - k];
	if (s.last_ref != ~0u) {
		out->mem = 1; out->mem_addr = s.last_ref;
		memcpy(out->mem_quad, g_o_ls + s.last_ref, 16);
		memcpy(g_o_ls + s.last_ref, g_o_ls0 + s.last_ref, 16);   /* undo the store */
	}
	return 1;
}
