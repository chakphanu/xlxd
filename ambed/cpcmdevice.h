#ifndef AMBED_PCM_DEVICE_H
#define AMBED_PCM_DEVICE_H
#include "cpcmcodec.h"
#include <string>

/** Open only the explicitly selected one-channel device, or return empty. */
std::unique_ptr<IPcmAmbe> OpenPcmDevice(const std::string &serial,uint8_t codec);
#endif
