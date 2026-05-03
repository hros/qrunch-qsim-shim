#define QRUNCH_QSIM_BUILD
#include "qrunch_qsim.h"

// nlohmann/json — single-header JSON library (extern/nlohmann/json.hpp)
#include "nlohmann/json.hpp"

// qsim — choose simulator implementation based on compile-time capability flags
#include "lib/circuit.h"
#include "lib/gates_qsim.h"
#include "lib/run_qsim.h"

#if defined(QRUNCH_QSIM_USE_AVX2)
#  include "lib/simulator_avx.h"
   using SimulatorImpl = qsim::SimulatorAVX<float>;
#elif defined(QRUNCH_QSIM_USE_SSE)
#  include "lib/simulator_sse.h"
   using SimulatorImpl = qsim::SimulatorSSE<float>;
#else
#  include "lib/simulator_basic.h"
   using SimulatorImpl = qsim::SimulatorBasic<float>;
#endif

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
using StateSpace = SimulatorImpl::StateSpace;
using State = StateSpace::State;

// ── Helpers ───────────────────────────────────────────────────────────────────

static char* heap_str(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// ── Gate mapping ──────────────────────────────────────────────────────────────

static Gate make_gate(const std::string& name,
                      const std::vector<unsigned>& q,
                      const std::vector<fp_type>& p,
                      unsigned time) {
    // Single-qubit gates
    if (name == "h")   return qsim::GateHd<fp_type>::Create(time, q[0]);
    if (name == "x")   return qsim::GateX<fp_type>::Create(time, q[0]);
    if (name == "y")   return qsim::GateY<fp_type>::Create(time, q[0]);
    if (name == "z")   return qsim::GateZ<fp_type>::Create(time, q[0]);
    if (name == "s")   return qsim::GateS<fp_type>::Create(time, q[0]);
    if (name == "t")   return qsim::GateT<fp_type>::Create(time, q[0]);
    if (name == "sdg") return qsim::GateSdg<fp_type>::Create(time, q[0]);
    if (name == "tdg") return qsim::GateTdg<fp_type>::Create(time, q[0]);
    // Parameterized single-qubit gates
    if (name == "rx")  return qsim::GateRX<fp_type>::Create(time, q[0], p[0]);
    if (name == "ry")  return qsim::GateRY<fp_type>::Create(time, q[0], p[0]);
    if (name == "rz")  return qsim::GateRZ<fp_type>::Create(time, q[0], p[0]);
    if (name == "p")   return qsim::GatePhase<fp_type>::Create(time, q[0], p[0]);
    // Two-qubit gates
    if (name == "cx")  return qsim::GateCNot<fp_type>::Create(time, q[0], q[1]);
    if (name == "swap") return qsim::GateSwap<fp_type>::Create(time, q[0], q[1]);
    throw std::runtime_error("qrunch_qsim: unsupported gate '" + name + "'");
}

// ── Core simulation ───────────────────────────────────────────────────────────

struct MeasureOp { unsigned qubit; unsigned bit; };

static std::string simulate(const json& cj, const json& oj) {
    const unsigned num_qubits = cj.at("qubit_count").get<unsigned>();
    const int      num_bits   = cj.at("bit_count").get<int>();
    const int      shots      = oj.value("shots", 1024);
    const int      max_fused  = oj.value("max_fused_gate_size", 2);
    int            num_threads = oj.value("threads", 0);
    if (num_threads <= 0) num_threads = 1;

    const bool has_seed = oj.contains("seed") && !oj["seed"].is_null();
    const uint64_t seed_val = has_seed ? oj["seed"].get<uint64_t>()
                                       : std::random_device{}();

    // Build circuit and collect measure ops
    Circuit circuit;
    circuit.num_qubits = num_qubits;
    std::vector<MeasureOp> measures;
    unsigned time = 0;

    for (const auto& op : cj.at("ops")) {
        const std::string type = op.at("type").get<std::string>();
        if (type == "gate") {
            const std::string name = op.at("name").get<std::string>();
            auto qubits = op.at("qubits").get<std::vector<unsigned>>();
            std::vector<fp_type> params;
            if (op.contains("params"))
                params = op["params"].get<std::vector<fp_type>>();
            circuit.gates.push_back(make_gate(name, qubits, params, time++));
        } else if (type == "measure") {
            measures.push_back({op.at("qubit").get<unsigned>(),
                                op.at("bit").get<unsigned>()});
        }
    }

    // Run the unitary part once; then sample from the resulting state vector.
    // qsim state vector qubit ordering: qubit 0 is the least-significant bit
    // of the state index, matching Qrunch's 0-based qubit indices.
    SimulatorImpl sim(static_cast<unsigned>(num_threads));
    StateSpace ss(static_cast<unsigned>(num_threads));

    State state = ss.Create(num_qubits);
    ss.SetStateZero(state);

    // QSimRunner::Run(param, factory, gates, state)
    typename qsim::QSimRunner::Parameter param;
    param.max_fused_size = static_cast<unsigned>(max_fused);
    param.num_threads    = static_cast<unsigned>(num_threads);
    param.verbosity      = 0;

    struct Factory {
        explicit Factory(unsigned t) : t(t) {}
        SimulatorImpl GetSimulator() const { return SimulatorImpl(t); }
        unsigned t;
    };

    if (!qsim::QSimRunner::Run(param, Factory(num_threads), circuit.gates, state))
        throw std::runtime_error("qsim runner failed");

    // Compute probability distribution over all 2^n basis states
    const uint64_t state_size = uint64_t(1) << num_qubits;
    std::vector<double> probs(state_size);
    double prob_sum = 0.0;
    for (uint64_t i = 0; i < state_size; i++) {
        auto amp = ss.GetAmplitude(state, i);
        double p = static_cast<double>(std::norm(amp));
        probs[i] = p;
        prob_sum += p;
    }
    // Normalise to guard against floating-point drift
    if (prob_sum > 0.0)
        for (auto& p : probs) p /= prob_sum;

    // Sample shots times
    std::mt19937_64 rng(seed_val);
    std::discrete_distribution<uint64_t> dist(probs.begin(), probs.end());

    std::map<std::string, int> counts;
    std::vector<int> bit_outcomes(static_cast<size_t>(num_bits), 0);

    for (int s = 0; s < shots; s++) {
        const uint64_t idx = dist(rng);
        for (const auto& m : measures)
            bit_outcomes[m.bit] = static_cast<int>((idx >> m.qubit) & 1u);
        std::string key;
        key.reserve(static_cast<size_t>(num_bits));
        for (int b = 0; b < num_bits; b++)
            key += static_cast<char>('0' + bit_outcomes[b]);
        counts[key]++;
    }

    ss.Free(state);

    // Serialise result
    json result;
    result["success"] = true;
    result["backend"] = "qsim";
    result["shots"]   = shots;
    result["counts"]  = counts;
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
        const std::string result = simulate(cj, oj);
        *result_json = heap_str(result);
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

QRUNCH_QSIM_API void qrunch_qsim_free(char* ptr) {
    std::free(ptr);
}

QRUNCH_QSIM_API const char* qrunch_qsim_version(void) {
    return "0.1.0";
}

} // extern "C"
