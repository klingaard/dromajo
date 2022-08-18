/*
 * Top-level driver
 *
 * Copyright (C) 2018,2019, Esperanto Technologies Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "dromajo.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <unordered_map>

#include "LiveCacheCore.h"
#include "cutils.h"
#include "iomem.h"
#include "riscv_machine.h"
#include "virtio.h"

//#define REGRESS_COSIM 1
#ifdef REGRESS_COSIM
#include "dromajo_cosim.h"
#endif


#include "trace_markers.h"
#include "stf-inc/stf_writer.hpp"
#include "stf-inc/stf_record_types.hpp"

#ifdef SIMPOINT_BB
FILE *simpoint_bb_file = nullptr;
int   simpoint_roi     = 0;  // start without ROI enabled

int simpoint_step(RISCVMachine *m, int hartid) {
    assert(hartid == 0);  // Only single core for simpoint creation

    static uint64_t ninst = 0;  // ninst in BB
    ninst++;

    if (simpoint_bb_file == 0) {  // Creating checkpoints mode

        assert(!m->common.simpoints.empty());

        auto &sp = m->common.simpoints[m->common.simpoint_next];
        if (ninst > sp.start) {
            char str[100];
            sprintf(str, "sp%d", sp.id);
            virt_machine_serialize(m, str);

            m->common.simpoint_next++;
            if (m->common.simpoint_next == m->common.simpoints.size()) {
                return 0;  // notify to terminate nicely
            }
        }
        return 1;
    }

    // Creating bb trace mode
    assert(m->common.simpoints.empty());

    uint64_t                                 pc            = virt_machine_get_pc(m, hartid);
    static uint64_t                          next_bbv_dump = UINT64_MAX;
    static std::unordered_map<uint64_t, int> bbv;
    static std::unordered_map<uint64_t, int> pc2id;
    static int                               next_id = 1;
    if (m->common.maxinsns <= next_bbv_dump) {
        if (m->common.maxinsns > SIMPOINT_SIZE)
            next_bbv_dump = m->common.maxinsns - SIMPOINT_SIZE;
        else
            next_bbv_dump = 0;

        if (bbv.size()) {
            fprintf(simpoint_bb_file, "T");
            for (const auto ent : bbv) {
                auto it = pc2id.find(ent.first);
                int  id = 0;
                if (it == pc2id.end()) {
                    id = next_id;
                    next_id++;
                    pc2id[ent.first] = next_id;
                } else {
                    id = it->second;
                }

                fprintf(simpoint_bb_file, ":%d:%d ", id, ent.second);
            }
            fprintf(simpoint_bb_file, "\n");
            fflush(simpoint_bb_file);
            bbv.clear();
        }
    }

    static uint64_t last_pc = 0;
    if ((last_pc + 2) != pc && (last_pc + 4) != pc) {
        bbv[last_pc] += ninst;
        // fprintf(simpoint_bb_file,"xxxBB 0x%" PRIx64 " %d\n", pc, ninst);
        ninst = 0;
    }
    last_pc = pc;

    return 1;
}
#endif

////////////////////////////////////////////////////////////////////////////////
// STF Writing
stf::STFWriter stf_writer;
bool tracing_enabled = false;
uint64_t stf_prog_asid = 0;
uint64_t stf_count = 0;
////////////////////////////////////////////////////////////////////////////////

int iterate_core(RISCVMachine *m, int hartid) {
    if (m->common.maxinsns-- <= 0)
        /* Succeed after N instructions without failure. */
        return 0;

    RISCVCPUState *cpu = m->cpu_state[hartid];

    /* Instruction that raises exceptions should be marked as such in
     * the trace of retired instructions.
     */
    uint64_t last_pc  = virt_machine_get_pc(m, hartid);
    int      priv     = riscv_get_priv_level(cpu);
    uint32_t insn_raw = -1;
    (void)riscv_read_insn(cpu, &insn_raw, last_pc);
    int keep_going = virt_machine_run(m, hartid);
    if (last_pc == virt_machine_get_pc(m, hartid))
        return 0;

    if(m->common.stf_trace) {
        // check for start trace marker
        if(insn_raw == START_TRACE_OPC)
        {
            tracing_enabled = true;
            stf_prog_asid = (cpu->satp >> 4) & 0xFFFF;
            if((bool)stf_writer == false) {
                stf_writer.open(m->common.stf_trace);
                stf_writer.
                    addTraceInfo(stf::TraceInfoRecord(stf::STF_GEN::STF_GEN_GEM5, 1, 1, 0,
                                                      "Trace actually from Dromajo"));
                stf_writer.setISA(stf::ISA::RISCV);
                stf_writer.setVLen(64);
                stf_writer.setHeaderIEM(stf::INST_IEM::STF_INST_IEM_RV64);
                stf_writer.setTraceFeature(stf::TRACE_FEATURES::STF_CONTAIN_RV64);
                stf_writer.setTraceFeature(stf::TRACE_FEATURES::STF_CONTAIN_PHYSICAL_ADDRESS);
                stf_writer.setHeaderPC(virt_machine_get_pc(m, 0));
                stf_writer.finalizeHeader();
            }

            return keep_going;
        }
        else if (insn_raw == STOP_TRACE_OPC) {
            tracing_enabled = false;
            stf_writer.close();
            fprintf(dromajo_stderr, "Traced %ld insts\n", stf_count);
        }

        if(tracing_enabled)
        {
            // Only trace in user priv and the same application that
            // started the trace
            if((priv == 0) &&
               (cpu->pending_exception == -1) &&
               (stf_prog_asid == ((cpu->satp >> 4) & 0xFFFF)))
            {
                ++stf_count;
                const uint32_t inst_width = ((insn_raw & 0x3) == 0x3) ? 4 : 2;
                bool skip_record = false;

                // See if the instruction changed control flow or a
                // possible not-taken branch conditional
                if(cpu->info != ctf_nop) {
                    stf_writer << stf::InstPCTargetRecord(virt_machine_get_pc(m, 0));
                }
                else {
                    // Not sure what's going on, but there's a
                    // possibility that the current instruction will
                    // cause a page fault or a timer interrupt or
                    // process switch so the next instruction might
                    // not be on the program's path
                    if(cpu->pc != last_pc + inst_width) {
                        skip_record = true;
                    }
                }

                // Record the instruction trace record
                if(false == skip_record)
                {
                    // If the last instruction were a load/store,
                    // record the last vaddr, size, and if it were a
                    // read or write.
                    if(cpu->last_data_vaddr != std::numeric_limits<decltype(cpu->last_data_vaddr)>::max())
                    {
                        stf_writer << stf::InstMemAccessRecord(cpu->last_data_vaddr,
                                                               cpu->last_data_size,
                                                               0,
                                                               (cpu->last_data_type == 0) ?
                                                               stf::INST_MEM_ACCESS::READ :
                                                               stf::INST_MEM_ACCESS::WRITE);
                        stf_writer << stf::InstMemContentRecord(0); // empty content for now
                    }

                    if(inst_width == 4) {
                        stf_writer << stf::InstOpcode32Record(insn_raw);
                    }
                    else {
                        stf_writer << stf::InstOpcode16Record(insn_raw & 0xFFFF);
                    }
                }
            }
        }
        return keep_going;
    }

    if (m->common.trace) {
        --m->common.trace;
        return keep_going;
    }

    fprintf(dromajo_stderr,
            "%d %d 0x%016" PRIx64 " (0x%08x)",
            hartid,
            priv,
            last_pc,
            (insn_raw & 3) == 3 ? insn_raw : (uint16_t)insn_raw);

    int iregno = riscv_get_most_recently_written_reg(cpu);
    int fregno = riscv_get_most_recently_written_fp_reg(cpu);

    if (cpu->pending_exception != -1)
        fprintf(dromajo_stderr,
                " exception %d, tval %016" PRIx64,
                cpu->pending_exception,
                riscv_get_priv_level(cpu) == PRV_M ? cpu->mtval : cpu->stval);
    else if (iregno > 0)
        fprintf(dromajo_stderr, " x%2d 0x%016" PRIx64, iregno, virt_machine_get_reg(m, hartid, iregno));
    else if (fregno >= 0)
        fprintf(dromajo_stderr, " f%2d 0x%016" PRIx64, fregno, virt_machine_get_fpreg(m, hartid, fregno));

    putc('\n', dromajo_stderr);

    return keep_going;
}

static double execution_start_ts;
static uint64_t *execution_progress_meassure;


static void sigintr_handler(int dummy) {
    double t = get_current_time_in_seconds();
    fprintf(dromajo_stderr, "Simulation speed: %5.2f MIPS (single-core)\n",
            1e-6 * *execution_progress_meassure / (t - execution_start_ts));
    exit(1);
}

int main(int argc, char **argv) {
#ifdef REGRESS_COSIM
    dromajo_cosim_state_t *costate = 0;
    costate                        = dromajo_cosim_init(argc, argv);

    if (!costate)
        return 1;

    while (!dromajo_cosim_step(costate, 0, 0, 0, 0, 0, false))
        ;
    dromajo_cosim_fini(costate);
#else
    RISCVMachine *m = virt_machine_main(argc, argv);

#ifdef SIMPOINT_BB
    if (m->common.simpoints.empty()) {
        simpoint_bb_file = fopen("dromajo_simpoint.bb", "w");
        if (simpoint_bb_file == nullptr) {
            fprintf(dromajo_stderr, "\nerror: could not open dromajo_simpoint.bb for dumping trace\n");
            exit(-3);
        }
    }
#endif

    if (!m)
        return 1;

    execution_start_ts = get_current_time_in_seconds();
    execution_progress_meassure = &m->cpu_state[0]->minstret;
    signal(SIGINT, sigintr_handler);

    int keep_going;
    do {
        keep_going = 0;
        for (int i = 0; i < m->ncpus; ++i) keep_going |= iterate_core(m, i);
#ifdef SIMPOINT_BB
        if (simpoint_roi) {
            if (!simpoint_step(m, 0))
                break;
        }
#endif
    } while (keep_going);

    double t = get_current_time_in_seconds();

    for (int i = 0; i < m->ncpus; ++i) {
        int benchmark_exit_code = riscv_benchmark_exit_code(m->cpu_state[i]);
        if (benchmark_exit_code != 0) {
            fprintf(dromajo_stderr, "\nBenchmark exited with code: %i \n", benchmark_exit_code);
            return 1;
        }
    }

    fprintf(dromajo_stderr, "Simulation speed: %5.2f MIPS (single-core)\n",
            1e-6 * *execution_progress_meassure / (t - execution_start_ts));

    fprintf(dromajo_stderr, "\nPower off.\n");

    virt_machine_end(m);
#endif

#ifdef LIVECACHE
#if 0
    // LiveCache Dump
    uint64_t addr_size;
    uint64_t *addr = m->llc->traverse(addr_size);

    for (uint64_t i = 0u; i < addr_size; ++i) {
        printf("addr:%llx %s\n", (unsigned long long)addr[i], (addr[i] & 1) ? "ST" : "LD");
    }
#endif
    delete m->llc;
#endif

    return 0;
}
