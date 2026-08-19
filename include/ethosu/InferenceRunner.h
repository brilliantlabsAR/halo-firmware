#ifndef INFERENCERUNNER_H
#define INFERENCERUNNER_H

#include <zephyr/kernel.h>

/**
 * @brief InferenceRunner: A reusable threaded inference loop for embedded ML using Zephyr RTOS.
 *
 * This class encapsulates the logic of executing a model inference pipeline in a dedicated Zephyr
 * thread. It handles input collection, preprocessing, model execution, postprocessing, and output
 * handling.
 *
 * @tparam Model          ML model class providing inference logic and buffer access.
 *                        Required interface:
 *                        ----------------------------------------
 *                        static constexpr size_t InputSize;    // In bytes
 *                        bool Init();
 *                        bool PreProcess();
 *                        bool RunInference();
 *                        bool PostProcess();
 *                        void* GetInputBuffer(); // Returns pointer to buffer of InputSize bytes
 *                        auto GetResult();       // Returns processed inference result
 *                        ----------------------------------------
 *                        Notes:
 *                        - InputSize must exactly match Input::OutputSize.
 *
 * @tparam Input          Class responsible for supplying raw input data to the model.
 *                        Required interface:
 *                        ----------------------------------------
 *                        static constexpr size_t OutputSize;   // In bytes
 *                        bool Start(); // Begins input data stream or source
 *                        bool Stop();  // Ends input data stream or source
 *                        bool GetInputData(void* buffer); // Blocks until buffer is filled with
 * OutputSize bytes
 *                        ----------------------------------------
 *
 * @tparam OutputHandler  Class responsible for consuming the model's result.
 *                        Required interface:
 *                        ----------------------------------------
 *                        template<typename T>
 *                        class OutputHandler {
 *                            void ProcessOutput(const T& result);
 *                        };
 *                        ----------------------------------------
 *                        Notes:
 *                        - Called in the inference thread context.
 *                        - Must be fast and **non-blocking**.
 *                        - If multi-threaded communication is required, use queues/semaphores.
 *
 * @tparam StackSize      Size of the Zephyr thread's stack (default: 2024 bytes).
 * @tparam ThreadPriority Zephyr thread priority (default: 10).
 *
 * @note Copy/move constructors and assignment operators are disabled due to internal thread/stack
 * ownership.
 * @note The thread starts once via `Start()` and terminates cleanly in the destructor.
 *
 * @example
 *   using MyRunner = InferenceRunner<MyModel, MyInput, MyOutputHandler<MyModel::Result>>;
 *   MyRunner runner;
 *   runner.Start();
 */

template <typename Model, typename Input, typename OutputHandler, size_t StackSize = 2024,
	int ThreadPriority = 10>
class InferenceRunner
{
public:
	static_assert(Model::InputSize == Input::OutputSize);

	InferenceRunner()
	{
		k_sem_init(&m_inferenceSem, 0, 1);
	}

	InferenceRunner(const InferenceRunner &) = delete;
	InferenceRunner &operator=(const InferenceRunner &) = delete;
	InferenceRunner(InferenceRunner &&) = delete;
	InferenceRunner &operator=(InferenceRunner &&) = delete;

	~InferenceRunner()
	{
		m_input.Stop();
		k_sem_give(&m_inferenceSem);
		k_thread_join(&m_inferenceThread, K_FOREVER);
	}

	void Start(void)
	{
		if (m_started) {
			return;
		}

		m_started = true;

		k_thread_create(&m_inferenceThread, m_stack, K_THREAD_STACK_SIZEOF(m_stack),
				ThreadEntry, this, NULL, NULL, ThreadPriority, 0, K_NO_WAIT);
	}

private:
	static void ThreadEntry(void *ctx, void *, void *)
	{
		static_cast<InferenceRunner *>(ctx)->Run();
	}

	void Run(void)
	{
		if (!m_model.Init()) {
			return;
		}

		if (!m_input.Start()) {
			return;
		}

		while (true) {
			if (k_sem_take(&m_inferenceSem, K_NO_WAIT) == 0) {
				break;
			}

			if (!m_input.GetInputData(m_model.GetInputBuffer())) {
				break;
			}

			if (!m_model.PreProcess()) {
				break;
			}

			if (!m_model.RunInference()) {
				break;
			}

			if (!m_model.PostProcess()) {
				break;
			}

			m_outputHandler.ProcessOutput(m_model.GetResult());
		}

		m_input.Stop();
	}

private:
	Model m_model;
	Input m_input;
	OutputHandler m_outputHandler;
	K_KERNEL_STACK_MEMBER(m_stack, StackSize);
	struct k_thread m_inferenceThread;
	struct k_sem m_inferenceSem;
	bool m_started = false;
};

#endif /* INFERENCERUNNER_H */
