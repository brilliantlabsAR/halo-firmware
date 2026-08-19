/*
 * Copyright (c) 2021-2022 Arm Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "UseCaseHandler.hpp"
#include "AudioBackend.hpp"
#include "KwsClassifier.hpp"
#include "MicroNetKwsModel.hpp"
#include "AudioUtils.hpp"
#include "KwsResult.hpp"
#include "KwsProcessing.hpp"

#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(UseCaseHandler);

using arm::app::ApplicationContext;
using arm::app::ClassificationResult;
using arm::app::KwsClassifier;
using arm::app::KwsPostProcess;
using arm::app::KwsPreProcess;
using arm::app::MicroNetKwsModel;
using arm::app::Model;

#define AUDIO_SAMPLES  CONFIG_AUDIO_SAMPLES
#define AUDIO_STRIDE   CONFIG_AUDIO_STRIDE
#define RESULTS_MEMORY CONFIG_RESULTS_MEMORY

static int16_t audio_inf[AUDIO_SAMPLES + AUDIO_STRIDE];

namespace alif
{
namespace app
{

namespace audio
{
using namespace arm::app::audio;
}

namespace kws
{
using namespace arm::app::kws;
}

/**
 * @brief           Presents KWS inference results.
 * @param[in]       results     Vector of KWS classification results to be displayed.
 * @return          true if successful, false otherwise.
 **/
static bool PresentInferenceResult(const std::vector<arm::app::kws::KwsResult> &results);

/* KWS inference handler. */
bool ClassifyAudioHandler(ApplicationContext &ctx, bool oneshot)
{
	auto &model = ctx.Get<Model &>("model");
	const auto mfccFrameLength = ctx.Get<int>("frameLength");
	const auto mfccFrameStride = ctx.Get<int>("frameStride");
	const auto audioRate = ctx.Get<int>("audioRate");
	const auto scoreThreshold = ctx.Get<float>("scoreThreshold");

	constexpr int minTensorDims = static_cast<int>(
		(MicroNetKwsModel::ms_inputRowsIdx > MicroNetKwsModel::ms_inputColsIdx)
			? MicroNetKwsModel::ms_inputRowsIdx
			: MicroNetKwsModel::ms_inputColsIdx);

	if (!model.IsInited()) {
		LOG_ERR("Model is not initialised! Terminating processing.");
		return false;
	}

	/* Get Input and Output tensors for pre/post processing. */
	TfLiteTensor *inputTensor = model.GetInputTensor(0);
	TfLiteTensor *outputTensor = model.GetOutputTensor(0);
	if (!inputTensor->dims) {
		LOG_ERR("Invalid input tensor dims");
		return false;
	} else if (inputTensor->dims->size < minTensorDims) {
		LOG_ERR("Input tensor dimension should be >= %d", minTensorDims);
		return false;
	}

	/* Get input shape for feature extraction. */
	TfLiteIntArray *inputShape = model.GetInputShape(0);
	const uint32_t numMfccFeatures = inputShape->data[MicroNetKwsModel::ms_inputColsIdx];
	const uint32_t numMfccFrames =
		inputShape->data[arm::app::MicroNetKwsModel::ms_inputRowsIdx];

	/* We expect to be sampling 1 second worth of data at a time.
	 *  NOTE: This is only used for time stamp calculation. */
	const float secondsPerSample = 1.0f / audioRate;

	/* Set up pre and post-processing. */
	KwsPreProcess preProcess = KwsPreProcess(inputTensor, numMfccFeatures, numMfccFrames,
						 mfccFrameLength, mfccFrameStride);

	std::vector<ClassificationResult> singleInfResult;
	KwsPostProcess postProcess =
		KwsPostProcess(outputTensor, ctx.Get<KwsClassifier &>("classifier"),
			       ctx.Get<std::vector<std::string> &>("labels"), singleInfResult);

	int index = 0;
	std::vector<kws::KwsResult> infResults;
	int err = audio_init(audioRate);
	if (err) {
		LOG_ERR("hal_audio_init failed with error: %d", err);
		return false;
	}

	// Start first fill of final stride section of buffer
	get_audio_data(audio_inf + AUDIO_SAMPLES, AUDIO_STRIDE);

	do {
		// Wait until stride buffer is full - initiated above or by previous interation of
		// loop
		int err = wait_for_audio();
		if (err) {
			LOG_ERR("hal_get_audio_data failed with error: %d", err);
			return false;
		}

		// move buffer down by one stride, clearing space at the end for the next stride
		std::copy(audio_inf + AUDIO_STRIDE, audio_inf + AUDIO_STRIDE + AUDIO_SAMPLES,
			  audio_inf);

		// start receiving the next stride immediately before we start heavy processing, so
		// as not to lose anything
		get_audio_data(audio_inf + AUDIO_SAMPLES, AUDIO_STRIDE);

		audio_preprocessing(audio_inf + AUDIO_SAMPLES - AUDIO_STRIDE, AUDIO_STRIDE);

		const int16_t *inferenceWindow = audio_inf;

		uint32_t start = k_cycle_get_32();
		/* Run the pre-processing, inference and post-processing. */
		if (!preProcess.DoPreProcess(inferenceWindow, index)) {
			LOG_ERR("Pre-processing failed.");
			return false;
		}
		LOG_INF("Preprocessing time = %.3f ms",
		       (double)(k_cycle_get_32() - start) / sys_clock_hw_cycles_per_sec() * 1000);

		start = k_cycle_get_32();
		if (!model.RunInference()) {
			LOG_ERR("Inference failed.");
			return false;
		}
		LOG_INF("Inference time = %.3f ms",
		       (double)(k_cycle_get_32() - start) / sys_clock_hw_cycles_per_sec() * 1000);

		start = k_cycle_get_32();
		if (!postProcess.DoPostProcess()) {
			LOG_ERR("Post-processing failed.");
			return false;
		}
		LOG_INF("Postprocessing time = %.3f ms",
		       (double)(k_cycle_get_32() - start) / sys_clock_hw_cycles_per_sec() * 1000);

		/* Add results from this window to our final results vector. */
		if (infResults.size() == RESULTS_MEMORY) {
			infResults.erase(infResults.begin());
		}
		infResults.emplace_back(kws::KwsResult(
			singleInfResult, index * secondsPerSample * preProcess.m_audioDataStride,
			index, scoreThreshold));

#if VERIFY_TEST_OUTPUT
		DumpTensor(outputTensor);
#endif /* VERIFY_TEST_OUTPUT */

		if (!PresentInferenceResult(infResults)) {
			return false;
		}

		++index;
	} while (!oneshot);

	audio_uninit();

	return true;
}

static bool PresentInferenceResult(const std::vector<kws::KwsResult> &results)
{
	LOG_INF("Final results:");
	LOG_INF("Total number of inferences: %zu", results.size());

	for (const auto &result : results) {

		std::string topKeyword{"<none>"};
		float score = 0.f;
		if (!result.m_resultVec.empty()) {
			topKeyword = result.m_resultVec[0].m_label;
			score = result.m_resultVec[0].m_normalisedVal;
		}

		if (result.m_resultVec.empty()) {
			LOG_INF("For timestamp: %f (inference #: %" PRIu32
			     "); label: %s; threshold: %f",
			     (double)result.m_timeStamp, result.m_inferenceNumber,
			     topKeyword.c_str(), (double)result.m_threshold);
		} else {
			for (uint32_t j = 0; j < result.m_resultVec.size(); ++j) {
				LOG_INF("For timestamp: %f (inference #: %" PRIu32
				     "); label: %s, score: %f; threshold: %f",
				     (double)result.m_timeStamp, result.m_inferenceNumber,
				     result.m_resultVec[j].m_label.c_str(),
				     result.m_resultVec[j].m_normalisedVal,
				     (double)result.m_threshold);
			}
		}
	}

	return true;
}

} /* namespace app */
} /* namespace alif */
