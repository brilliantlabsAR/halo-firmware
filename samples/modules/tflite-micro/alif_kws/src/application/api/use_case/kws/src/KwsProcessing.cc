/*
 * SPDX-FileCopyrightText: Copyright 2022 Arm Limited and/or its affiliates
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
#include "KwsProcessing.hpp"
#include "MicroNetKwsModel.hpp"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(KwsPreProcess);

namespace arm
{
namespace app
{

KwsPreProcess::KwsPreProcess(TfLiteTensor *inputTensor, size_t numFeatures, size_t numMfccFrames,
			     int mfccFrameLength, int mfccFrameStride)
	: m_inputTensor{inputTensor}, m_mfccFrameLength{mfccFrameLength},
	  m_mfccFrameStride{mfccFrameStride}, m_numMfccFrames{numMfccFrames},
	  m_mfcc{audio::MicroNetKwsMFCC(numFeatures, mfccFrameLength)}
{
	this->m_mfcc.Init();

	/* Deduce the data length required for 1 inference from the network parameters. */
	this->m_audioDataWindowSize = this->m_numMfccFrames * this->m_mfccFrameStride +
				      (this->m_mfccFrameLength - this->m_mfccFrameStride);

	/* Creating an MFCC feature sliding window for the data required for 1 inference. */
	this->m_mfccSlidingWindow = audio::SlidingWindow<const int16_t>(
		nullptr, this->m_audioDataWindowSize, this->m_mfccFrameLength,
		this->m_mfccFrameStride);

	/* For longer audio clips we choose to move by half the audio window size
	 * => for a 1 second window size there is an overlap of 0.5 seconds. */
	this->m_audioDataStride = this->m_audioDataWindowSize / 2;

	/* To have the previously calculated features re-usable, stride must be multiple
	 * of MFCC features window stride. Reduce stride through audio if needed. */
	if (0 != this->m_audioDataStride % this->m_mfccFrameStride) {
		this->m_audioDataStride -= this->m_audioDataStride % this->m_mfccFrameStride;
	}

	this->m_numMfccVectorsInAudioStride = this->m_audioDataStride / this->m_mfccFrameStride;

	/* Calculate number of the feature vectors in the window overlap region.
	 * These feature vectors will be reused.*/
	this->m_numReusedMfccVectors =
		this->m_mfccSlidingWindow.TotalStrides() + 1 - this->m_numMfccVectorsInAudioStride;

	/* Construct feature calculation function. */
	this->m_mfccFeatureCalculator = GetFeatureCalculator(this->m_mfcc, this->m_inputTensor,
							     this->m_numReusedMfccVectors);

	if (!this->m_mfccFeatureCalculator) {
		LOG_ERR("Feature calculator not initialized.");
	}
}

bool KwsPreProcess::DoPreProcess(const void *data, size_t inferenceIndex)
{
	if (data == nullptr) {
		LOG_ERR("Data pointer is null");
	}

	/* Set the features sliding window to the new address. */
	auto input = static_cast<const int16_t *>(data);
	this->m_mfccSlidingWindow.Reset(input);

	/* Cache is only usable if we have more than 1 inference to do and it's not the first
	 * inference. */
	bool useCache = inferenceIndex > 0 && this->m_numReusedMfccVectors > 0;

	/* Use a sliding window to calculate MFCC features frame by frame. */
	while (this->m_mfccSlidingWindow.HasNext()) {
		const int16_t *mfccWindow = this->m_mfccSlidingWindow.Next();

		std::vector<int16_t> mfccFrameAudioData =
			std::vector<int16_t>(mfccWindow, mfccWindow + this->m_mfccFrameLength);

		/* Compute features for this window and write them to input tensor. */
		this->m_mfccFeatureCalculator(mfccFrameAudioData, this->m_mfccSlidingWindow.Index(),
					      useCache, this->m_numMfccVectorsInAudioStride);
	}

	LOG_DBG("Input tensor populated");

	return true;
}

/**
 * @brief Generic feature calculator factory.
 *
 * Returns lambda function to compute features using features cache.
 * Real features math is done by a lambda function provided as a parameter.
 * Features are written to input tensor memory.
 *
 * @tparam T                Feature vector type.
 * @param[in] inputTensor   Model input tensor pointer.
 * @param[in] cacheSize     Number of feature vectors to cache. Defined by the sliding window
 * overlap.
 * @param[in] compute       Features calculator function.
 * @return                  Lambda function to compute features.
 */
template <class T>
std::function<void(std::vector<int16_t> &, size_t, bool, size_t)>
KwsPreProcess::FeatureCalc(TfLiteTensor *inputTensor, size_t cacheSize,
			   std::function<std::vector<T>(std::vector<int16_t> &)> compute)
{
	/* Feature cache to be captured by lambda function. */
	static std::vector<std::vector<T>> featureCache = std::vector<std::vector<T>>(cacheSize);

	return [=](std::vector<int16_t> &audioDataWindow, size_t index, bool useCache,
		   size_t featuresOverlapIndex) {
		T *tensorData = tflite::GetTensorData<T>(inputTensor);
		std::vector<T> features;

		/* Reuse features from cache if cache is ready and sliding windows overlap.
		 * Overlap is in the beginning of sliding window with a size of a feature cache. */
		if (useCache && index < featureCache.size()) {
			features = std::move(featureCache[index]);
		} else {
			features = std::move(compute(audioDataWindow));
		}
		auto size = features.size();
		auto sizeBytes = sizeof(T) * size;
		std::memcpy(tensorData + (index * size), features.data(), sizeBytes);

		/* Start renewing cache as soon iteration goes out of the windows overlap. */
		if (index >= featuresOverlapIndex) {
			featureCache[index - featuresOverlapIndex] = std::move(features);
		}
	};
}

template std::function<void(std::vector<int16_t> &, size_t, bool, size_t)>
KwsPreProcess::FeatureCalc<int8_t>(
	TfLiteTensor *inputTensor, size_t cacheSize,
	std::function<std::vector<int8_t>(std::vector<int16_t> &)> compute);

template std::function<void(std::vector<int16_t> &, size_t, bool, size_t)>
KwsPreProcess::FeatureCalc<float>(
	TfLiteTensor *inputTensor, size_t cacheSize,
	std::function<std::vector<float>(std::vector<int16_t> &)> compute);

std::function<void(std::vector<int16_t> &, int, bool, size_t)>
KwsPreProcess::GetFeatureCalculator(audio::MicroNetKwsMFCC &mfcc, TfLiteTensor *inputTensor,
				    size_t cacheSize)
{
	std::function<void(std::vector<int16_t> &, size_t, bool, size_t)> mfccFeatureCalc = nullptr;

	TfLiteQuantization quant = inputTensor->quantization;

	if (kTfLiteAffineQuantization == quant.type) {
		auto *quantParams = (TfLiteAffineQuantization *)quant.params;
		const float quantScale = quantParams->scale->data[0];
		const int quantOffset = quantParams->zero_point->data[0];

		switch (inputTensor->type) {
		case kTfLiteInt8: {
			mfccFeatureCalc = this->FeatureCalc<int8_t>(
				inputTensor, cacheSize,
				[=, &mfcc](std::vector<int16_t> &audioDataWindow) {
					return mfcc.MfccComputeQuant<int8_t>(
						audioDataWindow, quantScale, quantOffset);
				});
			break;
		}
		default:
			LOG_ERR("Tensor type %s not supported",
				   TfLiteTypeGetName(inputTensor->type));
		}
	} else {
		mfccFeatureCalc = this->FeatureCalc<float>(
			inputTensor, cacheSize, [&mfcc](std::vector<int16_t> &audioDataWindow) {
				return mfcc.MfccCompute(audioDataWindow);
			});
	}
	return mfccFeatureCalc;
}

KwsPostProcess::KwsPostProcess(TfLiteTensor *outputTensor, KwsClassifier &classifier,
			       const std::vector<std::string> &labels,
			       std::vector<ClassificationResult> &results,
			       size_t averagingWindowLen)
	: m_outputTensor{outputTensor}, m_kwsClassifier{classifier}, m_labels{labels},
	  m_results{results}
{
	this->m_resultHistory = {averagingWindowLen, std::vector<float>(labels.size())};
}

bool KwsPostProcess::DoPostProcess()
{
	return this->m_kwsClassifier.GetClassificationResults(this->m_outputTensor, this->m_results,
							      this->m_labels, 1, true,
							      this->m_resultHistory);
}

} /* namespace app */
} /* namespace arm */