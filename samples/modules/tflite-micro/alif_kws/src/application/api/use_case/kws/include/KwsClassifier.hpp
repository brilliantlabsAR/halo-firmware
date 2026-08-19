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
#ifndef KWS_CLASSIFIER_HPP
#define KWS_CLASSIFIER_HPP

#include "ClassificationResult.hpp"
#include "TensorFlowLiteMicro.hpp"
#include "Classifier.hpp"

#include <vector>

namespace arm {
namespace app {

    /**
     * @brief   KWS Classifier - a helper class to get certain number of top
     *          results from the output vector from a classification NN.
     *          Allows for averaging of previous results.
     **/
    class KwsClassifier : public Classifier {
    public:

        /**
         * @brief           Gets the top N classification results from the
         *                  output vector.
         * @param[in]       outputTensor   Inference output tensor from an NN model.
         * @param[out]      vecResults     A vector of classification results.
         *                                 populated by this function.
         * @param[in]       labels         Labels vector to match classified classes.
         * @param[in]       topNCount      Number of top classifications to pick. Default is 1.
         * @param[in]       useSoftmax     Whether Softmax normalisation should be applied to output. Default is false.
         * @param[in/out]   resultHistory  History of previous classification results to be updated.
         * @return          true if successful, false otherwise.
         **/
         using Classifier::GetClassificationResults;  /* We are overloading not overriding. */
         bool GetClassificationResults(TfLiteTensor* outputTensor, std::vector<ClassificationResult>& vecResults,
                 const std::vector <std::string>& labels, uint32_t topNCount,
                 bool use_softmax, std::vector<std::vector<float>>& resultHistory);

        /**
         * @brief        Average the given history of results.
         * @param[in]    resultHistory   The history of results to take on average of.
         * @param[out]   averageResult   The calculated average.
         **/
         static void AveragResults(const std::vector<std::vector<float>>& resultHistory,
                 std::vector<float>& averageResult);
    };

} /* namespace app */
} /* namespace arm */

#endif /* KWS_CLASSIFIER_HPP */
