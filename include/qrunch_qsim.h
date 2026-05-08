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
 *    "results":[{"bits":[0,1],"count":50},...],
 *    "bit_count":N,
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

/*
 * Capability bitmask constants for qrunch_qsim_capabilities().
 * Consumers bitwise-AND the return value against these flags.
 */
#define QRUNCH_QSIM_CAP_UNITARY       (1 << 0)  /* standard unitary gates        */
#define QRUNCH_QSIM_CAP_RESET         (1 << 1)  /* mid-circuit reset              */
#define QRUNCH_QSIM_CAP_GPU_CUQUANTUM (1 << 2)  /* reserved: GPU / cuQuantum      */

/*
 * Return the C ABI version of the shim as a simple integer.
 * Increment this whenever the ABI surface changes incompatibly.
 * Current version: 1.
 */
QRUNCH_QSIM_API int qrunch_qsim_version(void);

/*
 * Return a bitmask of supported features (see QRUNCH_QSIM_CAP_* above).
 * Callers can test specific features with (caps & QRUNCH_QSIM_CAP_UNITARY) etc.
 */
QRUNCH_QSIM_API int qrunch_qsim_capabilities(void);

#ifdef __cplusplus
}
#endif
