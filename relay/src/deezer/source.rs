//! DeezerSource: a decoded track exposed as an `AudioSource` for the /stream
//! pipeline (→ ADPCM → Wii U). Holds the whole decoded track in memory and
//! plays it out; a real queue/streaming-decode comes later.

use crate::audio::{AudioSource, CHANNELS, SAMPLE_RATE};

pub struct DeezerSource {
    samples: Vec<f32>, // interleaved stereo @ 44.1k
    pos: usize,
}

impl DeezerSource {
    pub fn new(samples: Vec<f32>) -> Self {
        DeezerSource { samples, pos: 0 }
    }
}

impl AudioSource for DeezerSource {
    fn read(&mut self, out: &mut [f32]) -> usize {
        let n = out.len().min(self.samples.len() - self.pos);
        out[..n].copy_from_slice(&self.samples[self.pos..self.pos + n]);
        self.pos += n;
        n
    }
    fn sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }
    fn channels(&self) -> u16 {
        CHANNELS
    }
    fn seek(&mut self, position_ms: u64) {
        let frame = CHANNELS as usize;
        let mut idx = (position_ms as usize) * SAMPLE_RATE as usize / 1000 * frame;
        idx = idx.min(self.samples.len());
        // align to frame boundary
        self.pos = idx - (idx % frame);
    }
}
