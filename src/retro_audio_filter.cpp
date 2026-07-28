#include "retro_audio_filter.h"

RetroAudioFilter::RetroAudioFilter() 
    : drive(1.25f), 
      enabled(true), 
      use_fbcf(false),
      comb_write_ptr(0), 
      comb_delay_samples(110), //80//110// 80 samples = ~1.8 ms cavity bounce
      comb_feedback(0.65f)    //.35// .25-.55Controls "plasticiness" / ringing
{}

void RetroAudioFilter::init_highpass(Biquad& f, float freq, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * 0.7071f);
    float a0 = 1.0f + alpha;

    f.b0 = ((1.0f + cosw0) / 2.0f) / a0;
    f.b1 = (-(1.0f + cosw0)) / a0;
    f.b2 = ((1.0f + cosw0) / 2.0f) / a0;
    f.a1 = (-2.0f * cosw0) / a0;
    f.a2 = (1.0f - alpha) / a0;
    f.x1 = f.x2 = f.y1 = f.y2 = 0.0f;
}

void RetroAudioFilter::init_lowpass(Biquad& f, float freq, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * 0.7071f);
    float a0 = 1.0f + alpha;

    f.b0 = ((1.0f - cosw0) / 2.0f) / a0;
    f.b1 = (1.0f - cosw0) / a0;
    f.b2 = ((1.0f - cosw0) / 2.0f) / a0;
    f.a1 = (-2.0f * cosw0) / a0;
    f.a2 = (1.0f - alpha) / a0;
    f.x1 = f.x2 = f.y1 = f.y2 = 0.0f;
}

void RetroAudioFilter::init_peaking(Biquad& f, float freq, float dbGain, float Q, float sampleRate) {
    float A = powf(10.0f, dbGain / 40.0f);
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float alpha = sinf(w0) / (2.0f * Q);
    float cosw0 = cosf(w0);
    float a0 = 1.0f + alpha / A;

    f.b0 = (1.0f + alpha * A) / a0;
    f.b1 = (-2.0f * cosw0) / a0;
    f.b2 = (1.0f - alpha * A) / a0;
    f.a1 = (-2.0f * cosw0) / a0;
    f.a2 = (1.0f - alpha / A) / a0;
    f.x1 = f.x2 = f.y1 = f.y2 = 0.0f;
}

void RetroAudioFilter::Init(float sampleRate) {
    init_highpass(highpass, 80.0f, sampleRate);//160
    init_peaking(box_peak, 400.0f, 8.0f, 1.4f, sampleRate);//default 500.0f, 4.5gain, 1.4f Q, 
    init_lowpass(lowpass, 7500.0f, sampleRate);
}

void RetroAudioFilter::SetEnabled(bool state) {
    enabled = state;
    if (enabled) ResetBuffers();
}
void RetroAudioFilter::SetFBCF(bool state) {
    use_fbcf = state;
    if (enabled) ResetBuffers();
}
bool RetroAudioFilter::IsEnabled() const { return enabled; }

void RetroAudioFilter::ResetBuffers() {
    highpass.x1 = highpass.x2 = highpass.y1 = highpass.y2 = 0.0f;
    box_peak.x1 = box_peak.x2 = box_peak.y1 = box_peak.y2 = 0.0f;
    lowpass.x1  = lowpass.x2  = lowpass.y1  = lowpass.y2  = 0.0f;
    for (int i = 0; i < COMB_BUFFER_SIZE; i++) {
        comb_buffer[i] = 0.0f;
    }
}

float RetroAudioFilter::ProcessSample(float sample_in) {
    if (!enabled) return sample_in;
    float x = sample_in * 0.8f;//0.6f; //Apply pre-attenuation pad (~ -4.5 dB) to prevent filter overflow/clipping
    x = fast_soft_clip(x * drive); // Drive / Soft Saturation

    if (use_fbcf){
        // Plastic Cabinet Comb Filter (Short Cavity Bounce)
        int read_ptr = (comb_write_ptr - comb_delay_samples + COMB_BUFFER_SIZE) % COMB_BUFFER_SIZE;
        float delayed_sample = comb_buffer[read_ptr];

        // Store sample + feedback back into ring buffer
        comb_buffer[comb_write_ptr] = x + (delayed_sample * comb_feedback);
        comb_write_ptr = (comb_write_ptr + 1) % COMB_BUFFER_SIZE;

    // Blend the resonant standing wave with the direct signal
    //x = x + (delayed_sample * 0.5f);2:1 ratio blend
    //x = (x * 0.75f) + (delayed_sample * 0.5f); // 3:2 ratio = extra phase cancellation
    //x = (x * 0.5f) + (delayed_sample * 0.5f); // 1:1 ratio = maximum phase cancellation
    //x = (x * 0.7f) + (delayed_sample * 0.3f);// Goldilocks Blend: 70% direct signal, 30% resonant cavity reflection
    x = (x * 0.6f) + (delayed_sample * 0.4f);// Hair More Resonance: 60% direct signal, 40% resonant cavity reflection

    x = process_biquad(highpass, x);
    x = process_biquad(box_peak, x); // +4.5 dB peak at 500 Hz is now safe
    x = process_biquad(lowpass, x);
    return x;
}

void RetroAudioFilter::ProcessBufferS16(int16_t* buffer, int count) {
    if (!enabled) return;
    for (int i = 0; i < count; i++) {
        float s = (float)buffer[i] / 32768.0f;
        s = ProcessSample(s);
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        buffer[i] = (int16_t)(s * 32767.0f);
    }
}