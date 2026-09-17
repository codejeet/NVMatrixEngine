#include <cuda_runtime.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(cudaError_t result, const char *operation) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(result));
}
__global__ void increment(unsigned *state) {
    if (state[1] < state[0])
        ++state[1];
}
__global__ void condition(unsigned *state, cudaGraphConditionalHandle handle) {
    cudaGraphSetConditional(handle, state[1] < state[0]);
}
struct Fixture {
    cudaStream_t stream = nullptr, capture = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t executable = nullptr;
    unsigned *state = nullptr;
    ~Fixture() {
        if (stream)
            cudaStreamSynchronize(stream);
        if (executable)
            cudaGraphExecDestroy(executable);
        if (graph)
            cudaGraphDestroy(graph);
        if (state)
            cudaFree(state);
        if (capture)
            cudaStreamDestroy(capture);
        if (stream)
            cudaStreamDestroy(stream);
    }
    void create(bool conditional) {
        check(cudaMalloc(&state, 2 * sizeof(unsigned)), "allocate");
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "stream");
        check(cudaStreamCreateWithFlags(&capture, cudaStreamNonBlocking), "capture stream");
        check(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal), "begin capture");
        if (conditional) {
            cudaStreamCaptureStatus status{};
            check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &graph), "capture graph");
            cudaGraphConditionalHandle handle{};
            check(cudaGraphConditionalHandleCreate(&handle, graph, 0, cudaGraphCondAssignDefault), "handle");
            condition<<<1, 1, 0, stream>>>(state, handle);
            const cudaGraphNode_t *dependencies = nullptr;
            const cudaGraphEdgeData *edges = nullptr;
            size_t count = 0;
            check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &graph, &dependencies, &edges, &count),
                  "dependencies");
            cudaGraphNodeParams params{};
            params.type = cudaGraphNodeTypeConditional;
            params.conditional.handle = handle;
            params.conditional.type = cudaGraphCondTypeWhile;
            params.conditional.size = 1;
            cudaGraphNode_t node{};
            check(cudaGraphAddNode(&node, graph, dependencies, edges, count, &params), "while node");
            const auto body = params.conditional.phGraph_out[0];
            check(cudaStreamBeginCaptureToGraph(capture, body, nullptr, nullptr, 0,
                                                cudaStreamCaptureModeThreadLocal),
                  "begin body");
            increment<<<1, 1, 0, capture>>>(state);
            condition<<<1, 1, 0, capture>>>(state, handle);
            cudaGraph_t ended{};
            check(cudaStreamEndCapture(capture, &ended), "end body");
            if (ended != body)
                throw std::runtime_error("body ownership changed");
            check(cudaStreamUpdateCaptureDependencies(stream, &node, nullptr, 1,
                                                      cudaStreamSetCaptureDependencies),
                  "join while");
        } else {
            for (unsigned i = 0; i < 32; ++i)
                increment<<<1, 1, 0, stream>>>(state);
        }
        check(cudaStreamEndCapture(stream, &graph), "end graph");
        check(cudaGraphInstantiate(&executable, graph, 0), "instantiate");
    }
    void replay(unsigned iterations) {
        std::array<unsigned, 2> value{iterations, 0};
        check(cudaMemcpyAsync(state, value.data(), sizeof(value), cudaMemcpyHostToDevice, stream), "input");
        check(cudaGraphLaunch(executable, stream), "launch");
        check(cudaMemcpyAsync(value.data(), state, sizeof(value), cudaMemcpyDeviceToHost, stream), "output");
        check(cudaStreamSynchronize(stream), "replay synchronize");
        if (value[0] != iterations || value[1] != iterations)
            throw std::runtime_error("incorrect replay iteration count");
    }
};
} // namespace
int main(int argc, char **argv) try {
    const bool conditional = !(argc == 2 && std::strcmp(argv[1], "--unrolled") == 0);
    Fixture test;
    test.create(conditional);
    for (unsigned iterations : {0u, 1u, 7u, 32u, 0u, 8u}) {
        std::cout << "RUN " << (conditional ? "conditional" : "unrolled") << " iterations=" << iterations
                  << std::endl;
        test.replay(iterations);
    }
    std::cout << "PASS CUDA standalone graph replay\n";
} catch (const std::exception &error) {
    std::cerr << "FAIL CUDA standalone graph replay: " << error.what() << '\n';
    return 1;
}
