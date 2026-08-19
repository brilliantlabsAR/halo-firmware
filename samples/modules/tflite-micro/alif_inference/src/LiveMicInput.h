#ifndef LIVEMICINPUT_H
#define LIVEMICINPUT_H

#include <cstddef>

class LiveMicInput
{
public:
	/* 0.5 sec stride (in bytes) */
	static constexpr size_t OutputSize = (CONFIG_I2S_SAMPLE_RATE / 2) * sizeof(int16_t);

	bool Start();
	bool Stop();
	bool GetInputData(void *buffer);
};

#endif /* LIVEMICINPUT_H */
