// whisper.unity ships its Android library for ARM64 only. On a 32-bit (ARMv7)
// build these empty stand-ins let the app link. They are never called there:
// HearSayLiveCaption always sends audio to the Ultra96 Whisper server on 32-bit phones.
#if defined(__arm__) && !defined(__aarch64__)
#include <stddef.h>

void* whisper_init_from_buffer_with_params() { return NULL; }
int whisper_lang_max_id() { return 0; }
int whisper_lang_id() { return -1; }
const char* whisper_lang_str() { return NULL; }
int whisper_token_eot() { return 0; }
const char* whisper_print_system_info() { return "whisper not available on 32-bit Android"; }
void whisper_full_default_params() {}
void whisper_context_default_params() {}
int whisper_full() { return -1; }
int whisper_full_n_segments() { return 0; }
int whisper_full_lang_id() { return -1; }
int whisper_is_multilingual() { return 0; }
long long whisper_full_get_segment_t0() { return 0; }
long long whisper_full_get_segment_t1() { return 0; }
const char* whisper_full_get_segment_text() { return NULL; }
int whisper_full_n_tokens() { return 0; }
const char* whisper_full_get_token_text() { return NULL; }
void whisper_full_get_token_data() {}
void whisper_free() {}
#endif
