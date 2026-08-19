/*
 * SPDX-FileCopyrightText: Copyright 2022 Arm Limited and/or its affiliates <open-source-office@arm.com>
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
#ifndef KWS_PROCESSING_HPP
#define KWS_PROCESSING_HPP

#include "AudioUtils.hpp"
#include "BaseProcessing.hpp"
#include "KwsClassifier.hpp"
#include "MicroNetKwsMfcc.hpp"

#include <functional>

namespace arm {
namespace app {

    /**
     * @brief   Pre-processing class for Keyword Spotting use case.
     *          Implements methods declared by BasePreProcess and anything else needed
     *          to populate input tensors ready for inference.
     */
    class KwsPreProcess : public BasePreProcess {

    public:
        /**
         * @brief       Constructor
         * @param[in]   inputTensor        Pointer to the TFLite Micro input Tensor.
         * @param[in]   numFeatures        How many MFCC features to use.
         * @param[in]   numFeatureFrames   Number of MFCC vectors that need to be calculated
         *                                 for an inference.
         * @param[in]   mfccFrameLength    Number of audio samples used to calculate one set of MFCC values when
         *                                 sliding a window through the audio sample.
         * @param[in]   mfccFrameStride    Number of audio samples between consecutive windows.
         **/
        explicit KwsPreProcess(TfLiteTensor* inputTensor, size_t numFeatures, size_t numFeatureFrames,
                               int mfccFrameLength, int mfccFrameStride);

        /**
         * @brief       Should perform pre-processing of 'raw' input audio data and load it into
         *              TFLite Micro input tensors ready for inference.
         * @param[in]   input      Pointer to the data that pre-processing will work on.
         * @param[in]   inputSize  Size of the input data.
         * @return      true if successful, false otherwise.
         **/
        bool DoPreProcess(const void* input, size_t inferenceIndex = 0) override;

        size_t m_audioDataWindowSize;   /* Amount of audio needed for 1 inference. */
        size_t m_audioDataStride;       /* Amount of audio to stride across if doing >1 inference in longer clips. */

    private:
        TfLiteTensor* m_inputTensor;    /* Model input tensor. */
        const int m_mfccFrameLength;
        const int m_mfccFrameStride;
        const size_t m_numMfccFrames;   /* How many sets of m_numMfccFeats. */

        audio::MicroNetKwsMFCC m_mfcc;
        audio::SlidingWindow<const int16_t> m_mfccSlidingWindow;
        size_t m_numMfccVectorsInAudioStride;
        size_t m_numReusedMfccVectors;
        std::function<void (std::vector<int16_t>&, int, bool, size_t)> m_mfccFeatureCalculator;

        /**
         * @brief Returns a function to perform feature calculation and populates input tensor data with
         * MFCC data.
         *
         * Input tensor data type check is performed to choose correct MFCC feature data type.
         * If tensor has an integer data type then original features are quantised.
         *
         * Warning: MFCC calculator provided as input must have the same life scope as returned function.
         *
         * @param[in]       mfcc          MFCC feature calculator.
         * @param[in,out]   inputTensor   Input tensor pointer to store calculated features.
         * @param[in]       cacheSize     Size of the feature vectors cache (number of feature vectors).
         * @return          Function to be called providing audio sample and sliding window index.
         */
        std::function<void (std::vector<int16_t>&, int, bool, size_t)>
        GetFeatureCalculator(audio::MicroNetKwsMFCC&  mfcc,
                             TfLiteTensor*            inputTensor,
                             size_t                   cacheSize);

        template<class T>
        std::function<void (std::vector<int16_t>&, size_t, bool, size_t)>
        FeatureCalc(TfLiteTensor* inputTensor, size_t cacheSize,
                    std::function<std::vector<T> (std::vector<int16_t>& )> compute);
    };

    /**
     * @brief   Post-processing class for Keyword Spotting use case.
     *          Implements methods declared by BasePostProcess and anything else needed
     *          to populate result vector.
     */
    class KwsPostProcess : public BasePostProcess {

    private:
        TfLiteTensor* m_outputTensor;                      /* Model output tensor. */
        KwsClassifier& m_kwsClassifier;                    /* KWS Classifier object. */
        const std::vector<std::string>& m_labels;          /* KWS Labels. */
        std::vector<ClassificationResult>& m_results;      /* Results vector for a single inference. */
        std::vector<std::vector<float>> m_resultHistory;   /* Store previous results so they can be averaged. */
    public:
        /**
         * @brief           Constructor
         * @param[in]       outputTensor   Pointer to the TFLite Micro output Tensor.
         * @param[in]       classifier     Classifier object used to get top N results from classification.
         * @param[in]       labels         Vector of string labels to identify each output of the model.
         * @param[in/out]   results        Vector of classification results to store decoded outputs.
         **/
        KwsPostProcess(TfLiteTensor* outputTensor, KwsClassifier& classifier,
                       const std::vector<std::string>& labels,
                       std::vector<ClassificationResult>& results, size_t averagingWindowLen = 1);

        /**
         * @brief    Should perform post-processing of the result of inference then
         *           populate KWS result data for any later use.
         * @return   true if successful, false otherwise.
         **/
        bool DoPostProcess() override;
    };

} /* namespace app */
} /* namespace arm */

#endif /* KWS_PROCESSING_HPP */