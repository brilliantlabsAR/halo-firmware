#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>
#include <limits>
#include <vector>

#include "KWSModel.h"
#include "LiveMicInput.h"
#include "ethosu/InferenceRunner.h"

LOG_MODULE_REGISTER(main);

template <typename T>
class PrintHighestConfidence
{
public:
	void ProcessOutput(const T &result)
	{
		const auto it =
			std::max_element(result.confidences.begin(), result.confidences.end());
		const auto highest_idx = std::distance(result.confidences.begin(), it);

		LOG_INF("%s: %f", T::Result::GetLabelName(highest_idx),
			static_cast<double>(result.confidences[highest_idx]));
	}
};

int main()
{
	InferenceRunner<KWSModel, LiveMicInput, PrintHighestConfidence<KWSModel::Result>> runner;

	runner.Start();

	while (true) {
		k_usleep(1000);
	}

	return 0;
}
