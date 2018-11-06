#include "faasm/faasm.h"

/**
 * Multiplies its array input by two
 */
namespace faasm {
    int exec(FaasmMemory *memory) {
        long inputSize = memory->getInputSize();
        const uint8_t *input = memory->getInput();
        uint8_t output[inputSize];

        for (int i = 0; i < inputSize; i++) {
            output[i] = input[i] * 2;
        }

        memory->setOutput(output, inputSize);

        return 0;
    }
}
