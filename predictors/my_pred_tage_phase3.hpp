#include "../cbp.hpp"
#include "../harcom.hpp"
#include "common.hpp"

using namespace hcm;

template<u64 LOGB = 12, u64 LOGG = 11, u64 TAGBITS = 9,
         u64 NUMG = 4, u64 MINHIST = 4, u64 MAXHIST = 64>
struct my_pred_tage : predictor {
    static constexpr u64 BTABLE = 1 << LOGB;
    static constexpr u64 GTABLE = 1 << LOGG;

    // tagged-entry field widths, packed as [ tag | ctr | u ] (tag = high bits)
    static constexpr u64 CTRBITS = 3;  // counter: [0 -> 3] = [T -> NT]
    static constexpr u64 UBITS   = 1;  // useful bit
    static constexpr u64 EW      = TAGBITS + CTRBITS + UBITS;

    ram<val<2>, BTABLE> bht;
    ram<val<EW>, GTABLE> gt[NUMG];
    geometric_folds<NUMG, MINHIST, MAXHIST, LOGG, TAGBITS> gfolds;

    reg<2> bctr;                    // base counter read in predict1
    reg<LOGB> bindex;               // base index
    arr<reg<LOGG>, NUMG> gindex;    // per-table indexes
    arr<reg<TAGBITS>, NUMG> gtag;   // per-table tags computed this prediction
    arr<reg<EW>, NUMG> gent;        // per-table entries read in predict2
    reg<NUMG + 1> match1;           // provider (longest match), one-hot; bit NUMG = base
    reg<NUMG + 1> match2;           // alternate (second-longest match), one-hot

    // ---- hashes ----
    val<LOGB> base_index(val<64> inst_pc) {
        return (inst_pc >> 2).make_array(val<LOGB>{}).fold_xor();
    }
    // unpack a tagged entry
    val<TAGBITS> ent_tag(val<EW> e) { return val<TAGBITS>{e >> (UBITS + CTRBITS)}; }
    val<CTRBITS> ent_ctr(val<EW> e) { return val<CTRBITS>{e >> UBITS}; }
    val<1>       ent_u  (val<EW> e) { return val<1>{e}; }
    val<1>       ctr_pred(val<CTRBITS> c) { return val<1>{c >> (CTRBITS - 1)}; }

    // ---- prediction ----
    val<1> predict1(val<64> inst_pc) {
        bindex = base_index(inst_pc);
        bctr = bht.read(bindex);
        return bctr >> 1;
    }

    val<1> reuse_predict1([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    val<1> predict2(val<64> inst_pc) {
        // index / tag bases from PC; index folds all PC bits, tag reverses low
        // bits, so the two hashes stay decorrelated
        val<LOGG> pc_idx = (inst_pc >> 2).make_array(val<LOGG>{}).fold_xor();
        val<TAGBITS> pc_tag = val<TAGBITS>{inst_pc >> 2}.reverse();

        for (u64 i = 0; i < NUMG; i++) {
            gindex[i] = pc_idx ^ gfolds.template get<0>(i);     // history fold for index
            gtag[i]   = pc_tag ^ gfolds.template get<1>(i);     // history fold for tag
        }
        for (u64 i = 0; i < NUMG; i++) {
            gent[i] = gt[i].read(gindex[i]);
        }

        // per-table tag match (bit i = table i hits); table 0 = longest history
        arr<val<1>, NUMG> tagcmp = [&](u64 i) { return ent_tag(gent[i]) == gtag[i]; };
        val<NUMG> hits = tagcmp.concat();
        // per-table prediction + base prediction packed into one vector
        arr<val<1>, NUMG> tpred = [&](u64 i) { return ctr_pred(ent_ctr(gent[i])); };
        val<1> base_pred = val<1>{bctr >> 1};
        val<NUMG + 1> preds = concat(base_pred, tpred.concat());   // base at bit NUMG

        // match has the base as a permanent fallback at bit NUMG; one_hot()
        // isolates the lowest set bit == longest matching table (or base).
        val<NUMG + 1> match = concat(val<1>{1}, hits);
        match1 = match.one_hot();                          // provider
        match2 = (match ^ match1).one_hot();               // alternate

        return (match1 & preds) != hard<0>{};
    }

    val<1> reuse_predict2([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    // ---- update ----
    void update_condbr([[maybe_unused]] val<64> branch_pc, val<1> taken, [[maybe_unused]] val<64> next_pc) {
        // recompute the provider / alternate predictions from saved state
        arr<val<1>, NUMG> tpred = [&](u64 i) { return ctr_pred(ent_ctr(gent[i])); };
        val<1> base_pred = val<1>{bctr >> 1};
        val<NUMG + 1> preds = concat(base_pred, tpred.concat());
        val<1> pred1 = (match1 & preds) != hard<0>{};      // provider prediction
        val<1> altpred = (match2 & preds) != hard<0>{};    // alternate prediction
        val<1> mispredict = pred1 != taken;

        // (1) base bimodal: always nudge toward the outcome
        val<2> new_b = update_ctr(bctr, taken);
        val<1> update_base = new_b != bctr;

        // masks over the NUMG tagged tables -----------------------------------
        val<NUMG> provmask = val<NUMG>{match1};            // provider table (if any)
        // tables LONGER than the provider == bits below the provider bit.
        // match1-1 sets exactly those bits (and if provider==base, all tables).
        val<NUMG> postmask = val<NUMG>{match1 - 1};
        arr<val<1>, NUMG> ufree = [&](u64 i) { return ent_u(gent[i]) == hard<0>{}; };
        val<NUMG> notu = ufree.concat();
        val<NUMG> mispB = mispredict.replicate(hard<NUMG>{}).concat();

        // candidate allocation slots: longer than provider, not useful, on a miss
        val<NUMG> cand = postmask & notu & mispB;
        // pick the candidate closest to the provider (highest index == shortest
        // of the longer histories): reverse -> lowest -> reverse back
        val<NUMG> alloc = cand.reverse().one_hot().reverse();
        // if no free slot, clear u on the longer tables so a slot frees up later
        val<1> no_cand = cand == hard<0>{};
        val<NUMG> uclear = postmask & no_cand.replicate(hard<NUMG>{}).concat() & mispB;

        arr<val<1>, NUMG> prov  = provmask.make_array(val<1>{});
        arr<val<1>, NUMG> allo  = alloc.make_array(val<1>{});
        arr<val<1>, NUMG> uclr  = uclear.make_array(val<1>{});

        val<CTRBITS> weak_ctr = select(taken, val<CTRBITS>{4}, val<CTRBITS>{3});

        // build per-table write enable + data
        arr<val<1>, NUMG> we = [&](u64 i) { return prov[i] | allo[i] | uclr[i]; };
        arr<val<EW>, NUMG> wd = [&](u64 i) -> val<EW> {
            val<CTRBITS> ctr_i = ent_ctr(gent[i]);
            val<1> tp_i = ctr_pred(ctr_i);
            // provider: train counter; mark useful if it was right AND differed
            // from the alternate (i.e. it actually earned its keep)
            val<1> new_u = (tp_i == taken) & (pred1 != altpred);
            val<EW> trained = concat(gtag[i], update_ctr(ctr_i, taken), new_u);
            val<EW> allocated = concat(gtag[i], weak_ctr, val<1>{0});
            val<EW> ucleared = concat(ent_tag(gent[i]), ctr_i, val<1>{0});
            return select(prov[i], trained, select(allo[i], allocated, ucleared));
        };

        val<1> any_g = (provmask | alloc | uclear) != hard<0>{};
        need_extra_cycle(update_base | any_g);
        execute_if(update_base, [&]() { bht.write(bindex, new_b); });
        for (u64 i = 0; i < NUMG; i++) {
            execute_if(we[i], [&]() { gt[i].write(gindex[i], wd[i]); });
        }

        // advance the (folded) global history by this branch's direction
        gfolds.update(val<1>{taken});
    }

    void update_cycle([[maybe_unused]] instruction_info &block_end_info) {
    }
};