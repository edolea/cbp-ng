#include "../cbp.hpp"
#include "../harcom.hpp"

using namespace hcm;

template<u64 LOG_TABLE = 12, u64 HIST = 12>
struct my_pred : predictor {
    // TODO: see if u can add .fo1 to pc_branch, to t_update function, ...
    // static_assert(HIST <= LOG_TABLE);
    static constexpr u64 TABLE = 1 << LOG_TABLE;

    ram<val<2>, TABLE> bht;
    ram<val<2>, TABLE> global_bht;
    reg<HIST> global_history;
    ram<val<2>, TABLE> chooser;

    reg<2> t;
    reg<2> global_t;
    reg<2> choice;

    reg<LOG_TABLE> index;
    reg<LOG_TABLE> global_index;
    reg<LOG_TABLE> chooser_index;


    val<LOG_TABLE> get_index([[maybe_unused]] val<64> inst_pc) {
        return (inst_pc >> 2).make_array(val<LOG_TABLE>{}).fold_xor();
    }

    val<LOG_TABLE> get_global_index([[maybe_unused]] val<64> inst_pc) {
        return get_index(inst_pc) ^ val<LOG_TABLE>{global_history};
    }

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
        global_t = global_bht.read(global_index);

        // chooser_index = get_index(inst_pc); --> gain minus 50fJ on 12 bit LOG
        choice = chooser.read(index);
        return select(val<1>{choice >> 1}, global_t >> 1, t >> 1);  // 0 -> local; 1 --> global
    }

    val<1> reuse_predict2([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    val<2> update_t(val<2> old_t, val<1> taken) {
        val<2> increased = select(old_t == 3, old_t, val<2>{old_t + 1});
        val<2> decreased = select(old_t == 0, old_t, val<2>{old_t - 1});
        return select(taken, increased, decreased);
    }

    void update_condbr([[maybe_unused]] val<64> branch_pc, [[maybe_unused]] val<1> taken, [[maybe_unused]] val<64> next_pc) {
        val<2> new_t = update_t(t, taken);
        val<1> update_local = new_t != t;

        val<2> new_global_t = update_t(global_t, taken);
        val<1> update_global = new_global_t != global_t;

        val<1> global_was_taken = val<1>{global_t >> 1} == taken;
        val<2> new_choice = update_t(choice, global_was_taken);
        val<1> update_chooser = (t >> 1) != (global_t >> 1);

        need_extra_cycle(update_local | update_global | update_chooser);
        execute_if(update_local, [&]() {
            bht.write(index, new_t);
        });
        execute_if(update_global, [&]() {
            global_bht.write(global_index, new_global_t);
        });
        execute_if(update_chooser, [&](){
            chooser.write(index, new_choice);
        });

        global_history = (global_history << 1) | val<HIST>{taken};
    }

    void update_cycle([[maybe_unused]] instruction_info &block_end_info) {
    }
};