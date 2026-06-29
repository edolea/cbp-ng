#include "../cbp.hpp"
#include "../harcom.hpp"
#include "common.hpp"   // update_ctr, geometric_folds, ... (shared helpers)

using namespace hcm;

template<u64 LOG_TABLE = 12, u64 HIST = 12, u64 LOG_GTABLE = 12, u64 TAGBITS = 8>
struct my_pred_tage_phase2 : predictor {
    // static_assert(HIST <= LOG_TABLE);
    static constexpr u64 TABLE = 1 << LOG_TABLE;
    static constexpr u64 GLOBAL_TABLE = 1 << LOG_GTABLE;

    // [ tag | ctr | u ] -> [8, 3, 1]
    static constexpr u64 CTR_BITS = 3;   // counter: [0 -> 3] = [T -> NT]
    static constexpr u64 U_BITS   = 1;   // useful bit
    static constexpr u64 WIDTH    = TAGBITS + CTR_BITS + U_BITS;

    ram<val<2>, TABLE> bht;
    ram<val<WIDTH>, GLOBAL_TABLE> tagged_bht;
    reg<HIST> global_history;

    reg<2> t;
    reg<TAGBITS> tag_t;
    reg<WIDTH> entry_t;
    reg<1> hit;

    reg<LOG_TABLE> index;
    reg<LOG_GTABLE> global_index;

    // get index helpers
    val<LOG_TABLE> get_index([[maybe_unused]] val<64> inst_pc) {
        return (inst_pc >> 2).make_array(val<LOG_TABLE>{}).fold_xor();
    }

    val<LOG_GTABLE> get_global_index([[maybe_unused]] val<64> inst_pc) {
        return (inst_pc >> 2).make_array(val<LOG_GTABLE>{}).fold_xor() ^ val<LOG_GTABLE>{global_history};
    }

    val<TAGBITS> get_tag([[maybe_unused]] val<64> inst_pc) {
        return val<TAGBITS>{inst_pc >> 2}.reverse() ^ val<TAGBITS>{global_history >> 1};
    }

    // unpacking helpers
    val<TAGBITS>  entry_tag(val<WIDTH> e) { return val<TAGBITS>{e >> (U_BITS + CTR_BITS)}; }
    val<CTR_BITS> entry_ctr(val<WIDTH> e) { return val<CTR_BITS>{e >> U_BITS}; }
    val<1>        entry_u(val<WIDTH> e) { return val<1>{e}; }
    val<1>        ctr_pred(val<CTR_BITS> c) { return val<1>{c >> (CTR_BITS - 1)}; }

    // ----- predictions ------
    val<1> predict1([[maybe_unused]] val<64> inst_pc) {
        index = get_index(inst_pc);
        t = bht.read(index);
        return t >> 1;
    }

    val<1> reuse_predict1([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    val<1> predict2([[maybe_unused]] val<64> inst_pc) {
        global_index = get_global_index(inst_pc);
        val<WIDTH> e = tagged_bht.read(global_index);
        entry_t = e; // da togliere 'e' ed usare 'entry_t'

        tag_t = get_tag(inst_pc);
        hit = entry_tag(e) == tag_t;
        val<1> tagged_pred = ctr_pred(entry_ctr(e));
        val<1> base_pred = t >> 1;

        return select(hit, tagged_pred, base_pred);
    }

    val<1> reuse_predict2([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    void update_condbr([[maybe_unused]] val<64> branch_pc, [[maybe_unused]] val<1> taken, [[maybe_unused]] val<64> next_pc) {
        // recover what each component predicted (from values saved at predict time)
        val<CTR_BITS> ctr = entry_ctr(entry_t);
        val<1> u = entry_u(entry_t);
        val<1> base_pred = val<1>{t >> 1};
        val<1> tagged_pred = ctr_pred(ctr);
        val<1> pred2 = select(hit, tagged_pred, base_pred);
        val<1> mispredict = pred2 != taken;

        // (1) base bimodal: always nudge toward the outcome
        val<2> new_t = update_ctr(t, taken);
        val<1> update_base = new_t != t;

        // (2) tagged table, case A — HIT: train the counter; set the useful bit
        //     when the tagged prediction was right AND it disagreed with the base
        //     (i.e. it actually earned its keep).
        val<CTR_BITS> new_ctr = update_ctr(ctr, taken);
        val<1> tagged_correct = tagged_pred == taken;
        val<1> differ = tagged_pred != base_pred;
        val<1> new_u = tagged_correct & differ;
        val<WIDTH> trained = concat(tag_t, new_ctr, new_u);

        // (2) tagged table, case B — MISS + misprediction: allocate a new entry,
        //     but only over a non-useful slot (u == 0). Counter starts weak,
        //     just past the threshold toward the actual outcome.
        val<1> can_alloc = ~hit & mispredict & (u == hard<0>{});
        val<CTR_BITS> weak_ctr = select(taken, val<CTR_BITS>{4}, val<CTR_BITS>{3});
        val<WIDTH> allocated = concat(tag_t, weak_ctr, val<1>{0});

        val<1> write_t1 = hit | can_alloc;
        val<WIDTH> t1_data = select(hit, trained, allocated);

        need_extra_cycle(update_base | write_t1);
        execute_if(update_base, [&]() { bht.write(index, new_t); });
        execute_if(write_t1,    [&]() { tagged_bht.write(global_index, t1_data); });

        global_history = (global_history << 1) | val<HIST>{taken};
    }

    void update_cycle([[maybe_unused]] instruction_info &block_end_info) {
    }
};