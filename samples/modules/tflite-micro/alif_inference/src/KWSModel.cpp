#include <zephyr/logging/log.h>

#include "KWSModel.h"

#include "mfcc/PlatformMath.hpp"
#include "mfcc/MicroNetKwsModel.hpp"
#include "BufAttributes.hpp"

LOG_MODULE_REGISTER(KWSModel);

// By having these as global state prevents us from having multiple instances of KWSModel
// these can be move into the class, but then these must be allocated dynamically
static uint8_t tensorArena[CONFIG_ACTIVATION_BUF_SZ] ACTIVATION_BUF_ATTRIBUTE;

// Full 1 second sample buffer
static int16_t audio_inf[CONFIG_I2S_SAMPLE_RATE];

static const char *labelsVec[] LABELS_ATTRIBUTE = {
	"down",  "go",   "left", "no",  "off",       "on",
	"right", "stop", "up",   "yes", "_silence_", "_unknown_",
};

namespace arm::app::kws
{
extern uint8_t *GetModelPointer();
extern const int g_FrameLength;
extern const int g_FrameStride;
} // namespace arm::app::kws

const char *KWSModel::Result::GetLabelName(size_t index)
{
	return labelsVec[index];
}

bool KWSModel::Init()
{
	m_resolver.AddEthosU();

	m_pInterpreter = std::make_unique<tflite::MicroInterpreter>(
		tflite::GetModel(arm::app::kws::GetModelPointer()), m_resolver, tensorArena,
		CONFIG_ACTIVATION_BUF_SZ);

	const auto rc = m_pInterpreter->AllocateTensors();
	if (rc != kTfLiteOk) {
		LOG_ERR("AllocateTensors failed: %i", rc);
		return false;
	}

	auto *inputTensor = m_pInterpreter->input(0);
	size_t numMfccFeatures =
		inputTensor->dims->data[arm::app::MicroNetKwsModel::ms_inputColsIdx];
	size_t numMfccFrames = inputTensor->dims->data[arm::app::MicroNetKwsModel::ms_inputRowsIdx];
	int mfccFrameLength = arm::app::kws::g_FrameLength;
	int mfccFrameStride = arm::app::kws::g_FrameStride;

	m_preProcess = std::make_unique<arm::app::KwsPreProcess>(
		inputTensor, numMfccFeatures, numMfccFrames, mfccFrameLength, mfccFrameStride);

	return true;
}

bool KWSModel::PreProcess()
{
	if (!m_preProcess->DoPreProcess(audio_inf, m_index)) {
		LOG_ERR("DoPreProcess failed");
		return false;
	}

	++m_index;

	// move buffer down by one stride, clearing space at the end for the next stride
	std::copy(audio_inf + (CONFIG_I2S_SAMPLE_RATE / 2), audio_inf + CONFIG_I2S_SAMPLE_RATE,
		  audio_inf);

	return true;
}

bool KWSModel::RunInference()
{
	const auto rc = m_pInterpreter->Invoke();
	if (rc != kTfLiteOk) {
		LOG_ERR("Invoke failed: %i", rc);
		return false;
	}

	return true;
}

bool KWSModel::PostProcess()
{
	m_output.confidences.clear();

	const auto *tensor = m_pInterpreter->output(0);

	// int8 dequantize, does not work if per-axis quantization is used.
	for (size_t i = 0; i < tensor->bytes; i++) {
		float value = tensor->params.scale * (static_cast<float>(tensor->data.int8[i]) -
						      tensor->params.zero_point);
		m_output.confidences.push_back(value);
	}

	arm::app::math::MathUtils::SoftmaxF32(m_output.confidences);

	return true;
}

void *KWSModel::GetInputBuffer()
{
	// Fill input data to last stride in the buffer
	return &audio_inf[CONFIG_I2S_SAMPLE_RATE / 2];
}

KWSModel::Result KWSModel::GetResult()
{
	return m_output;
}
