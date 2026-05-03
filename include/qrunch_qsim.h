#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#  ifdef QRUNCH_QSIM_BUILD
#    define QRUNCH_QSIM_API __declspec(dllexport)
#  else
#    define QRUNCH_QSIM_API __declspec(dllimport)
#  endif
#else
#  define QRUNCH_QSIM_API __attribute__((visibility("default")))
#endif

/*
 * Run a quantum circuit and return shot-based measurement counts.
 *
 * circuit_json  — SimCircuit JSON produced by Qrunch's sim_circuit_json():
 *   {"name":"...","qubit_count":N,"bit_count":N,"ops":[
 *     {"type":"gate","name":"h","qubits":[0],"params":[]},
 *     {"type":"gate","name":"cx","qubits":[0,1],"params":[]},
 *     {"type":"measure","qubit":0,"bit":0}, ...
 *   ]}
 *
 * options_json  — simulation options:
 *   {"shots":1024,"seed":42,"max_fused_gate_size":2,"threads":0,
 *    "simd":"auto","return_statevector":false}
 *   "seed" may be absent or null for non-deterministic seeding.
 *   "threads" == 0 means "let qsim choose".
 *
 * result_json   — on success: caller must free with qrunch_qsim_free().
 *   {"success":true,"backend":"qsim","shots":N,
 *    "counts":{"00":N0,"11":N1,...},
 *    "metadata":{"nqubits":N,"threads":N,"simd":"avx"}}
 *
 * error_json    — on failure: caller must free with qrunch_qsim_free().
 *   {"success":false,"error":"<message>"}
 *
 * Returns 0 on success, nonzero on failure.
 */
QRUNCH_QSIM_API int qrunch_qsim_run_json(
    const char* circuit_json,
    const char* options_json,
    char**      result_json,
    char**      error_json
);

/* Free a buffer returned by qrunch_qsim_run_json. */
QRUNCH_QSIM_API void qrunch_qsim_free(char* ptr);

/* Null-terminated version string, e.g. "0.1.0". Never free this pointer. */
QRUNCH_QSIM_API const char* qrunch_qsim_version(void);

#ifdef __cplusplus
}
#endif
