/*
 * Copyright (C) 2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "framework/test_case/register_test_case.h"
#include "framework/ur/error.h"
#include "framework/ur/ur.h"
#include "framework/utility/file_helper.h"
#include "framework/utility/timer.h"

#include "definitions/submit_kernel.h"

#include <gtest/gtest.h>
#include <pthread.h>

static constexpr size_t n_dimensions = 3;
static constexpr size_t global_size[] = {1, 1, 1};
static constexpr size_t local_size[] = {1, 1, 1};

static TestResult run(const SubmitKernelArguments &arguments, Statistics &statistics) {
    MeasurementFields typeSelector(MeasurementUnit::Microseconds, MeasurementType::Cpu);

    if (isNoopRun()) {
        statistics.pushUnitAndType(typeSelector.getUnit(), typeSelector.getType());
        return TestResult::Nooped;
    }

    int kernelExecutionTime = arguments.kernelExecutionTime;

    // Setup
    UrState ur;
    Timer timer;
    cpu_set_t cpuset;
    pthread_t current_thread = pthread_self();

    // Zero out the cpuset mask
    CPU_ZERO(&cpuset);
    // Set the mask to CPU 0
    CPU_SET(1, &cpuset);

    // Set the affinity for the current thread
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        return TestResult::KernelNotFound;
    }

    // Create kernel
    auto spirvModule = FileHelper::loadBinaryFile("api_overhead_benchmark_eat_time.spv");
    if (spirvModule.size() == 0)
        return TestResult::KernelNotFound;

    ur_program_handle_t program;
    EXPECT_UR_RESULT_SUCCESS(urProgramCreateWithIL(ur.context, spirvModule.data(),
                                                   spirvModule.size(), nullptr, &program));

    EXPECT_UR_RESULT_SUCCESS(urProgramBuild(ur.context, program, nullptr));

    const char *kernelName = "eat_time";

    ur_kernel_handle_t kernel;
    EXPECT_UR_RESULT_SUCCESS(urKernelCreate(program, kernelName, &kernel));

    ur_queue_handle_t queue;
    ur_queue_properties_t queueProperties = {};

    if (!arguments.inOrderQueue) {
        queueProperties.flags = UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE;
    }

    EXPECT_UR_RESULT_SUCCESS(urQueueCreate(ur.context, ur.device,
                                           &queueProperties, &queue));

    std::vector<ur_event_handle_t> events(arguments.numKernels);

    // warmup
    for (auto iteration = 0u; iteration < arguments.numKernels; iteration++) {
        EXPECT_UR_RESULT_SUCCESS(urKernelSetArgValue(
            kernel, 0, sizeof(int), nullptr,
            reinterpret_cast<void *>(&kernelExecutionTime)));

        ur_event_handle_t *signalEvent = nullptr;
        if (!arguments.discardEvents) {
            signalEvent = &events[iteration];
        }

        EXPECT_UR_RESULT_SUCCESS(urEnqueueKernelLaunch(
            queue, kernel, n_dimensions, nullptr, &local_size[0],
            &global_size[0], 0, nullptr, signalEvent));
    }

    for (auto &event : events) {
        if (event)
            urEventRelease(event);
    }

    // Benchmark
    for (auto i = 0u; i < arguments.iterations; i++) {
        timer.measureStart();
        for (auto iteration = 0u; iteration < arguments.numKernels; iteration++) {
            EXPECT_UR_RESULT_SUCCESS(urKernelSetArgValue(
                kernel, 0, sizeof(int), nullptr,
                reinterpret_cast<void *>(&kernelExecutionTime)));

            ur_event_handle_t *signalEvent = nullptr;
            if (!arguments.discardEvents) {
                signalEvent = &events[iteration];
            }

            EXPECT_UR_RESULT_SUCCESS(urEnqueueKernelLaunch(
                queue, kernel, n_dimensions, nullptr,
                &local_size[0], &global_size[0], 0, nullptr, signalEvent));
        }

        if (!arguments.measureCompletionTime) {
            timer.measureEnd();
            statistics.pushValue(timer.get(), typeSelector.getUnit(), typeSelector.getType());
        }

        EXPECT_UR_RESULT_SUCCESS(urQueueFinish(queue));

        if (arguments.measureCompletionTime) {
            timer.measureEnd();
            statistics.pushValue(timer.get(), typeSelector.getUnit(), typeSelector.getType());
        }

        for (auto &event : events) {
            if (event)
                urEventRelease(event);
        }
    }

    EXPECT_UR_RESULT_SUCCESS(urQueueRelease(queue));
    EXPECT_UR_RESULT_SUCCESS(urKernelRelease(kernel));
    EXPECT_UR_RESULT_SUCCESS(urProgramRelease(program));

    return TestResult::Success;
}

[[maybe_unused]] static RegisterTestCaseImplementation<SubmitKernel> registerTestCase(run, Api::UR);
