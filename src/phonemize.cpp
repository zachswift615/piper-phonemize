#include <map>
#include <string>
#include <vector>

#include <espeak-ng/speak_lib.h>
#include <onnxruntime_cxx_api.h>

#include "phonemize.hpp"
#include "uni_algo.h"

namespace piper {

// language -> phoneme -> [phoneme, ...]
std::map<std::string, PhonemeMap> DEFAULT_PHONEME_MAP = {
    {"pt-br", {{U'c', {U'k'}}}}};

PIPERPHONEMIZE_EXPORT void
phonemize_eSpeak(std::string text, eSpeakPhonemeConfig &config,
                 std::vector<std::vector<Phoneme>> &phonemes) {

  auto voice = config.voice;
  int result = espeak_SetVoiceByName(voice.c_str());
  if (result != 0) {
    throw std::runtime_error("Failed to set eSpeak-ng voice");
  }

  std::shared_ptr<PhonemeMap> phonemeMap;
  if (config.phonemeMap) {
    phonemeMap = config.phonemeMap;
  } else if (DEFAULT_PHONEME_MAP.count(voice) > 0) {
    phonemeMap = std::make_shared<PhonemeMap>(DEFAULT_PHONEME_MAP[voice]);
  }

  // Modified by eSpeak
  std::string textCopy(text);

  std::vector<Phoneme> *sentencePhonemes = nullptr;
  const char *inputTextPointer = textCopy.c_str();
  int terminator = 0;

  while (inputTextPointer != NULL) {
    // Modified espeak-ng API to get access to clause terminator
    std::string clausePhonemes(espeak_TextToPhonemesWithTerminator(
        (const void **)&inputTextPointer,
        /*textmode*/ espeakCHARS_AUTO,
        /*phonememode = IPA*/ 0x02, &terminator));

    // Decompose, e.g. "ç" -> "c" + "̧"
    auto phonemesNorm = una::norm::to_nfd_utf8(clausePhonemes);
    auto phonemesRange = una::ranges::utf8_view{phonemesNorm};

    if (!sentencePhonemes) {
      // Start new sentence
      phonemes.emplace_back();
      sentencePhonemes = &phonemes[phonemes.size() - 1];
    }

    // Maybe use phoneme map
    std::vector<Phoneme> mappedSentPhonemes;
    if (phonemeMap) {
      for (auto phoneme : phonemesRange) {
        if (phonemeMap->count(phoneme) < 1) {
          // No mapping for phoneme
          mappedSentPhonemes.push_back(phoneme);
        } else {
          // Mapping for phoneme
          auto mappedPhonemes = &(phonemeMap->at(phoneme));
          mappedSentPhonemes.insert(mappedSentPhonemes.end(),
                                    mappedPhonemes->begin(),
                                    mappedPhonemes->end());
        }
      }
    } else {
      // No phoneme map
      mappedSentPhonemes.insert(mappedSentPhonemes.end(), phonemesRange.begin(),
                                phonemesRange.end());
    }

    auto phonemeIter = mappedSentPhonemes.begin();
    auto phonemeEnd = mappedSentPhonemes.end();

    if (config.keepLanguageFlags) {
      // No phoneme filter
      sentencePhonemes->insert(sentencePhonemes->end(), phonemeIter,
                               phonemeEnd);
    } else {
      // Filter out (lang) switch (flags).
      // These surround words from languages other than the current voice.
      bool inLanguageFlag = false;

      while (phonemeIter != phonemeEnd) {
        if (inLanguageFlag) {
          if (*phonemeIter == U')') {
            // End of (lang) switch
            inLanguageFlag = false;
          }
        } else if (*phonemeIter == U'(') {
          // Start of (lang) switch
          inLanguageFlag = true;
        } else {
          sentencePhonemes->push_back(*phonemeIter);
        }

        phonemeIter++;
      }
    }

    // Add appropriate punctuation depending on terminator type
    int punctuation = terminator & 0x000FFFFF;
    if (punctuation == CLAUSE_PERIOD) {
      sentencePhonemes->push_back(config.period);
    } else if (punctuation == CLAUSE_QUESTION) {
      sentencePhonemes->push_back(config.question);
    } else if (punctuation == CLAUSE_EXCLAMATION) {
      sentencePhonemes->push_back(config.exclamation);
    } else if (punctuation == CLAUSE_COMMA) {
      sentencePhonemes->push_back(config.comma);
      sentencePhonemes->push_back(config.space);
    } else if (punctuation == CLAUSE_COLON) {
      sentencePhonemes->push_back(config.colon);
      sentencePhonemes->push_back(config.space);
    } else if (punctuation == CLAUSE_SEMICOLON) {
      sentencePhonemes->push_back(config.semicolon);
      sentencePhonemes->push_back(config.space);
    }

    if ((terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE) {
      // End of sentence
      sentencePhonemes = nullptr;
    }

  } // while inputTextPointer != NULL

} /* phonemize_eSpeak */

// ----------------------------------------------------------------------------
// Position tracking implementation
// ----------------------------------------------------------------------------

// Thread-local storage for capturing phoneme events during synthesis
struct PhonemeEventCapture {
  std::vector<int32_t> positions;
  bool capturing = false;
};

thread_local PhonemeEventCapture g_phoneme_capture;

// Synthesis callback that captures phoneme events from espeak-ng
static int synth_callback(short *wav, int numsamples, espeak_EVENT *events) {
  if (!g_phoneme_capture.capturing) {
    return 0;
  }

  // DIAGNOSTIC: Callback invoked
  fprintf(stderr, "[PIPER_DEBUG] synth_callback invoked, numsamples=%d\n", numsamples);

  while (events && events->type != espeakEVENT_LIST_TERMINATED) {
    if (events->type == espeakEVENT_PHONEME) {
      // DIAGNOSTIC: Phoneme event received
      fprintf(stderr, "[PIPER_DEBUG]   Phoneme event: pos=%d\n", events->text_position);

      // Capture the text position for this phoneme
      g_phoneme_capture.positions.push_back(events->text_position);

      // Store the phoneme name (UTF8 string in id.string)
      // We don't actually need to store these since we get IPA phonemes separately
      // Just tracking positions is enough
    }
    events++;
  }

  return 0;
}

PIPERPHONEMIZE_EXPORT void
phonemize_eSpeak_with_positions(std::string text, eSpeakPhonemeConfig &config,
                                 std::vector<std::vector<Phoneme>> &phonemes,
                                 std::vector<std::vector<PhonemePosition>> &positions) {

  auto voice = config.voice;
  int result = espeak_SetVoiceByName(voice.c_str());
  if (result != 0) {
    throw std::runtime_error("Failed to set eSpeak-ng voice");
  }

  std::shared_ptr<PhonemeMap> phonemeMap;
  if (config.phonemeMap) {
    phonemeMap = config.phonemeMap;
  } else if (DEFAULT_PHONEME_MAP.count(voice) > 0) {
    phonemeMap = std::make_shared<PhonemeMap>(DEFAULT_PHONEME_MAP[voice]);
  }

  // Set up synthesis callback to capture events
  espeak_SetSynthCallback(synth_callback);

  // DIAGNOSTIC: Confirm callback registration
  fprintf(stderr, "[PIPER_DEBUG] Registered synth_callback for position tracking\n");

  // Modified by eSpeak
  std::string textCopy(text);

  std::vector<Phoneme> *sentencePhonemes = nullptr;
  std::vector<PhonemePosition> *sentencePositions = nullptr;
  const char *inputTextPointer = textCopy.c_str();
  int terminator = 0;

  while (inputTextPointer != NULL) {
    // Clear and enable capture for this clause
    g_phoneme_capture.positions.clear();
    g_phoneme_capture.capturing = true;

    // DIAGNOSTIC: Confirm capture enabled
    fprintf(stderr, "[PIPER_DEBUG] Enabled phoneme capture for clause\n");

    // Synthesize to trigger callbacks (output is ignored)
    int clauseStart = inputTextPointer - textCopy.c_str();
    espeak_Synth(inputTextPointer, strlen(inputTextPointer),
                 0, POS_CHARACTER, 0, espeakCHARS_AUTO | espeakENDPAUSE, NULL, NULL);
    espeak_Synchronize();

    g_phoneme_capture.capturing = false;

    // DIAGNOSTIC: Show captured positions
    fprintf(stderr, "[PIPER_DEBUG] Captured %zu positions from espeak\n",
            g_phoneme_capture.positions.size());

    // Get IPA phonemes using the standard API
    std::string clausePhonemes(espeak_TextToPhonemesWithTerminator(
        (const void **)&inputTextPointer,
        /*textmode*/ espeakCHARS_AUTO,
        /*phonememode = IPA*/ 0x02, &terminator));

    // Decompose, e.g. "ç" -> "c" + "̧"
    auto phonemesNorm = una::norm::to_nfd_utf8(clausePhonemes);
    auto phonemesRange = una::ranges::utf8_view{phonemesNorm};

    if (!sentencePhonemes) {
      // Start new sentence
      phonemes.emplace_back();
      positions.emplace_back();
      sentencePhonemes = &phonemes[phonemes.size() - 1];
      sentencePositions = &positions[positions.size() - 1];
    }

    // Maybe use phoneme map
    std::vector<Phoneme> mappedSentPhonemes;
    if (phonemeMap) {
      for (auto phoneme : phonemesRange) {
        if (phonemeMap->count(phoneme) < 1) {
          // No mapping for phoneme
          mappedSentPhonemes.push_back(phoneme);
        } else {
          // Mapping for phoneme
          auto mappedPhonemes = &(phonemeMap->at(phoneme));
          mappedSentPhonemes.insert(mappedSentPhonemes.end(),
                                    mappedPhonemes->begin(),
                                    mappedPhonemes->end());
        }
      }
    } else {
      // No phoneme map
      mappedSentPhonemes.insert(mappedSentPhonemes.end(), phonemesRange.begin(),
                                phonemesRange.end());
    }

    auto phonemeIter = mappedSentPhonemes.begin();
    auto phonemeEnd = mappedSentPhonemes.end();
    size_t posIdx = 0;

    if (config.keepLanguageFlags) {
      // No phoneme filter
      while (phonemeIter != phonemeEnd) {
        sentencePhonemes->push_back(*phonemeIter);

        // Create position info
        PhonemePosition pos;
        if (posIdx < g_phoneme_capture.positions.size()) {
          pos.text_position = g_phoneme_capture.positions[posIdx];
          // Calculate length from next position or use 1 as default
          if (posIdx + 1 < g_phoneme_capture.positions.size()) {
            pos.length = g_phoneme_capture.positions[posIdx + 1] - pos.text_position;
          } else {
            pos.length = 1;
          }
        } else {
          // No position data available
          pos.text_position = -1;
          pos.length = 0;
        }
        sentencePositions->push_back(pos);

        phonemeIter++;
        posIdx++;
      }
    } else {
      // Filter out (lang) switch (flags).
      bool inLanguageFlag = false;

      while (phonemeIter != phonemeEnd) {
        if (inLanguageFlag) {
          if (*phonemeIter == U')') {
            // End of (lang) switch
            inLanguageFlag = false;
          }
        } else if (*phonemeIter == U'(') {
          // Start of (lang) switch
          inLanguageFlag = true;
        } else {
          sentencePhonemes->push_back(*phonemeIter);

          // Create position info
          PhonemePosition pos;
          if (posIdx < g_phoneme_capture.positions.size()) {
            pos.text_position = g_phoneme_capture.positions[posIdx];
            // Calculate length from next position or use 1 as default
            if (posIdx + 1 < g_phoneme_capture.positions.size()) {
              pos.length = g_phoneme_capture.positions[posIdx + 1] - pos.text_position;
            } else {
              pos.length = 1;
            }
          } else {
            // No position data available
            pos.text_position = -1;
            pos.length = 0;
          }
          sentencePositions->push_back(pos);
        }

        phonemeIter++;
        if (!inLanguageFlag && *phonemeIter != U'(') {
          posIdx++;
        }
      }
    }

    // Add appropriate punctuation depending on terminator type
    // Note: Punctuation gets position of -1 since it's synthetic
    PhonemePosition punctPos = {-1, 0};

    int punctuation = terminator & 0x000FFFFF;
    if (punctuation == CLAUSE_PERIOD) {
      sentencePhonemes->push_back(config.period);
      sentencePositions->push_back(punctPos);
    } else if (punctuation == CLAUSE_QUESTION) {
      sentencePhonemes->push_back(config.question);
      sentencePositions->push_back(punctPos);
    } else if (punctuation == CLAUSE_EXCLAMATION) {
      sentencePhonemes->push_back(config.exclamation);
      sentencePositions->push_back(punctPos);
    } else if (punctuation == CLAUSE_COMMA) {
      sentencePhonemes->push_back(config.comma);
      sentencePositions->push_back(punctPos);
      sentencePhonemes->push_back(config.space);
      sentencePositions->push_back(punctPos);
    } else if (punctuation == CLAUSE_COLON) {
      sentencePhonemes->push_back(config.colon);
      sentencePositions->push_back(punctPos);
      sentencePhonemes->push_back(config.space);
      sentencePositions->push_back(punctPos);
    } else if (punctuation == CLAUSE_SEMICOLON) {
      sentencePhonemes->push_back(config.semicolon);
      sentencePositions->push_back(punctPos);
      sentencePhonemes->push_back(config.space);
      sentencePositions->push_back(punctPos);
    }

    if ((terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE) {
      // End of sentence
      sentencePhonemes = nullptr;
      sentencePositions = nullptr;
    }

  } // while inputTextPointer != NULL

  // DIAGNOSTIC: Final phoneme count
  fprintf(stderr, "[PIPER_DEBUG] Returning %zu phoneme sequences with positions\n",
          positions.size());

  // Reset callback
  espeak_SetSynthCallback(NULL);

} /* phonemize_eSpeak_with_positions */

// ----------------------------------------------------------------------------

PIPERPHONEMIZE_EXPORT void
phonemize_codepoints(std::string text, CodepointsPhonemeConfig &config,
                     std::vector<std::vector<Phoneme>> &phonemes) {

  if (config.casing == CASING_LOWER) {
    text = una::cases::to_lowercase_utf8(text);
  } else if (config.casing == CASING_UPPER) {
    text = una::cases::to_uppercase_utf8(text);
  } else if (config.casing == CASING_FOLD) {
    text = una::cases::to_casefold_utf8(text);
  }

  // Decompose, e.g. "ç" -> "c" + "̧"
  auto phonemesNorm = una::norm::to_nfd_utf8(text);
  auto phonemesRange = una::ranges::utf8_view{phonemesNorm};

  // No sentence boundary detection
  phonemes.emplace_back();
  auto sentPhonemes = &phonemes[phonemes.size() - 1];

  if (config.phonemeMap) {
    for (auto phoneme : phonemesRange) {
      if (config.phonemeMap->count(phoneme) < 1) {
        // No mapping for phoneme
        sentPhonemes->push_back(phoneme);
      } else {
        // Mapping for phoneme
        auto mappedPhonemes = &(config.phonemeMap->at(phoneme));
        sentPhonemes->insert(sentPhonemes->end(), mappedPhonemes->begin(),
                             mappedPhonemes->end());
      }
    }
  } else {
    // No phoneme map
    sentPhonemes->insert(sentPhonemes->end(), phonemesRange.begin(),
                         phonemesRange.end());
  }
} // phonemize_text

} // namespace piper
