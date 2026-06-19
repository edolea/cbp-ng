#include "../cbp.hpp"
#include "../harcom.hpp"

using namespace hcm;

template<u64 LOG_TABLE = 12, u64 HIST = 12>
struct my_pred_2 : predictor {
    static constexpr u64 TABLE = 1 << LOG_TABLE;

    ram<val<2>, TABLE> bht;
    ram<val<2>, TABLE> global_bht;
    ram<val<2>, TABLE> chooser;      // 0,1 = trust bimodal; 2,3 = trust gshare

    reg<HIST>       global_history;
    reg<2>          t;
    reg<2>          global_t;
    reg<2>          choice;          // chooser counter read at predict time

    reg<LOG_TABLE>  index;
    reg<LOG_TABLE>  global_index;
    reg<LOG_TABLE>  chooser_index;   // saved so update writes to same entry

    val<LOG_TABLE> get_index(val<64> inst_pc) {
        return (inst_pc >> 2).make_array(val<LOG_TABLE>{}).fold_xor();
    }

    val<LOG_TABLE> get_global_index(val<64> inst_pc) {
        return get_index(inst_pc) ^ val<LOG_TABLE>{global_history};
    }

    val<1> predict1(val<64> inst_pc) {
        index = get_index(inst_pc);
        t = bht.read(index);
        return t >> 1;
    }

    val<1> reuse_predict1([[maybe_unused]] val<64> inst_pc) { return hard<0>{}; }

    val<1> predict2(val<64> inst_pc) {
        global_index  = get_global_index(inst_pc);
        global_t      = global_bht.read(global_index);

        chooser_index = get_index(inst_pc);     // same PC hash, no history
        choice        = chooser.read(chooser_index);

        // high bit of chooser: 0 = trust bimodal, 1 = trust gshare
        val<1> use_gshare = choice >> 1;
        return select(use_gshare.fo1(), global_t >> 1, t >> 1);
    }

    val<1> reuse_predict2([[maybe_unused]] val<64> inst_pc) { return hard<0>{}; }

    val<2> sat_update(val<2> ctr, val<1> inc) {
        val<2> up   = select(ctr == hard<3>{}, ctr, val<2>{ctr + 1});
        val<2> down = select(ctr == hard<0>{}, ctr, val<2>{ctr - 1});
        return select(inc, up.fo1(), down.fo1());
    }

    void update_condbr([[maybe_unused]] val<64> branch_pc, val<1> taken,
                       [[maybe_unused]] val<64> next_pc) {

        val<2> new_t        = sat_update(t, taken);
        val<2> new_global_t = sat_update(global_t, taken);

        val<1> bimodal_was_right = val<1>{t >> 1}        == taken;
        val<1> gshare_was_right  = val<1>{global_t >> 1} == taken;
        val<1> they_disagree     = bimodal_was_right != gshare_was_right;

        // update chooser only when they disagreed; increment toward gshare if gshare was right
        val<2> new_choice   = sat_update(choice, gshare_was_right);
        val<1> update_ch = they_disagree;

        val<1> update_cur    = val<1>{new_t        != t};
        val<1> update_global = val<1>{new_global_t != global_t};

        need_extra_cycle(update_cur | update_global | update_ch);

        execute_if(update_cur, [&](){
            bht.write(index, new_t);
        });
        execute_if(update_global, [&](){
            global_bht.write(global_index, new_global_t);
        });
        execute_if(update_ch.fo1(), [&](){
            chooser.write(chooser_index, new_choice.fo1());
        });

        global_history = (global_history << 1) | val<HIST>{taken};
    }

    void update_cycle([[maybe_unused]] instruction_info &block_end_info) {}
};