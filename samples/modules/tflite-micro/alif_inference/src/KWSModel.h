#ifndef KWSMODEL_H
#define KWSMODEL_H

#include <memory>
#include <vector>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

#include "mfcc/KwsProcessing.hpp"

class KWSModel
{
public:
	class Result
	{
		public:
		std::vector<float> confidences;
		static const char *GetLabelName(size_t index);
	};

	/* Input buffer size in bytes (0.5 second of audio samples) */
	static constexpr size_t InputSize = (CONFIG_I2S_SAMPLE_RATE / 2) * sizeof(int16_t);

	bool Init(void);
	bool PreProcess(void);
	bool RunInference(void);
	bool PostProcess(void);
	void *GetInputBuffer(void);
	Result GetResult(void);

private:
	std::unique_ptr<tflite::MicroInterpreter> m_pInterpreter;
	std::unique_ptr<arm::app::KwsPreProcess> m_preProcess;
	tflite::MicroMutableOpResolver<1> m_resolver;
	int m_index = 0;
	Result m_output;
};

#endif
