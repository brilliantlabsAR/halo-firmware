/*
 * SPDX-FileCopyrightText: Copyright 2022-2023 Arm Limited and/or its affiliates
 * <open-source-office@arm.com> SPDX-License-Identifier: Apache-2.0
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
#include "KwsClassifier.hpp"

#include "TensorFlowLiteMicro.hpp"
#include "PlatformMath.hpp"
#include "KwsClassifier.hpp"

#include <vector>
#include <algorithm>
#include <string>
#include <set>
#include <cstdint>
#include <cinttypes>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(KwsClassifier);

namespace arm
{
namespace app
{

bool KwsClassifier::GetClassificationResults(TfLiteTensor *outputTensor,
					     std::vector<ClassificationResult> &vecResults,
					     const std::vector<std::string> &labels,
					     uint32_t topNCount, bool useSoftmax,
					     std::vector<std::vector<float>> &resultHistory)
{
	if (outputTensor == nullptr) {
		LOG_ERR("Output vector is null pointer.");
		return false;
	}

	uint32_t totalOutputSize = 1;
	for (int inputDim = 0; inputDim < outputTensor->dims->size; inputDim++) {
		totalOutputSize *= outputTensor->dims->data[inputDim];
	}

	/* Health check */
	if (totalOutputSize < topNCount) {
		LOG_ERR("Output vector is smaller than %" PRIu32 "", topNCount);
		return false;
	} else if (totalOutputSize != labels.size()) {
		LOG_ERR("Output size doesn't match the labels' size");
		return false;
	} else if (topNCount == 0) {
		LOG_ERR("Top N results cannot be zero");
		return false;
	}

	bool resultState;
	vecResults.clear();

	/* De-Quantize Output Tensor */
	QuantParams quantParams = GetTensorQuantParams(outputTensor);

	/* Floating point tensor data to be populated
	 * NOTE: The assumption here is that the output tensor size isn't too
	 * big and therefore, there's neglibible impact on heap usage. */
	std::vector<float> resultData(totalOutputSize);
	resultData.resize(totalOutputSize);

	/* Populate the floating point buffer */
	switch (outputTensor->type) {
	case kTfLiteUInt8: {
		uint8_t *tensor_buffer = tflite::GetTensorData<uint8_t>(outputTensor);
		for (size_t i = 0; i < totalOutputSize; ++i) {
			resultData[i] = quantParams.scale *
					(static_cast<float>(tensor_buffer[i]) - quantParams.offset);
		}
		break;
	}
	case kTfLiteInt8: {
		int8_t *tensor_buffer = tflite::GetTensorData<int8_t>(outputTensor);
		for (size_t i = 0; i < totalOutputSize; ++i) {
			resultData[i] = quantParams.scale *
					(static_cast<float>(tensor_buffer[i]) - quantParams.offset);
		}
		break;
	}
	case kTfLiteFloat32: {
		float *tensor_buffer = tflite::GetTensorData<float>(outputTensor);
		for (size_t i = 0; i < totalOutputSize; ++i) {
			resultData[i] = tensor_buffer[i];
		}
		break;
	}
	default:
		LOG_ERR("Tensor type %s not supported by classifier",
			   TfLiteTypeGetName(outputTensor->type));
		return false;
	}

	if (useSoftmax) {
		math::MathUtils::SoftmaxF32(resultData);
	}

	/* If keeping track of recent results, update and take an average. */
	if (resultHistory.size() > 1) {
		std::rotate(resultHistory.begin(), resultHistory.begin() + 1, resultHistory.end());
		resultHistory.back() = resultData;
		AveragResults(resultHistory, resultData);
	}

	/* Get the top N results. */
	resultState = GetTopNResults(resultData, vecResults, topNCount, labels);

	if (!resultState) {
		LOG_ERR("Failed to get top N results set");
		return false;
	}

	return true;
}

void app::KwsClassifier::AveragResults(const std::vector<std::vector<float>> &resultHistory,
				       std::vector<float> &averageResult)
{
	/* Compute averages of each class across the window length. */
	float sum;
	for (size_t j = 0; j < averageResult.size(); j++) {
		sum = 0;
		for (size_t i = 0; i < resultHistory.size(); i++) {
			sum += resultHistory[i][j];
		}
		averageResult[j] = (sum / resultHistory.size());
	}
}

} /* namespace app */
} /* namespace arm */