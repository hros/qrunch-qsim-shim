#define QRUNCH_QSIM_BUILD
#include "qrunch_qsim.h"

#include "nlohmann/json.hpp"

// qsim headers — simmux auto-selects AVX512/AVX2/SSE/basic based on
// compile-time CPU feature flags (-mavx2 etc. set in CMakeLists.txt).
#include "lib/circuit.h"
#include "lib/formux.h"        // qsim::For (SequentialFor or ParallelFor)
#include "lib/fuser_mqubit.h"  // MultiQubitGateFuser
#include "lib/gates_qsim.h"
#include "lib/io.h"            // qsim::IO (stderr/stdout logger)
#include "lib/run_qsim.h"      // QSimRunner
#include "lib/simmux.h"        // qsim::Simulator<For>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using fp_type = float;
using Gate = qsim::GateQSim<fp_type>;
using Circuit = qsim::Circuit<Gate>;

// Silence qsim's own verbose output inside the library.
namespace { struct SuppressQsimOutput { SuppressQsimOutput() { qsim::output::enabled = false; } } _suppress; }

// ── Factory / runner types (following qsim_base.cc example) ──────────────────

struct Factory {
    explicit Factory(unsigned t) : num_threads(t) {}
    using Simulator = qsim::Simulator<qsim::For>;
    using StateSpace = Simulator::StateSpace;
    StateSpace CreateStateSpace() const { return StateSpace(num_threads); }
    Simulator  CreateSimulator()  const { return Simulator(num_threads); }
    unsigned num_threads;
};

using Simulator  = Factory::Simulator;
using StateSpace = Factory::StateSpace;
using State      = StateSpace::State;
using Fuser      = qsim::MultiQubitGateFuser<qsim::IO, Gate>;
using Runner     = qsim::QSimRunner<qsim::IO, Fuser, Factory>;

// ── Helpers ───────────────────────────────────────────────────────────────────

static char* heap_str(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// Build a one-qubit GateMatrix1 from a 2×2 complex matrix given in row-major
// (re, im) pairs: {re(m00), im(m00), re(m01), im(m01), re(m10), im(m10), re(m11), im(m11)}.
static Gate matrix1(unsigned time, unsigned q,
                    fp_type r00, fp_type i00, fp_type r01, fp_type i01,
                    fp_type r10, fp_type i10, fp_type r11, fp_type i11) {
    return qsim::GateMatrix1<fp_type>::Create(
        time, q, qsim::Matrix<fp_type>{r00, i00, r01, i01, r10, i10, r11, i11});
}

// ── Gate mapping ──────────────────────────────────────────────────────────────

static Gate make_gate(const std::string& name,
                      const std::vector<unsigned>& q,
                      const std::vector<fp_type>& p,
                      unsigned time) {
    static const fp_type INVSQRT2 = static_cast<fp_type>(0.70710678118654752440);

    // Single-qubit gates
    if (name == "h")   return qsim::GateHd<fp_type>::Create(time, q[0]);
    if (name == "x")   return qsim::GateX<fp_type>::Create(time, q[0]);
    if (name == "y")   return qsim::GateY<fp_type>::Create(time, q[0]);
    if (name == "z")   return qsim::GateZ<fp_type>::Create(time, q[0]);
    if (name == "s")   return qsim::GateS<fp_type>::Create(time, q[0]);
    if (name == "t")   return qsim::GateT<fp_type>::Create(time, q[0]);
    // S† = [[1,0],[0,−i]]
    if (name == "sdg") return matrix1(time, q[0], 1,0, 0,0, 0,0, 0,-1);
    // T† = [[1,0],[0,e^{−iπ/4}]] = [[1,0],[0, 1/√2 − i/√2]]
    if (name == "tdg") return matrix1(time, q[0], 1,0, 0,0, 0,0, INVSQRT2,-INVSQRT2);
    // Parameterised single-qubit gates
    if (name == "rx")  return qsim::GateRX<fp_type>::Create(time, q[0], p[0]);
    if (name == "ry")  return qsim::GateRY<fp_type>::Create(time, q[0], p[0]);
    if (name == "rz")  return qsim::GateRZ<fp_type>::Create(time, q[0], p[0]);
    // P(θ) = [[1,0],[0,e^{iθ}]]
    if (name == "p") {
        fp_type c = std::cos(p[0]), s = std::sin(p[0]);
        return matrix1(time, q[0], 1,0, 0,0, 0,0, c,s);
    }
    // Two-qubit gates (qsim uses qubit[0] as the control for CNOT)
    if (name == "cx")   return qsim::GateCNot<fp_type>::Create(time, q[0], q[1]);
    if (name == "swap") return qsim::GateSwap<fp_type>::Create(time, q[0], q[1]);

    throw std::runtime_error("qrunch_qsim: unsupported gate '" + name + "'");
}

// ── Measure op bookkeeping ────────────────────────────────────────────────────

struct MeasureOp { unsigned qubit; unsigned bit; };

// ── Core simulation ───────────────────────────────────────────────────────────

static std::string simulate(const json& cj, const json& oj) {
    const unsigned num_qubits = cj.at("qubit_count").get<unsigned>();
    const int      num_bits   = cj.at("bit_count").get<int>();
    const int      shots      = oj.value("shots", 1024);
    const int      max_fused  = oj.value("max_fused_gate_size", 2);
    int            num_threads = oj.value("threads", 1);
    if (num_threads <= 0) num_threads = 1;

    const bool has_seed = oj.contains("seed") && !oj["seed"].is_null();
    const uint64_t seed_val = has_seed ? oj["seed"].get<uint64_t>()
                                       : static_cast<uint64_t>(std::random_device{}());

    // Parse circuit into segments split at reset boundaries.
    // Mid-circuit reset is non-unitary — we run the circuit in segments,
    // measure + collapse + conditional-X at each reset boundary.
    std::vector<std::vector<Gate>> segments;
    std::vector<unsigned> reset_qubits;
    std::vector<Gate> current_gates;
    std::vector<MeasureOp> measures;
    unsigned time = 0;

    for (const auto& op : cj.at("ops")) {
        const std::string type = op.at("type").get<std::string>();
        if (type == "gate") {
            const std::string name = op.at("name").get<std::string>();
            if (name == "reset") {
                auto qubits = op.at("qubits").get<std::vector<unsigned>>();
                segments.push_back(current_gates);
                reset_qubits.push_back(qubits[0]);
                current_gates.clear();
            } else {
                auto qubits = op.at("qubits").get<std::vector<unsigned>>();
                std::vector<fp_type> params;
                if (op.contains("params"))
                    params = op.at("params").get<std::vector<fp_type>>();
                current_gates.push_back(make_gate(name, qubits, params, time++));
            }
        } else if (type == "measure") {
            measures.push_back({op.at("qubit").get<unsigned>(),
                                op.at("bit").get<unsigned>()});
        }
    }
    segments.push_back(current_gates);  // final segment (no reset after)

    // Allocate state and run each segment, handling resets between them.
    // qsim's AVX state vector layout: qubit 0 is the least-significant bit of
    // the state index, matching Qrunch's 0-based qubit numbering.
    Factory factory(static_cast<unsigned>(num_threads));
    StateSpace state_space = factory.CreateStateSpace();
    State state = state_space.Create(num_qubits);

    if (state_space.IsNull(state))
        throw std::runtime_error("qrunch_qsim: not enough memory for " +
                                 std::to_string(num_qubits) + " qubits");
    state_space.SetStateZero(state);

    Runner::Parameter param;
    param.max_fused_size = static_cast<unsigned>(max_fused);
    param.seed           = static_cast<unsigned>(seed_val & 0xFFFFFFFFu);
    param.verbosity      = 0;

    // Create the sampling RNG early — it is also used for reset measurements
    // so the entire run (resets + final sampling) is seed-deterministic.
    std::mt19937_64 rng(seed_val);

    for (size_t seg_idx = 0; seg_idx < segments.size(); seg_idx++) {
        if (!segments[seg_idx].empty()) {
            Circuit seg_circuit;
            seg_circuit.num_qubits = num_qubits;
            seg_circuit.gates = segments[seg_idx];
            if (!Runner::Run(param, factory, seg_circuit, state))
                throw std::runtime_error("qrunch_qsim: segment run failed");
        }

        // Handle mid-circuit reset after this segment.
        if (seg_idx < reset_qubits.size()) {
            unsigned q = reset_qubits[seg_idx];
            std::vector<unsigned> qvec = {q};
            auto result = state_space.Measure(qvec, rng, state);
            if (result.valid && !result.bitstring.empty() && result.bitstring[0] == 1) {
                Circuit x_circuit;
                x_circuit.num_qubits = num_qubits;
                x_circuit.gates.push_back(qsim::GateX<fp_type>::Create(time++, q));
                if (!Runner::Run(param, factory, x_circuit, state))
                    throw std::runtime_error("qrunch_qsim: reset X gate failed");
            }
        }
    }

    // Compute probability distribution from the final state vector.
    const uint64_t state_size = uint64_t(1) << num_qubits;
    std::vector<double> probs(state_size);
    double prob_sum = 0.0;
    for (uint64_t i = 0; i < state_size; i++) {
        auto amp = StateSpace::GetAmpl(state, i);
        double p = static_cast<double>(std::norm(amp));
        probs[i] = p;
        prob_sum += p;
    }
    if (prob_sum > 0.0)
        for (auto& p : probs) p /= prob_sum;

    // Sample shots times using the already-seeded Mersenne-Twister.
    std::discrete_distribution<uint64_t> dist(probs.begin(), probs.end());

    std::map<std::vector<int>, int> counts;
    std::vector<int> bit_outcomes(static_cast<size_t>(num_bits), 0);

    for (int s = 0; s < shots; s++) {
        const uint64_t idx = dist(rng);
        for (const auto& m : measures)
            bit_outcomes[m.bit] = static_cast<int>((idx >> m.qubit) & 1u);
        counts[bit_outcomes]++;
    }

    // Serialise result JSON with structured measurement data.
    json result;
    result["success"] = true;
    result["backend"] = "qsim";
    result["shots"]   = shots;
    result["bit_count"] = num_bits;

    json results_array = json::array();
    for (const auto& [bits, count] : counts) {
        json entry;
        entry["bits"] = bits;
        entry["count"] = count;
        results_array.push_back(entry);
    }
    result["results"] = results_array;
    result["metadata"]["nqubits"] = num_qubits;
    result["metadata"]["threads"] = num_threads;
    return result.dump();
}

// ── C ABI ─────────────────────────────────────────────────────────────────────

extern "C" {

QRUNCH_QSIM_API int qrunch_qsim_run_json(
    const char* circuit_json,
    const char* options_json,
    char**      result_json,
    char**      error_json)
{
    *result_json = nullptr;
    *error_json  = nullptr;
    try {
        const auto cj = json::parse(circuit_json);
        const auto oj = json::parse(options_json);
        *result_json = heap_str(simulate(cj, oj));
        return 0;
    } catch (const std::exception& e) {
        json err;
        err["success"] = false;
        err["error"]   = e.what();
        *error_json = heap_str(err.dump());
        return 1;
    } catch (...) {
        json err;
        err["success"] = false;
        err["error"]   = "qsim: unknown exception";
        *error_json = heap_str(err.dump());
        return 1;
    }
}

QRUNCH_QSIM_API void qrunch_qsim_free(char* ptr) { std::free(ptr); }

QRUNCH_QSIM_API const char* qrunch_qsim_version(void) { return "0.1.0"; }

} // extern "C"
