/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2021  Marcelina Kościelnicka <mwk@0x04.net>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <functional>
#include <algorithm>

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/mem.h"
#include "kernel/qcsat.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

enum class RamKind {
	Auto,
	Logic,
	NotLogic,
	Distributed,
	Block,
	Huge,
};

enum class MemoryInitKind {
	None,
	Zero,
	Any,
};

enum class PortKind {
	Sr,
	Ar,
	Sw,
	Srsw,
	Arsw,
};

enum class ClkPolKind {
	Anyedge,
	Posedge,
	Negedge,
};

enum class RdEnKind {
	None,
	Any,
	WriteImplies,
	WriteExcludes,
};

enum class ResetKind {
	Init,
	Async,
	Sync,
};

enum class ResetValKind {
	None,
	Zero,
	Named,
};

enum class SrstKind {
	SrstOverEn,
	EnOverSrst,
	Any,
};

enum class TransTargetKind {
	Self,
	Other,
	Named,
};

enum class TransKind {
	New,
	Old,
};

typedef dict<std::string, Const> Options;

struct Empty {};

struct ClockDef {
	ClkPolKind kind;
	std::string name;
};

struct ResetValDef {
	ResetKind kind;
	ResetValKind val_kind;
	std::string name;
};

struct WrTransDef {
	TransTargetKind target_kind;
	std::string target_name;
	TransKind kind;
};

template<typename T> struct Capability {
	T val;
	Options opts, portopts;

	Capability(T val, Options opts, Options portopts) : val(val), opts(opts), portopts(portopts) {}
};

template<typename T> using Caps = std::vector<Capability<T>>;

struct PortGroupDef {
	PortKind kind;
	std::vector<std::string> names;
	Caps<ClockDef> clock;
	Caps<int> width;
	Caps<Empty> mixwidth;
	Caps<Empty> addrce;
	Caps<RdEnKind> rden;
	Caps<ResetValDef> rdrstval;
	Caps<SrstKind> rdsrstmode;
	Caps<int> wrbe;
	Caps<std::string> wrprio;
	Caps<WrTransDef> wrtrans;
	Caps<int> wrcs;
};

struct MemoryDimsDef {
	int abits;
	int dbits;
};

struct RamDef {
	IdString id;
	RamKind kind;
	Caps<PortGroupDef> ports;
	Caps<MemoryDimsDef> dims;
	Caps<MemoryInitKind> init;
	Caps<std::string> style;
};

struct Library {
	std::vector<RamDef> ram_defs;
	const pool<std::string> defines;
	pool<std::string> defines_unused;

	Library(pool<std::string> defines) : defines(defines), defines_unused(defines) {}

	void prepare() {
		for (auto def: defines_unused) {
			log_warning("define %s not used in the library.\n", def.c_str());
		}
	}
};

struct Parser {
	std::string filename;
	std::ifstream infile;
	int line_number = 0;
	Library &lib;
	std::vector<std::string> tokens;
	int token_idx = 0;
	bool eof = false;

	std::vector<std::pair<std::string, Const>> option_stack;
	std::vector<std::pair<std::string, Const>> portoption_stack;
	RamDef ram;
	PortGroupDef port;
	bool active = true;

	Parser(std::string filename, Library &lib) : filename(filename), lib(lib) {
		// Note: this rewrites the filename we're opening, but not
		// the one we're storing — this is actually correct, so that
		// we keep the original filename for diagnostics.
		rewrite_filename(filename);
		infile.open(filename);
		if (infile.fail()) {
			log_error("failed to open %s\n", filename.c_str());
		}
		parse();
		infile.close();
	}

	std::string peek_token() {
		if (eof)
			return "";

		if (token_idx < GetSize(tokens))
			return tokens[token_idx];

		tokens.clear();
		token_idx = 0;

		std::string line;
		while (std::getline(infile, line)) {
			line_number++;
			for (string tok = next_token(line); !tok.empty(); tok = next_token(line)) {
				if (tok[0] == '#')
					break;
				if (tok[tok.size()-1] == ';') {
					tokens.push_back(tok.substr(0, tok.size()-1));
					tokens.push_back(";");
				} else {
					tokens.push_back(tok);
				}
			}
			if (!tokens.empty())
				return tokens[token_idx];
		}

		eof = true;
		return "";
	}

	std::string get_token() {
		std::string res = peek_token();
		if (!eof)
			token_idx++;
		return res;
	}

	IdString get_id() {
		std::string token = get_token();
		if (token.empty() || (token[0] != '$' && token[0] != '\\')) {
			log_error("%s:%d: expected id string, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return IdString(token);
	}

	std::string get_name() {
		std::string res = get_token();
		bool valid = true;
		// Basic sanity check.
		if (res.empty() || (!isalpha(res[0]) && res[0] != '_'))
			valid = false;
		for (char c: res)
			if (!isalnum(c) && c != '_')
				valid = false;
		if (!valid)
			log_error("%s:%d: expected name, got `%s`.\n", filename.c_str(), line_number, res.c_str());
		return res;
	}

	std::string get_string() {
		std::string token = get_token();
		if (token.size() < 2 || token[0] != '"' || token[token.size()-1] != '"') {
			log_error("%s:%d: expected string, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return token.substr(1, token.size()-2);
	}

	bool peek_string() {
		std::string token = peek_token();
		return !token.empty() && token[0] == '"';
	}

	int get_int() {
		std::string token = get_token();
		char *endptr;
		long res = strtol(token.c_str(), &endptr, 0);
		if (token.empty() || *endptr || res > INT_MAX) {
			log_error("%s:%d: expected int, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return res;
	}

	bool peek_int() {
		std::string token = peek_token();
		return !token.empty() && isdigit(token[0]);
	}

	void get_semi() {
		std::string token = get_token();
		if (token != ";") {
			log_error("%s:%d: expected `;`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	Const get_value() {
		std::string token = peek_token();
		if (!token.empty() && token[0] == '"') {
			std::string s = get_string();
			return Const(s);
		} else {
			return Const(get_int());
		}
	}

	bool enter_ifdef(bool polarity) {
		bool res = active;
		std::string name = get_name();
		lib.defines_unused.erase(name);
		if (lib.defines.count(name)) {
			active = polarity;
		} else {
			active = !polarity;
		}
		return res;
	}

	void enter_else(bool save) {
		get_token();
		active = !active && save;
	}

	void enter_option() {
		std::string name = get_string();
		Const val = get_value();
		option_stack.push_back({name, val});
	}

	void exit_option() {
		option_stack.pop_back();
	}

	Options get_options() {
		Options res;
		for (auto it: option_stack)
			res[it.first] = it.second;
		return res;
	}

	void enter_portoption() {
		std::string name = get_string();
		Const val = get_value();
		portoption_stack.push_back({name, val});
	}

	void exit_portoption() {
		portoption_stack.pop_back();
	}

	Options get_portoptions() {
		Options res;
		for (auto it: portoption_stack)
			res[it.first] = it.second;
		return res;
	}

	template<typename T> void add_cap(Caps<T> &caps, T val) {
		if (active)
			caps.push_back(Capability<T>(val, get_options(), get_portoptions()));
	}

	void parse_port_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_port_item();
			get_token();
		} else {
			parse_port_item();
		}
	}

	void parse_ram_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_ram_item();
			get_token();
		} else {
			parse_ram_item();
		}
	}

	void parse_top_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_top_item();
			get_token();
		} else {
			parse_top_item();
		}
	}

	void parse_port_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_port_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_port_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_port_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_port_block();
			}
			active = save;
		} else if (token == "option") {
			enter_option();
			parse_port_block();
			exit_option();
		} else if (token == "portoption") {
			enter_portoption();
			parse_port_block();
			exit_portoption();
		} else if (token == "clock") {
			if (port.kind == PortKind::Ar) {
				log_error("%s:%d: `clock` not allowed in async read port.\n", filename.c_str(), line_number);
			}
			ClockDef def;
			token = peek_token();
			if (token == "anyedge") {
				def.kind = ClkPolKind::Anyedge;
				get_token();
			} else if (token == "posedge") {
				def.kind = ClkPolKind::Posedge;
				get_token();
			} else if (token == "negedge") {
				def.kind = ClkPolKind::Negedge;
				get_token();
			} else {
				log_error("%s:%d: expected `posedge`, `negedge`, or `anyedge`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			token = peek_token();
			if (peek_string()) {
				def.name = get_string();
			}
			get_semi();
			add_cap(port.clock, def);
		} else if (token == "width") {
			do {
				add_cap(port.width, get_int());
			} while (peek_int());
			get_semi();
		} else if (token == "mixwidth") {
			get_semi();
			add_cap(port.mixwidth, {});
		} else if (token == "addrce") {
			get_semi();
			add_cap(port.addrce, {});
		} else if (token == "rden") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `rden` only allowed on sync read ports.\n", filename.c_str(), line_number);
			token = get_token();
			RdEnKind val;
			if (token == "none") {
				val = RdEnKind::None;
			} else if (token == "any") {
				val = RdEnKind::Any;
			} else if (token == "write-implies") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `write-implies` only makes sense for read+write ports.\n", filename.c_str(), line_number);
				val = RdEnKind::WriteImplies;
			} else if (token == "write-excludes") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `write-excludes` only makes sense for read+write ports.\n", filename.c_str(), line_number);
				val = RdEnKind::WriteExcludes;
			} else {
				log_error("%s:%d: expected `none`, `any`, `write-implies`, or `write-excludes`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			add_cap(port.rden, val);
		} else if (token == "rdinitval" || token == "rdsrstval" || token == "rdarstval") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `%s` only allowed on sync read ports.\n", filename.c_str(), line_number, token.c_str());
			ResetValDef def;
			if (token == "rdinitval")
				def.kind = ResetKind::Init;
			else if (token == "rdsrstval")
				def.kind = ResetKind::Sync;
			else if (token == "rdarstval")
				def.kind = ResetKind::Async;
			else
				abort();
			token = peek_token();
			if (token == "none") {
				def.val_kind = ResetValKind::None;
				get_token();
			} else if (token == "zero") {
				def.val_kind = ResetValKind::Zero;
				get_token();
			} else {
				def.val_kind = ResetValKind::Named;
				def.name = get_string();
			}
			get_semi();
			add_cap(port.rdrstval, def);
		} else if (token == "rdsrstmode") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `rdsrstmode` only allowed on sync read ports.\n", filename.c_str(), line_number);
			SrstKind val;
			token = get_token();
			if (token == "en-over-srst") {
				val = SrstKind::EnOverSrst;
			} else if (token == "srst-over-en") {
				val = SrstKind::SrstOverEn;
			} else if (token == "any") {
				val = SrstKind::Any;
			} else {
				log_error("%s:%d: expected `en-over-srst`, `srst-over-en`, or `any`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			add_cap(port.rdsrstmode, val);
		} else if (token == "wrbe") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrbe` only allowed on write ports.\n", filename.c_str(), line_number);
			add_cap(port.wrbe, get_int());
			get_semi();
		} else if (token == "wrprio") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrprio` only allowed on write ports.\n", filename.c_str(), line_number);
			do {
				add_cap(port.wrprio, get_string());
			} while (peek_string());
			get_semi();
		} else if (token == "wrtrans") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrtrans` only allowed on write ports.\n", filename.c_str(), line_number);
			token = peek_token();
			WrTransDef def;
			if (token == "self") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `wrtrans self` only allowed on sync read + sync write ports.\n", filename.c_str(), line_number);
				def.target_kind = TransTargetKind::Self;
				get_token();
			} else if (token == "other") {
				def.target_kind = TransTargetKind::Other;
				get_token();
			} else {
				def.target_kind = TransTargetKind::Named;
				def.target_name = get_string();
			}
			token = get_token();
			if (token == "new") {
				def.kind = TransKind::New;
			} else if (token == "old") {
				def.kind = TransKind::Old;
			} else {
				log_error("%s:%d: expected `new` or `old`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			add_cap(port.wrtrans, def);
		} else if (token == "wrcs") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrcs` only allowed on write ports.\n", filename.c_str(), line_number);
			add_cap(port.wrcs, get_int());
			get_semi();
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing port item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown port-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse_ram_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_ram_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_ram_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_ram_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_ram_block();
			}
			active = save;
		} else if (token == "option") {
			enter_option();
			parse_ram_block();
			exit_option();
		} else if (token == "dims") {
			MemoryDimsDef dims;
			dims.abits = get_int();
			dims.dbits = get_int();
			get_semi();
			add_cap(ram.dims, dims);
		} else if (token == "init") {
			MemoryInitKind kind;
			token = get_token();
			if (token == "zero") {
				kind = MemoryInitKind::Zero;
			} else if (token == "any") {
				kind = MemoryInitKind::Any;
			} else if (token == "none") {
				kind = MemoryInitKind::None;
			} else {
				log_error("%s:%d: expected `zero`, `any`, or `none`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			add_cap(ram.init, kind);
		} else if (token == "style") {
			do {
				add_cap(ram.style, get_string());
			} while (peek_string());
			get_semi();
		} else if (token == "port") {
			int orig_line = line_number;
			port = PortGroupDef();
			token = get_token();
			if (token == "ar") {
				port.kind = PortKind::Ar;
			} else if (token == "sr") {
				port.kind = PortKind::Sr;
			} else if (token == "sw") {
				port.kind = PortKind::Sw;
			} else if (token == "arsw") {
				port.kind = PortKind::Arsw;
			} else if (token == "srsw") {
				port.kind = PortKind::Srsw;
			} else {
				log_error("%s:%d: expected `ar`, `sr`, `sw`, `arsw`, or `srsw`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			do {
				port.names.push_back(get_string());
			} while (peek_string());
			parse_port_block();
			if (active) {
				// Add defaults for some options.
				if (port.kind != PortKind::Ar) {
					if (port.clock.empty()) {
						ClockDef def;
						def.kind = ClkPolKind::Anyedge;
						add_cap(port.clock, def);
					}
				}
				if (port.width.empty()) {
					add_cap(port.width, 1);
				}
				// Refuse to guess this one — there is no "safe" default.
				if (port.kind == PortKind::Sr || port.kind == PortKind::Srsw) {
					if (port.rden.empty()) {
						log_error("%s:%d: `rden` capability should be specified.\n", filename.c_str(), orig_line);
					}
				}
				add_cap(ram.ports, port);
			}
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing ram item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown ram-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse_top_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_top_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_top_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_top_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_top_block();
			}
			active = save;
		} else if (token == "ram") {
			int orig_line = line_number;
			ram = RamDef();
			token = get_token();
			if (token == "distributed") {
				ram.kind = RamKind::Distributed;
			} else if (token == "block") {
				ram.kind = RamKind::Block;
			} else if (token == "huge") {
				ram.kind = RamKind::Huge;
			} else {
				log_error("%s:%d: expected `distributed`, `block`, or `huge`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			ram.id = get_id();
			parse_ram_block();
			if (active) {
				if (ram.dims.empty())
					log_error("%s:%d: `dims` capability should be specified.\n", filename.c_str(), orig_line);
				if (ram.ports.empty())
					log_error("%s:%d: at least one port group should be specified.\n", filename.c_str(), orig_line);
				pool<std::string> pnedge_clock;
				pool<std::string> anyedge_clock;
				for (auto &port: ram.ports) {
					for (auto &def: port.val.clock) {
						if (def.val.name.empty())
							continue;
						if (def.val.kind == ClkPolKind::Anyedge)
							anyedge_clock.insert(def.val.name);
						else
							pnedge_clock.insert(def.val.name);
					}
				}
				for (auto &x: pnedge_clock)
					if (anyedge_clock.count(x))
						log_error("%s:%d: named clock \"%s\" used with both posedge/negedge and anyedge clocks.\n", filename.c_str(), orig_line, x.c_str());
				lib.ram_defs.push_back(ram);
			}
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing top item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown top-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse() {
		while (peek_token() != "")
			parse_top_item();
	}
};

struct WrPortConfig {
	// Index of the read port this port is merged with, or -1 if none.
	int rd_port;
	// Index of the PortGroupDef in the RamDef.
	int port_def;
	// Already-decided port option settings.
	Options portopts;
	// Emulate priority logic for this list of (source) write port indices.
	std::vector<int> emu_prio;
	// Chosen width for this port.
	int width;
	// Chosen wrbe unit width for this port.
	int wrbe;

	WrPortConfig() : rd_port(-1) {}
};

struct RdPortConfig {
	// Index of the write port this port is merged with, or -1 if none.
	int wr_port;
	// Index of the PortGroupDef in the RamDef.
	int port_def;
	// Already-decided port option settings.  Unused if wr_port is not -1:
	// in this case, use write port's portopts instead.
	Options portopts;
	// The named reset value assignments.
	dict<std::string, Const> resetvals;
	// If true, this is a sync port mapped into async mem, make an output
	// register.  Exclusive with the following options.
	bool emu_sync;
	// Emulate the EN / ARST / SRST / init value circuitry.
	bool emu_en;
	bool emu_arst;
	bool emu_srst;
	bool emu_init;
	// Emulate EN-SRST priority.
	bool emu_srst_en_prio;
	// Emulate transparency logic for this list of (source) write port indices.
	std::vector<int> emu_trans;
	// Chosen width for this port.
	int width;

	RdPortConfig() : wr_port(-1), emu_sync(false), emu_en(false), emu_arst(false), emu_srst(false), emu_init(false), emu_srst_en_prio(false) {}
};

struct SwizzleBit {
	// -1 for unused.
	int src_bit;
	int d2w_idx;
	int d2a_idx;
};

struct MemConfig {
	// Index of the RamDef in the Library.
	int ram_def;
	// Already-decided option settings.
	Options opts;
	// Port assignments, indexed by Mem port index.
	std::vector<WrPortConfig> wr_ports;
	std::vector<RdPortConfig> rd_ports;
	// The named clock and clock polarity assignments.
	// For anyedge clocks: the bool is the shared clock polarity.
	// For pos/negedge clocks: the bool is the "needs inversion" flag.
	dict<std::string, std::pair<SigBit, bool>> clocks_anyedge;
	dict<std::string, std::pair<SigBit, bool>> clocks_pnedge;
	// The chosen dims.
	int unit_abits;
	int unit_dbits;
	// this many low bits of (target) address are always-0 on all ports.
	int base_width_log2;
	int d2w_log2;
	// Replicate this memory side-by-side this many times for wider data path.
	int mult_d;
	// A single (unit_dbits*mult_d)-bit word contains this many address units.
	int d2a_factor;
	std::vector<SwizzleBit> swizzle;
};

typedef std::vector<MemConfig> MemConfigs;

bool opts_applied(const Options &dst, const Options &src) {
	for (auto &it: src) {
		auto it2 = dst.find(it.first);
		if (it2 == dst.end())
			return false;
		if (it2->second != it.second)
			return false;
	}
	return true;
}

bool apply_opts(Options &dst, const Options &src) {
	for (auto &it: src) {
		auto it2 = dst.find(it.first);
		if (it2 == dst.end())
			dst[it.first] = it.second;
		else if (it2->second != it.second)
			return false;
	}
	return true;
}

template<typename T>
bool apply_wrport_opts(MemConfig &cfg, int pidx, const Capability<T> &cap) {
	auto &pcfg = cfg.wr_ports[pidx];
	return apply_opts(cfg.opts, cap.opts) && apply_opts(pcfg.portopts, cap.portopts);
}

template<typename T>
bool apply_rdport_opts(MemConfig &cfg, int pidx, const Capability<T> &cap) {
	auto &pcfg = cfg.rd_ports[pidx];
	if (pcfg.wr_port != -1)
		return apply_wrport_opts(cfg, pcfg.wr_port, cap);
	return apply_opts(cfg.opts, cap.opts) && apply_opts(pcfg.portopts, cap.portopts);
}

template<typename T>
bool wrport_opts_applied(const MemConfig &cfg, int pidx, const Capability<T> &cap) {
	auto &pcfg = cfg.wr_ports[pidx];
	return opts_applied(cfg.opts, cap.opts) && opts_applied(pcfg.portopts, cap.portopts);
}

template<typename T>
bool rdport_opts_applied(MemConfig &cfg, int pidx, const Capability<T> &cap) {
	auto &pcfg = cfg.rd_ports[pidx];
	if (pcfg.wr_port != -1)
		return wrport_opts_applied(cfg, pcfg.wr_port, cap);
	return opts_applied(cfg.opts, cap.opts) && opts_applied(pcfg.portopts, cap.portopts);
}

bool apply_clock(MemConfig &cfg, const ClockDef &def, SigBit clk, bool clk_polarity) {
	if (def.name.empty())
		return true;
	if (def.kind == ClkPolKind::Anyedge) {
		auto it = cfg.clocks_anyedge.find(def.name);
		if (it == cfg.clocks_anyedge.end()) {
			cfg.clocks_anyedge.insert({def.name, {clk, clk_polarity}});
			return true;
		} else {
			return it->second == std::make_pair(clk, clk_polarity);
		}
	} else {
		bool flip = clk_polarity ^ (def.kind == ClkPolKind::Posedge);
		auto it = cfg.clocks_pnedge.find(def.name);
		if (it == cfg.clocks_pnedge.end()) {
			cfg.clocks_pnedge.insert({def.name, {clk, flip}});
			return true;
		} else {
			return it->second == std::make_pair(clk, flip);
		}
	}
}

bool apply_rstval(RdPortConfig &pcfg, const ResetValDef &def, Const val) {
	if (def.val_kind == ResetValKind::None)
		return false;
	if (def.val_kind == ResetValKind::Zero) {
		for (auto bit: val.bits)
			if (bit == State::S1)
				return false;
		return true;
	} else {
		auto it = pcfg.resetvals.find(def.name);
		if (it == pcfg.resetvals.end()) {
			pcfg.resetvals.insert({def.name, val});
			return true;
		} else {
			return it->second == val;
		}
	}
}

struct MapWorker {
	Module *module;
	ModWalker modwalker;
	SigMap sigmap_xmux;

	MapWorker(Module *module) : module(module), modwalker(module->design, module), sigmap_xmux(module) {
		for (auto cell : module->cells())
		{
			if (cell->type == ID($mux))
			{
				RTLIL::SigSpec sig_a = sigmap_xmux(cell->getPort(ID::A));
				RTLIL::SigSpec sig_b = sigmap_xmux(cell->getPort(ID::B));

				if (sig_a.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_b);
				else if (sig_b.is_fully_undef())
					sigmap_xmux.add(cell->getPort(ID::Y), sig_a);
			}
		}
	}
};

struct MemMapping {
	MapWorker &worker;
	QuickConeSat qcsat;
	Mem &mem;
	Library &lib;
	std::vector<MemConfig> cfgs;
	bool logic_ok;
	RamKind kind;
	std::string style;
	dict<int, int> wr_en_cache;
	dict<std::pair<int, int>, bool> wr_implies_rd_cache;
	dict<std::pair<int, int>, bool> wr_excludes_rd_cache;

	MemMapping(MapWorker &worker, Mem &mem, Library &lib) : worker(worker), qcsat(worker.modwalker), mem(mem), lib(lib) {
		determine_style();
		logic_ok = determine_logic_ok();
		if (kind == RamKind::Logic)
			return;
		for (int i = 0; i < GetSize(lib.ram_defs); i++) {
			MemConfig cfg;
			cfg.ram_def = i;
			cfgs.push_back(cfg);
		}
		handle_ram_kind();
		handle_ram_style();
		handle_init();
		handle_wr_ports();
		handle_rd_ports();
		handle_trans();
		// If we got this far, the memory is mappable.  The following two can require emulating
		// some functionality, but cannot cause the mapping to fail.
		handle_priority();
		handle_rd_init();
		handle_rd_arst();
		handle_rd_srst();
		// Now it is just a matter of picking geometry.
		log_debug("Memory %s.%s mapping candidates (pre-geometry):\n", log_id(mem.module->name), log_id(mem.memid));
		if (logic_ok)
			log_debug("- logic fallback\n");
		for (auto &cfg: cfgs) {
			auto &rdef = lib.ram_defs[cfg.ram_def];
			log_debug("- %s:\n", log_id(rdef.id));
			for (auto &it: cfg.opts)
				log_debug("  - option %s %s\n", it.first.c_str(), log_const(it.second));
			for (int i = 0; i < GetSize(mem.wr_ports); i++) {
				auto &pcfg = cfg.wr_ports[i];
				auto &pdef = rdef.ports[pcfg.port_def].val;
				if (pcfg.rd_port == -1)
					log_debug("  - write port %d: port group %s\n", i, pdef.names[0].c_str());
				else
					log_debug("  - write port %d: port group %s (shared with read port %d)\n", i, pdef.names[0].c_str(), pcfg.rd_port);

				for (auto &it: pcfg.portopts)
					log_debug("    - option %s %s\n", it.first.c_str(), log_const(it.second));
				for (auto i: pcfg.emu_prio)
					log_debug("    - emulate priority over write port %d\n", i);
			}
			for (int i = 0; i < GetSize(mem.rd_ports); i++) {
				auto &pcfg = cfg.rd_ports[i];
				auto &pdef = rdef.ports[pcfg.port_def].val;
				if (pcfg.wr_port == -1)
					log_debug("  - read port %d: port group %s\n", i, pdef.names[0].c_str());
				else
					log_debug("  - read port %d: port group %s (shared with write port %d)\n", i, pdef.names[0].c_str(), pcfg.wr_port);
				for (auto &it: pcfg.portopts)
					log_debug("    - option %s %s\n", it.first.c_str(), log_const(it.second));
				if (pcfg.emu_sync)
					log_debug("    - emulate data register\n");
				if (pcfg.emu_en)
					log_debug("    - emulate clock enable\n");
				if (pcfg.emu_arst)
					log_debug("    - emulate async reset\n");
				if (pcfg.emu_srst)
					log_debug("    - emulate sync reset\n");
				if (pcfg.emu_init)
					log_debug("    - emulate init value\n");
				if (pcfg.emu_srst_en_prio)
					log_debug("    - emulate sync reset / enable priority\n");
				for (auto i: pcfg.emu_trans)
					log_debug("    - emulate transparency with write port %d\n", i);
			}
		}
		handle_dims();
		// XXX
	}

	bool addr_compatible(int wpidx, int rpidx) {
		auto &wport = mem.wr_ports[wpidx];
		auto &rport = mem.rd_ports[rpidx];
		int max_wide_log2 = std::max(rport.wide_log2, wport.wide_log2);
		SigSpec raddr = rport.addr.extract_end(max_wide_log2);
		SigSpec waddr = wport.addr.extract_end(max_wide_log2);
		int abits = std::max(GetSize(raddr), GetSize(waddr));
		raddr.extend_u0(abits);
		waddr.extend_u0(abits);
		return worker.sigmap_xmux(raddr) == worker.sigmap_xmux(waddr);
	}

	int get_wr_en(int wpidx) {
		auto it = wr_en_cache.find(wpidx);
		if (it != wr_en_cache.end())
			return it->second;
		int res = qcsat.ez->expression(qcsat.ez->OpOr, qcsat.importSig(mem.wr_ports[wpidx].en));
		wr_en_cache.insert({wpidx, res});
		return res;
	}

	bool get_wr_implies_rd(int wpidx, int rpidx) {
		auto key = std::make_pair(wpidx, rpidx);
		auto it = wr_implies_rd_cache.find(key);
		if (it != wr_implies_rd_cache.end())
			return it->second;
		int wr_en = get_wr_en(wpidx);
		int rd_en = qcsat.importSigBit(mem.rd_ports[rpidx].en[0]);
		qcsat.prepare();
		bool res = !qcsat.ez->solve(wr_en, qcsat.ez->NOT(rd_en));
		wr_implies_rd_cache.insert({key, res});
		return res;
	}

	bool get_wr_excludes_rd(int wpidx, int rpidx) {
		auto key = std::make_pair(wpidx, rpidx);
		auto it = wr_excludes_rd_cache.find(key);
		if (it != wr_excludes_rd_cache.end())
			return it->second;
		int wr_en = get_wr_en(wpidx);
		int rd_en = qcsat.importSigBit(mem.rd_ports[rpidx].en[0]);
		qcsat.prepare();
		bool res = !qcsat.ez->solve(wr_en, rd_en);
		wr_excludes_rd_cache.insert({key, res});
		return res;
	}

	void determine_style();
	bool determine_logic_ok();
	void handle_ram_kind();
	void handle_ram_style();
	void handle_init();
	void handle_wr_ports();
	void handle_rd_ports();
	void handle_trans();
	void handle_priority();
	void handle_rd_init();
	void handle_rd_arst();
	void handle_rd_srst();
	void handle_dims();
};

// Go through memory attributes to determine user-requested mapping style.
void MemMapping::determine_style() {
	kind = RamKind::Auto;
	style = "";
	for (auto attr: {ID::ram_block, ID::rom_block, ID::ram_style, ID::rom_style, ID::ramstyle, ID::romstyle, ID::syn_ramstyle, ID::syn_romstyle}) {
		if (mem.has_attribute(attr)) {
			Const val = mem.attributes.at(attr);
			if (val == 1) {
				kind = RamKind::NotLogic;
				return;
			}
			std::string val_s = val.decode_string();
			if (val_s == "auto") {
				// Nothing.
			} else if (val_s == "logic" || val_s == "registers") {
				kind = RamKind::Logic;
			} else if (val_s == "distributed") {
				kind = RamKind::Distributed;
			} else if (val_s == "block" || val_s == "block_ram" || val_s == "ebr") {
				kind = RamKind::Block;
			} else if (val_s == "huge" || val_s == "ultra") {
				kind = RamKind::Huge;
			} else {
				kind = RamKind::NotLogic;
				style = val_s;
			}
			return;
		}
	}
	if (mem.get_bool_attribute(ID::logic_block))
		kind = RamKind::Logic;
}

// Determine whether the memory can be mapped entirely to soft logic.
bool MemMapping::determine_logic_ok() {
	if (kind != RamKind::Auto && kind != RamKind::Logic)
		return false;
	// Memory is mappable entirely to soft logic iff all its write ports are in the same clock domain.
	if (mem.wr_ports.empty())
		return true;
	for (auto &port: mem.wr_ports) {
		if (!port.clk_enable)
			return false;
		if (port.clk != mem.wr_ports[0].clk)
			return false;
		if (port.clk_polarity != mem.wr_ports[0].clk_polarity)
			return false;
	}
	return true;
}

// Apply RAM kind restrictions (logic/distributed/block/huge), if any.
void MemMapping::handle_ram_kind() {
	if (kind == RamKind::Auto || kind == RamKind::NotLogic)
		return;
	MemConfigs new_cfgs;
	for (auto &cfg: cfgs) {
		if (lib.ram_defs[cfg.ram_def].kind == kind)
			new_cfgs.push_back(cfg);
	}
	cfgs = new_cfgs;
	if (cfgs.empty()) {
		const char *kind_s = "";
		switch (kind) {
			case RamKind::Distributed:
				kind_s = "distributed";
				break;
			case RamKind::Block:
				kind_s = "block";
				break;
			case RamKind::Huge:
				kind_s = "huge";
				break;
			default:
				break;
		}
		log_error("%s.%s: no available %s RAMs\n", log_id(mem.module->name), log_id(mem.memid), kind_s);
	}
}

// Apply specific RAM style restrictions, if any.
void MemMapping::handle_ram_style() {
	if (style == "")
		return;
	MemConfigs new_cfgs;
	for (auto &cfg: cfgs) {
		for (auto &def: lib.ram_defs[cfg.ram_def].style) {
			if (def.val != style)
				continue;
			MemConfig new_cfg = cfg;
			if (!apply_opts(new_cfg.opts, def.opts))
				continue;
			new_cfgs.push_back(new_cfg);
		}
	}
	cfgs = new_cfgs;
	if (cfgs.empty())
		log_error("%s.%s: no available RAMs with style \"%s\"\n", log_id(mem.module->name), log_id(mem.memid), style.c_str());
}

// Handle memory initializer restrictions, if any.
void MemMapping::handle_init() {
	bool has_nonx = false;
	bool has_one = false;

	for (auto &init: mem.inits) {
		if (init.data.is_fully_undef())
			continue;
		has_nonx = true;
		for (auto bit: init.data)
			if (bit == State::S1)
				has_one = true;
	}

	if (!has_nonx)
		return;

	MemConfigs new_cfgs;
	for (auto &cfg: cfgs) {
		for (auto &def: lib.ram_defs[cfg.ram_def].init) {
			if (has_one) {
				if (def.val != MemoryInitKind::Any)
					continue;
			} else {
				if (def.val != MemoryInitKind::Any && def.val != MemoryInitKind::Zero)
					continue;
			}
			MemConfig new_cfg = cfg;
			if (!apply_opts(new_cfg.opts, def.opts))
				continue;
			new_cfgs.push_back(new_cfg);
		}
	}
	cfgs = new_cfgs;
}

// Perform write port assignment, validating clock options as we go.
void MemMapping::handle_wr_ports() {
	for (auto &port: mem.wr_ports) {
		if (!port.clk_enable) {
			// Async write ports not supported.
			cfgs.clear();
			return;
		}
		MemConfigs new_cfgs;
		for (auto &cfg: cfgs) {
			auto &ram_def = lib.ram_defs[cfg.ram_def];
			for (int i = 0; i < GetSize(ram_def.ports); i++) {
				auto &def = ram_def.ports[i];
				// Make sure the target is a write port.
				if (def.val.kind == PortKind::Ar || def.val.kind == PortKind::Sr)
					continue;
				// Make sure the target port group still has a free port.
				int used = 0;
				for (auto &oport: cfg.wr_ports)
					if (oport.port_def == i)
						used++;
				if (used >= GetSize(def.val.names))
					continue;
				// Apply the options.
				MemConfig cfg2 = cfg;
				if (!apply_opts(cfg2.opts, def.opts))
					continue;
				WrPortConfig pcfg2;
				pcfg2.rd_port = -1;
				pcfg2.port_def = i;
				// Pick a clock def.
				for (auto &cdef: def.val.clock) {
					MemConfig cfg3 = cfg2;
					WrPortConfig pcfg3 = pcfg2;
					if (!apply_opts(cfg3.opts, cdef.opts))
						continue;
					if (!apply_opts(pcfg3.portopts, cdef.portopts))
						continue;
					if (!apply_clock(cfg3, cdef.val, port.clk, port.clk_polarity))
						continue;
					cfg3.wr_ports.push_back(pcfg3);
					new_cfgs.push_back(cfg3);
				}
			}
		}
		cfgs = new_cfgs;
	}
}

// Perform read port assignment, validating clock and rden options as we go.
void MemMapping::handle_rd_ports() {
	for (int pidx = 0; pidx < GetSize(mem.rd_ports); pidx++) {
		auto &port = mem.rd_ports[pidx];
		MemConfigs new_cfgs;
		for (auto &cfg: cfgs) {
			auto &ram_def = lib.ram_defs[cfg.ram_def];
			// First pass: read port not shared with a write port.
			for (int i = 0; i < GetSize(ram_def.ports); i++) {
				auto &def = ram_def.ports[i];
				// Make sure the target is a read port.
				if (def.val.kind == PortKind::Sw)
					continue;
				// If mapping an async port, accept only async defs.
				if (!port.clk_enable) {
					if (def.val.kind == PortKind::Sr || def.val.kind == PortKind::Srsw)
						continue;
				}
				// Make sure the target port group has a port not used up by write ports.
				// Overuse by other read ports is not a problem — this will just result
				// in memory duplication.
				int used = 0;
				for (auto &oport: cfg.wr_ports)
					if (oport.port_def == i)
						used++;
				if (used >= GetSize(def.val.names))
					continue;
				// Apply the options.
				MemConfig cfg2 = cfg;
				if (!apply_opts(cfg2.opts, def.opts))
					continue;
				RdPortConfig pcfg2;
				pcfg2.wr_port = -1;
				pcfg2.port_def = i;
				if (def.val.kind == PortKind::Sr || def.val.kind == PortKind::Srsw) {
					pcfg2.emu_sync = false;
					// Pick a clock def.
					for (auto &cdef: def.val.clock) {
						MemConfig cfg3 = cfg2;
						RdPortConfig pcfg3 = pcfg2;
						if (!apply_opts(cfg3.opts, cdef.opts))
							continue;
						if (!apply_opts(pcfg3.portopts, cdef.portopts))
							continue;
						if (!apply_clock(cfg3, cdef.val, port.clk, port.clk_polarity))
							continue;
						// Pick a rden def.
						for (auto &endef: def.val.rden) {
							MemConfig cfg4 = cfg3;
							RdPortConfig pcfg4 = pcfg3;
							if (!apply_opts(cfg4.opts, endef.opts))
								continue;
							if (!apply_opts(pcfg4.portopts, endef.portopts))
								continue;
							if (endef.val == RdEnKind::None && port.en != State::S1) {
								pcfg4.emu_en = true;
							}
							cfg4.rd_ports.push_back(pcfg4);
							new_cfgs.push_back(cfg4);
						}
					}
				} else {
					pcfg2.emu_sync = port.clk_enable;
					cfg2.rd_ports.push_back(pcfg2);
					new_cfgs.push_back(cfg2);
				}
			}
			// Second pass: read port shared with a write port.
			for (int wpidx = 0; wpidx < GetSize(mem.wr_ports); wpidx++) {
				auto &wport = mem.wr_ports[wpidx];
				int didx = cfg.wr_ports[wpidx].port_def;
				auto &def = ram_def.ports[didx];
				// Make sure the write port is not yet shared.
				if (cfg.wr_ports[wpidx].rd_port != -1)
					continue;
				// Make sure the target is a read port.
				if (def.val.kind == PortKind::Sw)
					continue;
				// Validate address compatibility.
				if (!addr_compatible(wpidx, pidx))
					continue;
				// Validate clock compatibility, if needed.
				if (def.val.kind == PortKind::Srsw) {
					if (!port.clk_enable)
						continue;
					if (port.clk != wport.clk)
						continue;
					if (port.clk_polarity != wport.clk_polarity)
						continue;
				}
				// Okay, let's fill it in.
				MemConfig cfg2 = cfg;
				cfg2.wr_ports[wpidx].rd_port = pidx;
				RdPortConfig pcfg2;
				pcfg2.wr_port = wpidx;
				pcfg2.port_def = didx;
				pcfg2.emu_sync = port.clk_enable && def.val.kind == PortKind::Arsw;
				// For srsw, pick rden capability.
				if (def.val.kind == PortKind::Srsw) {
					for (auto &endef: def.val.rden) {
						MemConfig cfg3 = cfg2;
						RdPortConfig pcfg3 = pcfg2;
						if (!apply_wrport_opts(cfg3, wpidx, endef))
							continue;
						switch (endef.val) {
							case RdEnKind::None:
								pcfg3.emu_en = port.en != State::S1;
								break;
							case RdEnKind::Any:
								break;
							case RdEnKind::WriteImplies:
								pcfg3.emu_en = !get_wr_implies_rd(wpidx, pidx);
								break;
							case RdEnKind::WriteExcludes:
								if (!get_wr_excludes_rd(wpidx, pidx))
									continue;
								break;
						}
						cfg3.rd_ports.push_back(pcfg3);
						new_cfgs.push_back(cfg3);
					}
				} else {
					cfg2.rd_ports.push_back(pcfg2);
					new_cfgs.push_back(cfg2);
				}
			}
		}
		cfgs = new_cfgs;
	}
}

// Validate transparency restrictions, determine where to add soft transparency logic.
void MemMapping::handle_trans() {
	for (int rpidx = 0; rpidx < GetSize(mem.rd_ports); rpidx++) {
		auto &rport = mem.rd_ports[rpidx];
		if (!rport.clk_enable)
			continue;
		for (int wpidx = 0; wpidx < GetSize(mem.wr_ports); wpidx++) {
			auto &wport = mem.wr_ports[wpidx];
			if (!wport.clk_enable)
				continue;
			if (rport.clk != wport.clk)
				continue;
			if (rport.clk_polarity != wport.clk_polarity)
				continue;
			if (rport.collision_x_mask[wpidx])
				continue;
			bool transparent = rport.transparency_mask[wpidx];
			// If we got this far, we have a transparency restriction
			// to uphold.
			MemConfigs new_cfgs;
			for (auto &cfg: cfgs) {
				auto &rpcfg = cfg.rd_ports[rpidx];
				auto &wpcfg = cfg.wr_ports[wpidx];
				auto &rdef = lib.ram_defs[cfg.ram_def];
				auto &wpdef = rdef.ports[wpcfg.port_def];
				auto &rpdef = rdef.ports[rpcfg.port_def];
				if (rpcfg.emu_sync) {
					// For async read port, just add the transparency logic
					// if necessary.
					if (transparent)
						rpcfg.emu_trans.push_back(wpidx);
					new_cfgs.push_back(cfg);
				} else {
					// Otherwise, split through the relevant wrtrans caps.
					// For non-transparent ports, the cap needs to be present.
					// For transparent ports, we can emulate transparency
					// even without a direct cap.
					bool found_free = false;
					for (auto &tdef: wpdef.val.wrtrans) {
						// Check if the target matches.
						switch (tdef.val.target_kind) {
							case TransTargetKind::Self:
								if (wpcfg.rd_port != rpidx)
									continue;
								break;
							case TransTargetKind::Other:
								if (wpcfg.rd_port == rpidx)
									continue;
								break;
							case TransTargetKind::Named:
								if (rpdef.val.names[0] != tdef.val.target_name)
									continue;
								break;
						}
						// Check if the transparency kind is acceptable.
						if (transparent) {
							if (tdef.val.kind == TransKind::Old)
								continue;
						} else {
							if (tdef.val.kind != TransKind::Old)
								continue;
						}
						// Okay, we can use this cap.
						MemConfig cfg2 = cfg;
						if (wrport_opts_applied(cfg2, wpidx, tdef))
							found_free = true;
						else if (!apply_wrport_opts(cfg2, wpidx, tdef))
							continue;
						new_cfgs.push_back(cfg2);
					}
					if (!found_free && transparent) {
						// If the port pair is transparent, but no cap was
						// found, or the cap found had a splitting cost
						// to it, consider emulation as well.
						rpcfg.emu_trans.push_back(wpidx);
						new_cfgs.push_back(cfg);
					}
				}
			}
			cfgs = new_cfgs;
		}
	}
}

// Determine where to add soft priority logic.
void MemMapping::handle_priority() {
	for (int p1idx = 0; p1idx < GetSize(mem.wr_ports); p1idx++) {
		for (int p2idx = 0; p2idx < GetSize(mem.wr_ports); p2idx++) {
			auto &port2 = mem.wr_ports[p2idx];
			if (!port2.priority_mask[p1idx])
				continue;
			MemConfigs new_cfgs;
			for (auto &cfg: cfgs) {
				auto &p1cfg = cfg.rd_ports[p1idx];
				auto &p2cfg = cfg.wr_ports[p2idx];
				auto &rdef = lib.ram_defs[cfg.ram_def];
				auto &p1def = rdef.ports[p1cfg.port_def];
				auto &p2def = rdef.ports[p2cfg.port_def];
				bool found_free = false;
				for (auto &prdef: p2def.val.wrprio) {
					// Check if the target matches.
					if (p1def.val.names[0] != prdef.val)
						continue;
					// Okay, we can use this cap.
					MemConfig cfg2 = cfg;
					if (wrport_opts_applied(cfg2, p2idx, prdef))
						found_free = true;
					else if (!apply_wrport_opts(cfg2, p2idx, prdef))
						continue;
					new_cfgs.push_back(cfg2);
				}
				if (!found_free) {
					// If no cap was found, or the cap found had a splitting
					// cost to it, consider emulation as well.
					p2cfg.emu_prio.push_back(p1idx);
					new_cfgs.push_back(cfg);
				}
			}
			cfgs = new_cfgs;
		}
	}
}

// Determine where to add soft init value logic.
void MemMapping::handle_rd_init() {
	for (int pidx = 0; pidx < GetSize(mem.rd_ports); pidx++) {
		auto &port = mem.rd_ports[pidx];
		// Only sync ports are relevant.
		if (!port.clk_enable)
			continue;
		// Skip ports with no init value.
		if (port.init_value.is_fully_undef())
			continue;
		MemConfigs new_cfgs;
		for (auto &cfg: cfgs) {
			auto &pcfg = cfg.rd_ports[pidx];
			auto &rdef = lib.ram_defs[cfg.ram_def];
			auto &pdef = rdef.ports[pcfg.port_def];
			// If emulated by async port, init value will be included for free.
			if (pcfg.emu_sync) {
				new_cfgs.push_back(cfg);
				continue;
			}
			// Otherwise, find a cap.
			bool found_free = false;
			for (auto &rstdef: pdef.val.rdrstval) {
				if (rstdef.val.kind != ResetKind::Init)
					continue;
				MemConfig cfg2 = cfg;
				auto &pcfg2 = cfg2.rd_ports[pidx];
				if (!apply_rstval(pcfg2, rstdef.val, port.init_value))
					continue;
				if (rdport_opts_applied(cfg2, pidx, rstdef))
					found_free = true;
				else if (!apply_rdport_opts(cfg2, pidx, rstdef))
					continue;
				new_cfgs.push_back(cfg2);
			}
			if (!found_free) {
				// If no cap was found, or the cap found had a splitting
				// cost to it, consider emulation as well.
				pcfg.emu_init = true;
				new_cfgs.push_back(cfg);
			}
		}
		cfgs = new_cfgs;
	}
}

// Determine where to add soft async reset logic.
void MemMapping::handle_rd_arst() {
	for (int pidx = 0; pidx < GetSize(mem.rd_ports); pidx++) {
		auto &port = mem.rd_ports[pidx];
		// Only sync ports are relevant.
		if (!port.clk_enable)
			continue;
		// Skip ports with no async reset.
		if (port.arst == State::S0)
			continue;
		if (port.arst_value.is_fully_undef())
			continue;
		MemConfigs new_cfgs;
		for (auto &cfg: cfgs) {
			auto &pcfg = cfg.rd_ports[pidx];
			auto &rdef = lib.ram_defs[cfg.ram_def];
			auto &pdef = rdef.ports[pcfg.port_def];
			// If emulated by async port, reset will be included for free.
			if (pcfg.emu_sync) {
				new_cfgs.push_back(cfg);
				continue;
			}
			// Otherwise, find a cap.
			bool found_free = false;
			for (auto &rstdef: pdef.val.rdrstval) {
				if (rstdef.val.kind != ResetKind::Async)
					continue;
				MemConfig cfg2 = cfg;
				auto &pcfg2 = cfg2.rd_ports[pidx];
				if (!apply_rstval(pcfg2, rstdef.val, port.arst_value))
					continue;
				if (rdport_opts_applied(cfg2, pidx, rstdef))
					found_free = true;
				else if (!apply_rdport_opts(cfg2, pidx, rstdef))
					continue;
				new_cfgs.push_back(cfg2);
			}
			if (!found_free) {
				// If no cap was found, or the cap found had a splitting
				// cost to it, consider emulation as well.
				pcfg.emu_arst = true;
				new_cfgs.push_back(cfg);
			}
		}
		cfgs = new_cfgs;
	}
}

// Determine where to add soft sync reset logic.
void MemMapping::handle_rd_srst() {
	for (int pidx = 0; pidx < GetSize(mem.rd_ports); pidx++) {
		auto &port = mem.rd_ports[pidx];
		// Only sync ports are relevant.
		if (!port.clk_enable)
			continue;
		// Skip ports with no async reset.
		if (port.srst == State::S0)
			continue;
		if (port.srst_value.is_fully_undef())
			continue;
		MemConfigs new_cfgs;
		for (auto &cfg: cfgs) {
			auto &pcfg = cfg.rd_ports[pidx];
			auto &rdef = lib.ram_defs[cfg.ram_def];
			auto &pdef = rdef.ports[pcfg.port_def];
			// If emulated by async port, reset will be included for free.
			if (pcfg.emu_sync) {
				new_cfgs.push_back(cfg);
				continue;
			}
			// Otherwise, find a cap.
			bool found_free = false;
			for (auto &rstdef: pdef.val.rdrstval) {
				if (rstdef.val.kind != ResetKind::Sync)
					continue;
				MemConfig cfg2 = cfg;
				auto &pcfg2 = cfg2.rd_ports[pidx];
				if (!apply_rstval(pcfg2, rstdef.val, port.srst_value))
					continue;
				if (rdport_opts_applied(cfg2, pidx, rstdef))
					found_free = true;
				else if (!apply_rdport_opts(cfg2, pidx, rstdef))
					continue;
				// If enable is in use, need to make sure the relative priority of
				// enable and srst is correct.  Otherwise, proceed immediately.
				if (port.en == State::S1) {
					new_cfgs.push_back(cfg2);
				} else {
					for (auto &mdef: pdef.val.rdsrstmode) {
						// Any value of the option is usable, at worst we'll emulate the priority.
						MemConfig cfg3 = cfg2;
						auto &pcfg3 = cfg3.rd_ports[pidx];
						if (mdef.val == SrstKind::SrstOverEn && port.ce_over_srst)
							pcfg3.emu_srst_en_prio = true;
						if (mdef.val == SrstKind::EnOverSrst && !port.ce_over_srst)
							pcfg3.emu_srst_en_prio = true;
						if (!apply_rdport_opts(cfg3, pidx, mdef))
							continue;
						new_cfgs.push_back(cfg3);
					}
				}
			}
			if (!found_free) {
				// If no cap was found, or the cap found had a splitting
				// cost to it, consider emulation as well.
				pcfg.emu_srst = true;
				new_cfgs.push_back(cfg);
			}
		}
		cfgs = new_cfgs;
	}
}

void MemMapping::handle_dims() {
	// XXX
}

struct MemoryLibMapPass : public Pass {
	MemoryLibMapPass() : Pass("memory_libmap", "map memories to cells") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    memory_libmap -lib <library_file> [-D <condition>] [selection]\n");
		log("\n");
		log("This pass takes a description of available RAM cell types and maps\n");
		log("all selected memories to one of them, or leaves them  to be mapped to FFs.\n");
		log("\n");
		log("  -lib <library_file>\n");
		log("    Selects a library file containing RAM cell definitions. This option\n");
		log("    can be passed more than once to select multiple libraries.\n");
		log("\n");
		log("  -D <condition>\n");
		log("    Enables a condition that can be checked within the library file\n");
		log("    to eg. select between slightly different hardware variants.\n");
		log("    This option can be passed any number of times.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::vector<std::string> lib_files;
		pool<std::string> defines;
		log_header(design, "Executing MEMORY_LIBMAP pass (mapping memories to cells).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-lib" && argidx+1 < args.size()) {
				lib_files.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-D" && argidx+1 < args.size()) {
				defines.insert(args[++argidx]);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		Library lib(defines);
		for (auto &file: lib_files) {
			Parser(file, lib);
		}
		lib.prepare();

		for (auto module : design->selected_modules()) {
			MapWorker worker(module);
			auto mems = Mem::get_selected_memories(module);
			for (auto &mem : mems)
			{
				MemMapping map(worker, mem, lib);
				// TODO
			}
		}
	}
} MemoryLibMapPass;

PRIVATE_NAMESPACE_END
