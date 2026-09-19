#if !defined(LYRICS_H)
#define LYRICS_H

#include "cbase.h"
#include "pipeline.h"

#if !defined(LYRICS_API)
#if defined(_WIN32)
#define LYRICS_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define LYRICS_API __attribute__((visibility("default")))
#else
#define LYRICS_API
#endif
#endif

LYRICS_API void lyrics_config_init(LrcPipelineConfig *config);
LYRICS_API bool lyrics_extract_vocals(LrcPipelineConfig *config,
                                      LrcVocalsExtractResult *result);
LYRICS_API bool lyrics_generate_lrc(LrcPipelineConfig *config,
                                    LrcPipelineGenerateResult *result);
LYRICS_API int32 lyrics_main(int32 argc, char **argv);

#endif /* LYRICS_H */
