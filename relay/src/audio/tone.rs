//! Built-in test AudioSource: a stereo sine tone. Lets the whole HTTP-chunked
//! PCM pipeline be exercised with curl BEFORE librespot is wired in (milestone
//! ordering in README/ARCHITECTURE). Default build uses this; enable the
//! `librespot` feature for real Spotify audio.

use super::{AudioSource, CHANNELS, SAMPLE_RATE};

pub struct ToneSource {
    phase: f32,
    freq: f32,
    amp: f32,
}

impl ToneSource {
    pub fn new(freq: f32) -> Self {
        ToneSource {
            phase: 0.0,
            freq,
            amp: 0.20,
        }
    }
}

impl Default for ToneSource {
    fn default() -> Self {
        ToneSource::new(440.0)
    }
}

impl AudioSource for ToneSource {
    fn read(&mut self, out: &mut [f32]) -> usize {
        let step = std::f32::consts::TAU * self.freq / SAMPLE_RATE as f32;
        // out is interleaved stereo; advance phase per frame (pair of samples).
        let frames = out.len() / CHANNELS as usize;
        for f in 0..frames {
            let v = self.phase.sin() * self.amp;
            out[f * 2] = v;
            out[f * 2 + 1] = v;
            self.phase += step;
            if self.phase > std::f32::consts::TAU {
                self.phase -= std::f32::consts::TAU;
            }
        }
        frames * CHANNELS as usize
    }
}
