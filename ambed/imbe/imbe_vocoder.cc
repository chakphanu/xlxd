#include "imbe_vocoder_impl.h"
#include "imbe_vocoder.h"
#include <mutex>

namespace {
// Complete public operations share arithmetic flags and synthesis randomness.
// One boundary protects both legacy and PCM callers without changing the math.
std::mutex &operation_mutex()
{
        static std::mutex mutex;
        return mutex;
}
}

imbe_vocoder::imbe_vocoder()
{
        std::lock_guard<std::mutex> guard(operation_mutex());
        Impl = new imbe_vocoder_impl();
}

imbe_vocoder::~imbe_vocoder()
{
        delete Impl;
}

void imbe_vocoder::imbe_encode(int16_t *frame_vector, int16_t *snd)
{
        std::lock_guard<std::mutex> guard(operation_mutex());
        Impl->imbe_encode(frame_vector, snd);
}

void imbe_vocoder::imbe_decode(int16_t *frame_vector, int16_t *snd)
{
        std::lock_guard<std::mutex> guard(operation_mutex());
        Impl->imbe_decode(frame_vector, snd);
}

void imbe_vocoder::encode_4400(int16_t *snd, uint8_t *imbe)
{
        std::lock_guard<std::mutex> guard(operation_mutex());
	Impl->encode_4400(snd, imbe);
}

void imbe_vocoder::decode_4400(int16_t *snd, uint8_t *imbe)
{
        std::lock_guard<std::mutex> guard(operation_mutex());
	Impl->decode_4400(snd, imbe);
}

const IMBE_PARAM* imbe_vocoder::param(void)
{
	return Impl->param();
}
