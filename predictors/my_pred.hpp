#include "../cbp.hpp"
#include "../harcom.hpp"

using namespace hcm;

template<u64 LOG_TABLE = 8>
struct my_pred : predictor {
    static constexpr u64 TABLE = 1 << LOG_TABLE;

    ram<val<2>, TABLE> bht;
    reg<2> t;

    val<LOG_TABLE> get_index([[maybe_unused]] val<64> inst_pc) {
        return inst_pc.make_array(val<LOG_TABLE>{}).fold_xor();
    }

    val<1> predict1([[maybe_unused]] val<64> inst_pc) {
        val<LOG_TABLE> index = get_index(inst_pc);
        t = bht.read(index);
        return t >> 1;
    }

    val<1> reuse_predict1([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    val<1> predict2([[maybe_unused]] val<64> inst_pc) {
        return t >> 1;
    }

    val<1> reuse_predict2([[maybe_unused]] val<64> inst_pc) {
        return hard<0>{};
    }

    void update_condbr([[maybe_unused]] val<64> branch_pc, [[maybe_unused]] val<1> taken, [[maybe_unused]] val<64> next_pc) {
        val<2> increased = select(t == 3, t, val<2>{t + 1});
        val<2> decreased = select(t == 0, t, val<2>{t - 1});
        val<2> new_t = select(taken, increased, decreased);

        need_extra_cycle(val<1>{1});
        bht.write(get_index(branch_pc), new_t);
    }

    void update_cycle([[maybe_unused]] instruction_info &block_end_info) {
    }
};