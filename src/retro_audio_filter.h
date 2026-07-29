#ifndef RETRO_AUDIO_FILTER_H
#define RETRO_AUDIO_FILTER_H

#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class RetroAudioFilter {
private:
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1, x2, y1, y2;
    };

    Biquad highpass;
    Biquad box_peak;
    Biquad lowpass;
    float drive;
    bool enabled;
    bool use_fbcf;

    inline float fast_soft_clip(float x) {
        return x / (1.0f + fabsf(x));
    }

    inline float process_biquad(Biquad& f, float in) {
        float out = f.b0 * in + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
        f.x2 = f.x1;
        f.x1 = in;
        f.y2 = f.y1;
        f.y1 = out;
        return out;
    }

    void init_highpass(Biquad& f, float freq, float sampleRate);
    void init_lowpass(Biquad& f, float freq, float sampleRate);
    void init_peaking(Biquad& f, float freq, float dbGain, float Q, float sampleRate);
    
    // Plastic Resonance Delay Buffer
    static const int COMB_BUFFER_SIZE = 128;
    float comb_buffer[COMB_BUFFER_SIZE];
    int comb_write_ptr;
    float comb_delay_ms; // Target delay in milliseconds//
    int comb_delay_samples; // ~80 samples @ 44.1kHz = 1.8 ms
    float comb_feedback;    // Amount of resonance decay (~0.35)

public:
    RetroAudioFilter();
    float CombFilter(float x);
    void Init(float sampleRate);
    void SetEnabled(bool state);
    void SetFBCF(bool state);
    bool IsEnabled() const;
    void ResetBuffers();

    // Process a single normalized sample [-1.0f, 1.0f]
    float ProcessSample(float sample_in);

    // Helper to process an array of 16-bit PCM samples in-place
    void ProcessBufferS16(int16_t* buffer, int count);
};

#endif // RETRO_AUDIO_FILTER_H