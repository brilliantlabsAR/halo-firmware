/*
 * SPDX-FileCopyrightText: Copyright 2021-2022 Arm Limited and/or its affiliates <open-source-office@arm.com>
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
#ifndef KWS_RESULT_HPP
#define KWS_RESULT_HPP

#include "ClassificationResult.hpp"

#include <vector>

namespace arm {
namespace app {
namespace kws {

    using ResultVec = std::vector<arm::app::ClassificationResult>;

    /* Structure for holding kws result. */
    class KwsResult {

    public:
        ResultVec       m_resultVec;        /* Container for "thresholded" classification results. */
        float           m_timeStamp;        /* Audio timestamp for this result. */
        uint32_t        m_inferenceNumber;  /* Corresponding inference number. */
        float           m_threshold;        /* Threshold value for `m_resultVec`. */

        KwsResult() = delete;
        KwsResult(ResultVec&        resultVec,
                  const float       timestamp,
                  const uint32_t    inferenceIdx,
                  const float       scoreThreshold) {

            this->m_threshold = scoreThreshold;
            this->m_timeStamp = timestamp;
            this->m_inferenceNumber = inferenceIdx;

            this->m_resultVec = ResultVec();
            for (auto& i : resultVec) {
                if (i.m_normalisedVal >= (double)this->m_threshold) {
                    this->m_resultVec.emplace_back(i);
                }
            }
        }
        ~KwsResult() = default;
    };

} /* namespace kws */
} /* namespace app */
} /* namespace arm */

#endif /* KWS_RESULT_HPP */