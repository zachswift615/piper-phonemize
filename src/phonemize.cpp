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

// Represents a single word with its character position and the phonemes it contains
struct WordInfo {
  int32_t text_position;  // Character offset where word starts in source text
  int32_t length;         // Number of characters in the word
  std::vector<size_t> phoneme_indices;  // Indices of phonemes belonging to this word
};

// Thread-local storage for capturing phoneme and word events during synthesis
// NOTE: espeak-ng provides WORD-LEVEL position data, not phoneme-level.
// All phonemes in a word share the same text_position from espeak.
struct PhonemeEventCapture {
  std::vector<int32_t> phoneme_positions;  // text_position for each phoneme (word-level, not character-level)
  std::vector<WordInfo> words;             // Words with their positions and phoneme groupings
  bool capturing = false;
  size_t current_phoneme_index = 0;        // Counter for phonemes as they arrive
};

thread_local PhonemeEventCapture g_phoneme_capture;

// Synthesis callback that captures phoneme and word events from espeak-ng
//
// IMPORTANT: espeak-ng provides WORD-LEVEL position tracking, not phoneme-level.
// - WORD events (type=1) give us the text_position and length (character count) of each word
// - PHONEME events (type=7) give us phonemes, but their text_position is the word's position, not unique per phoneme
//
// Strategy: Group phonemes by word, then assign each phoneme the full word's character range.
static int synth_callback(short *wav, int numsamples, espeak_EVENT *events) {
  if (!g_phoneme_capture.capturing) {
    return 0;
  }

  // DIAGNOSTIC: Callback invoked
  fprintf(stderr, "[PIPER_DEBUG] synth_callback invoked, numsamples=%d\n", numsamples);

  // DIAGNOSTIC: Check if events is NULL
  if (!events) {
    fprintf(stderr, "[PIPER_DEBUG]   ERROR: events pointer is NULL!\n");
    return 0;
  }

  int event_count = 0;
  WordInfo* current_word = nullptr;

  while (events && events->type != espeakEVENT_LIST_TERMINATED) {
    event_count++;

    // DIAGNOSTIC: Log ALL event types
    fprintf(stderr, "[PIPER_DEBUG]   Event #%d: type=%d, text_pos=%d, length=%d\n",
            event_count, events->type, events->text_position, events->length);

    if (events->type == espeakEVENT_WORD) {
      // WORD event: Start of a new word with its position and length
      fprintf(stderr, "[PIPER_DEBUG]   --> WORD event: text_pos=%d, length=%d\n",
              events->text_position, events->length);

      // Create a new word entry
      WordInfo word;
      word.text_position = events->text_position;
      word.length = events->length;
      g_phoneme_capture.words.push_back(word);
      current_word = &g_phoneme_capture.words.back();

    } else if (events->type == espeakEVENT_PHONEME) {
      // PHONEME event: A phoneme belonging to the current word
      fprintf(stderr, "[PIPER_DEBUG]   --> PHONEME event: text_pos=%d (phoneme #%zu)\n",
              events->text_position, g_phoneme_capture.current_phoneme_index);

      // Store the raw position (for debugging/verification purposes)
      g_phoneme_capture.phoneme_positions.push_back(events->text_position);

      // Associate this phoneme with the current word
      if (current_word) {
        current_word->phoneme_indices.push_back(g_phoneme_capture.current_phoneme_index);
        fprintf(stderr, "[PIPER_DEBUG]       -> Assigned to word at pos=%d, len=%d\n",
                current_word->text_position, current_word->length);
      } else {
        // Edge case: Phoneme before first WORD event (shouldn't happen in normal espeak output)
        fprintf(stderr, "[PIPER_DEBUG]       -> WARNING: Phoneme before first WORD event!\n");
      }

      g_phoneme_capture.current_phoneme_index++;
    }

    events++;
  }

  // DIAGNOSTIC: Report total events processed
  fprintf(stderr, "[PIPER_DEBUG]   Processed %d events (%zu words, %zu phonemes)\n",
          event_count, g_phoneme_capture.words.size(),
          g_phoneme_capture.phoneme_positions.size());

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
    g_phoneme_capture.phoneme_positions.clear();
    g_phoneme_capture.words.clear();
    g_phoneme_capture.current_phoneme_index = 0;
    g_phoneme_capture.capturing = true;

    // DIAGNOSTIC: Confirm capture enabled
    fprintf(stderr, "[PIPER_DEBUG] Enabled phoneme capture for clause\n");

    // Synthesize to trigger callbacks (output is ignored)
    int clauseStart = inputTextPointer - textCopy.c_str();
    espeak_Synth(inputTextPointer, strlen(inputTextPointer),
                 0, POS_CHARACTER, 0, espeakCHARS_AUTO | espeakENDPAUSE, NULL, NULL);
    espeak_Synchronize();

    g_phoneme_capture.capturing = false;

    // DIAGNOSTIC: Show captured data
    fprintf(stderr, "[PIPER_DEBUG] Captured %zu words and %zu phoneme positions from espeak\n",
            g_phoneme_capture.words.size(),
            g_phoneme_capture.phoneme_positions.size());

    // DIAGNOSTIC: Show word groupings
    for (size_t i = 0; i < g_phoneme_capture.words.size(); i++) {
      const auto& word = g_phoneme_capture.words[i];
      fprintf(stderr, "[PIPER_DEBUG]   Word #%zu: pos=%d, len=%d, phonemes=[",
              i, word.text_position, word.length);
      for (size_t j = 0; j < word.phoneme_indices.size(); j++) {
        fprintf(stderr, "%zu%s", word.phoneme_indices[j],
                j + 1 < word.phoneme_indices.size() ? ", " : "");
      }
      fprintf(stderr, "]\n");
    }

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

    // Step 1: Add all phonemes to the sentence, tracking which ones came from espeak vs synthetic
    auto phonemeIter = mappedSentPhonemes.begin();
    auto phonemeEnd = mappedSentPhonemes.end();
    size_t phoneme_start_index = sentencePhonemes->size();  // Remember where this clause's phonemes start
    std::vector<size_t> espeak_phoneme_indices;  // Track which sentence indices correspond to espeak phonemes

    if (config.keepLanguageFlags) {
      // No phoneme filter - add all phonemes directly
      while (phonemeIter != phonemeEnd) {
        sentencePhonemes->push_back(*phonemeIter);
        espeak_phoneme_indices.push_back(sentencePhonemes->size() - 1);
        phonemeIter++;
      }
    } else {
      // Filter out (lang) switch (flags)
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
          // Regular phoneme - add it
          sentencePhonemes->push_back(*phonemeIter);
          espeak_phoneme_indices.push_back(sentencePhonemes->size() - 1);
        }

        phonemeIter++;
      }
    }

    // Step 2: Distribute word positions to phonemes
    // Build a lookup table: espeak_phoneme_index -> PhonemePosition
    std::map<size_t, PhonemePosition> position_map;

    for (const auto& word : g_phoneme_capture.words) {
      // Assign this word's full character range to all its phonemes
      PhonemePosition word_pos;
      word_pos.text_position = word.text_position;
      word_pos.length = word.length;

      for (size_t espeak_idx : word.phoneme_indices) {
        position_map[espeak_idx] = word_pos;
      }
    }

    // Step 3: Apply positions to the phonemes we actually added
    // Note: espeak may generate more/fewer phonemes than what we ended up with after filtering/mapping
    for (size_t i = 0; i < espeak_phoneme_indices.size(); i++) {
      PhonemePosition pos;

      if (position_map.count(i) > 0) {
        // We have word position data for this phoneme
        pos = position_map[i];
      } else {
        // No position data (edge case: phoneme before first word, or data mismatch)
        pos.text_position = -1;
        pos.length = 0;
      }

      sentencePositions->push_back(pos);
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
