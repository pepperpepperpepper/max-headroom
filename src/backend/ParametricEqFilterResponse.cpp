#include "ParametricEqFilter.h"

#include <algorithm>
#include <cmath>

namespace {
double dbToLin(double db)
{
  return std::pow(10.0, db / 20.0);
}
} // namespace

void ParametricEqFilter::processLocked()
{
  if (!m_filter) {
    return;
  }

  const bool enabled = m_preset.enabled;
  const double pre = enabled ? dbToLin(m_preset.preampDb) : 1.0;

  for (const auto& port : m_ports) {
    void* inPort = port.inPort;
    void* outPort = port.outPort;
    const int channels = std::max(1, port.channels);
    const int base = port.channelBase;
    if (!inPort || !outPort) {
      continue;
    }

    pw_buffer* inBuf = pw_filter_dequeue_buffer(inPort);
    pw_buffer* outBuf = pw_filter_dequeue_buffer(outPort);
    if (!inBuf || !outBuf) {
      if (inBuf) {
        pw_filter_queue_buffer(inPort, inBuf);
      }
      if (outBuf) {
        pw_filter_queue_buffer(outPort, outBuf);
      }
      continue;
    }

    spa_buffer* inSpa = inBuf->buffer;
    spa_buffer* outSpa = outBuf->buffer;
    if (!inSpa || !outSpa || inSpa->n_datas < 1 || outSpa->n_datas < 1) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    spa_data& inD = inSpa->datas[0];
    spa_data& outD = outSpa->datas[0];
    if (!inD.data || !inD.chunk || !outD.data || !outD.chunk) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    const uint32_t inOffset = inD.chunk->offset;
    const uint32_t inSize = inD.chunk->size;
    const uint32_t outOffset = outD.chunk->offset;
    const uint32_t outMax = outD.maxsize;
    if (inOffset + inSize > inD.maxsize || outOffset >= outMax) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    const auto* inF = reinterpret_cast<const float*>(static_cast<const uint8_t*>(inD.data) + inOffset);
    auto* outF = reinterpret_cast<float*>(static_cast<uint8_t*>(outD.data) + outOffset);

    const uint32_t inFrames = inSize / (sizeof(float) * static_cast<uint32_t>(channels));
    const uint32_t outFrames = (outMax - outOffset) / (sizeof(float) * static_cast<uint32_t>(channels));
    const uint32_t frames = std::min(inFrames, outFrames);
    if (frames == 0) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    if (!enabled) {
      const uint32_t samples = frames * static_cast<uint32_t>(channels);
      for (uint32_t i = 0; i < samples; ++i) {
        outF[i] = inF[i];
      }
    } else {
      const int bands = m_preset.bands.size();
      for (uint32_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
          const uint32_t idx = i * static_cast<uint32_t>(channels) + static_cast<uint32_t>(c);
          const int ch = base + c;

          double x = static_cast<double>(inF[idx]) * pre;
          double y = x;
          for (int b = 0; b < bands; ++b) {
            const EqBand band = m_preset.bands[b];
            if (!band.enabled) {
              continue;
            }

            const Biquad coeffs = m_biquads[ch][b];
            BiquadState& st = m_biquadState[ch][b];

            const double out = coeffs.b0 * y + st.z1;
            st.z1 = coeffs.b1 * y - coeffs.a1 * out + st.z2;
            st.z2 = coeffs.b2 * y - coeffs.a2 * out;
            y = out;
          }
          outF[idx] = static_cast<float>(y);
        }
      }
    }

    outD.chunk->offset = outOffset;
    outD.chunk->size = frames * static_cast<uint32_t>(channels) * sizeof(float);
    outD.chunk->stride = static_cast<uint32_t>(channels) * sizeof(float);

    pw_filter_queue_buffer(inPort, inBuf);
    pw_filter_queue_buffer(outPort, outBuf);
  }
}

