#include <iostream>
#include <vector>
#include <espeak-ng/speak_lib.h>
#include "src/phonemize.hpp"
#include "src/uni_algo.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <espeak-ng-data-path>" << std::endl;
    return 1;
  }

  // Initialize espeak
  int result = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, argv[1], 0);
  if (result < 0) {
    std::cerr << "Failed to initialize eSpeak" << std::endl;
    return 1;
  }

  // Set up config
  piper::eSpeakPhonemeConfig config;
  config.voice = "en-us";

  // Test with simple text
  std::string test_text = "I will build it.";
  std::vector<std::vector<piper::Phoneme>> phonemes;
  std::vector<std::vector<piper::PhonemePosition>> positions;

  std::cout << "\n=== Testing: \"" << test_text << "\" ===" << std::endl;
  std::cout << "Expected: 'build' at position 7 (0-indexed) with length 5" << std::endl;
  std::cout << "All phonemes in 'build' should map to text_position=7, length=5\n" << std::endl;

  // Call the position-tracking function
  piper::phonemize_eSpeak_with_positions(test_text, config, phonemes, positions);

  // Print results
  std::cout << "\n=== Results ===" << std::endl;
  for (size_t sent_idx = 0; sent_idx < phonemes.size(); sent_idx++) {
    std::cout << "Sentence " << sent_idx << ":" << std::endl;
    for (size_t phon_idx = 0; phon_idx < phonemes[sent_idx].size(); phon_idx++) {
      auto phoneme = phonemes[sent_idx][phon_idx];
      auto pos = positions[sent_idx][phon_idx];

      // Convert phoneme to UTF-8 for display
      std::u32string phonemeU32;
      phonemeU32 += phoneme;
      std::string phonemeStr = una::utf32to8(phonemeU32);

      std::cout << "  [" << phon_idx << "] '" << phonemeStr << "' -> ";
      std::cout << "pos=" << pos.text_position << ", len=" << pos.length;

      // Show what text this maps to (if valid position)
      if (pos.text_position >= 0 && pos.length > 0 &&
          pos.text_position + pos.length <= (int)test_text.length()) {
        std::string mapped_text = test_text.substr(pos.text_position, pos.length);
        std::cout << " (\"" << mapped_text << "\")";
      }
      std::cout << std::endl;
    }
  }

  espeak_Terminate();
  return 0;
}
