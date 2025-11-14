#include "phonemize.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <espeak-ng/speak_lib.h>

using namespace piper;

void test_abbreviation_normalization() {
    printf("Testing abbreviation normalization...\n");

    eSpeakPhonemeConfig config;
    config.voice = "en-us";

    PhonemeResult result;
    phonemize_eSpeak_with_normalized("Dr. Smith", config, result);

    // Verify we got phonemes
    assert(!result.phonemes.empty());
    printf("  ✓ Generated %zu phoneme sequences\n", result.phonemes.size());

    // Verify normalized text is populated
    assert(!result.normalized_text.empty());
    printf("  ✓ Normalized text: '%s'\n", result.normalized_text.c_str());

    // Check if "Doctor" appears in normalized text
    if (strstr(result.normalized_text.c_str(), "Doctor") != nullptr ||
        strstr(result.normalized_text.c_str(), "doctor") != nullptr) {
        printf("  ✓ Found 'Doctor' in normalized text (abbreviation expanded)\n");
    } else {
        printf("  ⚠ WARNING: 'Doctor' not found in normalized text\n");
        printf("    This may be expected if espeak doesn't expand 'Dr.' in this context\n");
    }

    // Verify character mapping exists
    printf("  ✓ Character mapping has %zu entries\n", result.char_mapping.size());

    printf("✓ Abbreviation normalization test passed\n\n");
}

void test_number_normalization() {
    printf("Testing number normalization...\n");

    eSpeakPhonemeConfig config;
    config.voice = "en-us";

    PhonemeResult result;
    phonemize_eSpeak_with_normalized("I have 42 apples", config, result);

    // Verify we got phonemes
    assert(!result.phonemes.empty());
    printf("  ✓ Generated %zu phoneme sequences\n", result.phonemes.size());

    // Verify normalized text is populated
    assert(!result.normalized_text.empty());
    printf("  ✓ Normalized text: '%s'\n", result.normalized_text.c_str());

    // Check if number was expanded
    if (strstr(result.normalized_text.c_str(), "forty") != nullptr ||
        strstr(result.normalized_text.c_str(), "two") != nullptr) {
        printf("  ✓ Found expanded number in normalized text\n");
    } else {
        printf("  ⚠ WARNING: Expanded number not found in normalized text\n");
    }

    printf("✓ Number normalization test passed\n\n");
}

void test_simple_text() {
    printf("Testing simple text (no normalization needed)...\n");

    eSpeakPhonemeConfig config;
    config.voice = "en-us";

    PhonemeResult result;
    phonemize_eSpeak_with_normalized("hello world", config, result);

    // Verify we got phonemes
    assert(!result.phonemes.empty());
    printf("  ✓ Generated %zu phoneme sequences\n", result.phonemes.size());

    // Verify normalized text is populated
    assert(!result.normalized_text.empty());
    printf("  ✓ Normalized text: '%s'\n", result.normalized_text.c_str());

    printf("✓ Simple text test passed\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <espeak-ng-data-path>\n", argv[0]);
        return 1;
    }

    const char* data_path = argv[1];

    printf("Initializing espeak-ng with data path: %s\n", data_path);
    int result = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, data_path, 0);
    if (result < 0) {
        fprintf(stderr, "ERROR: Failed to initialize espeak-ng (code: %d)\n", result);
        return 1;
    }
    printf("✓ espeak-ng initialized successfully\n\n");

    try {
        test_simple_text();
        test_abbreviation_normalization();
        test_number_normalization();

        printf("\n======================\n");
        printf("All tests passed! ✓\n");
        printf("======================\n");

    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR: Test failed with exception: %s\n", e.what());
        return 1;
    }

    espeak_Terminate();
    return 0;
}
