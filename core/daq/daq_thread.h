// core/daq/daq_thread.h
//
// Real-time DAQ hot loop in a dedicated thread. Replaces the single-
// threaded QTimer-driven acquisition in RunDialog. See Developer_Guide.md
// §4.4 for the thread model rationale.
//
// Responsibilities of the hot loop (one iteration per DAQ read):
//   1. readAI()              — pull the next chunk of scans from hardware
//   2. TNetwork::Update()    — integrate the model with the last sampled Vm
//   3. writeAO()             — close the loop with the computed currents
//                              (zeroed during Before/After phases)
//   4. submit chunk to ScanWriter (guaranteed delivery to disk)
//   5. invoke on_chunk callback (best-effort fanout to UI ring buffers)
//
// Design invariants:
//   • The hot loop allocates zero memory. All buffers are pre-allocated
//     in start(), sized from Config::max_scans_per_read.
//   • The loop never blocks on UI state. A full ScanWriter queue returns
//     false from submit() and aborts the run cleanly — the contract is
//     "no data loss, stop instead."
//   • The DAQInterface is configured (AI + AO) by the *caller* before
//     construction. This keeps policy (sample rate negotiation, channel
//     selection) in one place — RunDialog — and makes DaqThread unit-
//     testable with a DAQMock.

#ifndef DAQ_THREAD_H
#define DAQ_THREAD_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "daq_interface.h"
#include "rt_priority.h"
#include "scan_writer.h"

class TNetwork;

/// Background DAQ thread.
class DaqThread {
public:
    /// Per-scan on-disk layout is:
    ///   [ai_0 .. ai_{num_ai-1},
    ///    vm_0 .. vm_{num_cells-1},
    ///    i_0  .. i_{num_vdep_cells-1}]
    /// so each scan record is `scan_size` doubles wide.
    struct Config {
        DAQInterface *daq  = nullptr;   // not owned; already configured
        TNetwork     *net  = nullptr;   // not owned
        ScanWriter   *writer = nullptr; // optional; may be null for headless tests

        double   sample_rate_hz = 0.0;  // coerced rate returned by configureAI
        int32_t  num_ai_channels = 0;
        int32_t  num_cells = 0;         // total cells from DescribeNetwork
        int32_t  num_vdep_cells = 0;    // voltage-dependent (DAC output) cells

        /// Phase boundaries (seconds from run start). During "before" and
        /// "after", AO is driven with zeros regardless of what the network
        /// computes. During "during", the network's currents go out.
        double sec_before = 0.0;
        double sec_during = 1.0;
        double sec_after  = 0.0;

        /// Pre-allocation cap. Real DAQ chunks on NI-DAQmx are typically
        /// 50–200 scans; 4096 gives plenty of headroom.
        std::size_t max_scans_per_read = 4096;

        /// AO clip range (Volts). The hot loop saturates each I_nA
        /// scan against this range before writing. Defaults to ±10 V
        /// (X-series convention); start() overrides them on entry from
        /// the DAQ's reported aoRange() so unipolar USB boards
        /// (0–5 V on USB-6001/6008/6009) clip correctly. Set them
        /// explicitly in Config to override.
        double ao_clip_min = -10.0;
        double ao_clip_max =  10.0;
        /// If true, start() will override ao_clip_min/ao_clip_max from
        /// the DAQ's aoRange() (the usual behavior). Set false to
        /// force the values you supplied.
        bool ao_clip_from_daq = true;

        /// Optional per-chunk callback. Invoked on the DAQ thread after
        /// writeAO and after submit-to-disk, once per chunk. Must not
        /// block; is expected to push into SpscRing buffers for the UI.
        ///
        /// Pointers are valid only for the duration of the call:
        ///   ai_interleaved  — num_scans × num_ai doubles, scan-major
        ///   vm_out          — num_cells doubles, end-of-chunk state
        ///   i_na            — num_vdep_cells doubles, end-of-chunk state
        ///   time_ms_start   — monotonic DAQ time at the first scan of
        ///                     this chunk (scan_index / sample_rate)
        using OnChunkFn = void(*)(void* ctx,
                                  double time_ms_start,
                                  int32_t num_scans,
                                  const double* ai_interleaved,
                                  const double* vm_out,
                                  const double* i_na);
        OnChunkFn on_chunk     = nullptr;
        void*     on_chunk_ctx = nullptr;
    };

    explicit DaqThread(Config cfg);
    ~DaqThread();

    DaqThread(const DaqThread&) = delete;
    DaqThread& operator=(const DaqThread&) = delete;

    /// Starts the underlying DAQ task (daq->start()) and launches the
    /// thread. Returns false on configuration/start error; inspect
    /// last_error() for the reason.
    bool start();

    /// Requests shutdown, joins the thread, stops the DAQ. Idempotent.
    void stop();

    // --- Diagnostics (safe to call from any thread) --------------------

    uint64_t scans_acquired() const { return m_scans_acquired.load(); }
    uint64_t chunks_processed() const { return m_chunks_processed.load(); }
    uint64_t disk_rejects() const { return m_disk_rejects.load(); }

    /// False once the thread has hit a fatal error or been asked to stop
    /// after a disk rejection.
    bool ok() const { return m_ok.load(); }

    /// Monotonic elapsed DAQ time (seconds) based on scan counts.
    double elapsed_sec() const {
        if (m_sample_rate <= 0) return 0;
        return static_cast<double>(m_scans_acquired.load()) / m_sample_rate;
    }

    /// Platform report from the RT-priority elevation attempt.
    rt_priority::Report rt_report() const {
        std::lock_guard<std::mutex> g(m_err_mu);
        return m_rt_report;
    }

    std::string last_error() const;

    /// Returns true once the thread has exited (either cleanly or on error).
    bool done() const { return m_done.load(); }

private:
    void run();                          // thread body
    void assemble_scan_chunk(
        const double* ai_interleaved,
        int32_t num_scans,
        const double* vm_out,
        const double* i_na,
        std::vector<double>& out) const;
    void set_error(std::string msg);

    Config m_cfg;
    double m_sample_rate    = 0.0;
    double m_step_ms        = 0.0;
    int32_t m_scan_size     = 0;
    std::thread m_thread;

    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_ok{true};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_done{false};

    std::atomic<uint64_t> m_scans_acquired{0};
    std::atomic<uint64_t> m_chunks_processed{0};
    std::atomic<uint64_t> m_disk_rejects{0};

    mutable std::mutex m_err_mu;
    std::string m_last_error;
    rt_priority::Report m_rt_report{rt_priority::Result::NotSupported, ""};

    // Pre-allocated hot-loop buffers. Owned by DaqThread, touched only
    // by the DAQ thread after start().
    std::vector<double> m_ai_buf;         // max_scans_per_read * num_ai
    std::vector<double> m_vm_out;         // num_cells
    std::vector<double> m_i_na;           // num_vdep_cells
    std::vector<double> m_zero_ao;        // num_vdep_cells (pre-zeroed)
    std::vector<double> m_ao_chunk;       // max_scans_per_read * num_vdep_cells
};

#endif  // DAQ_THREAD_H
